#include "fs/block_device.hpp"
#include "fs/crash_consistency.hpp"
#include "fs/disk_format.hpp"
#include "fs/wal_format.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unistd.h>

namespace {

using Page = std::array<std::byte, fs::kBlockSize>;

class TempImage {
public:
    TempImage() {
        std::array<char, 40> path{};
        const std::string pattern = "/tmp/filesystem-wal-XXXXXX";
        std::copy(pattern.begin(), pattern.end(), path.begin());
        const int fd = mkstemp(path.data());
        if (fd < 0) {
            throw std::system_error(errno, std::generic_category(), "mkstemp");
        }
        close(fd);
        path_ = path.data();
    }

    ~TempImage() {
        std::remove(path_.c_str());
    }

    const std::string& path() const noexcept {
        return path_;
    }

private:
    std::string path_;
};

void write_record(fs::BlockDevice& device, uint64_t wal_start, uint64_t& cursor,
                  fs::detail::WalHeader header) {
    const auto record = fs::detail::record_block(header);
    device.write_at((wal_start + cursor++) * fs::kBlockSize, record);
}

} // namespace

int main() {
    try {
        TempImage image;
        auto device = fs::BlockDevice::create(image.path(), 2 * 1024 * 1024);
        fs::format_image(device);
        auto consistency = fs::CrashConsistency::open(device);
        const auto superblock = consistency.superblock();
        const uint64_t target = superblock.data_start + 12;
        const uint64_t base_lsn = superblock.last_checkpoint_lsn;
        const Page zero_page{};
        Page readback{};

        uint64_t cursor = 0;
        const uint64_t incomplete_lsn = base_lsn + 1;
        const uint64_t incomplete_tx = 71;
        Page incomplete_page{};
        incomplete_page[0] = std::byte{0x41};
        write_record(device, superblock.wal_start, cursor,
                     {fs::detail::kWalMagic, static_cast<uint32_t>(fs::detail::WalType::Begin),
                      0, incomplete_lsn, incomplete_tx});
        write_record(device, superblock.wal_start, cursor,
                     {fs::detail::kWalMagic, static_cast<uint32_t>(fs::detail::WalType::Update),
                      0, incomplete_lsn, incomplete_tx, target,
                      fs::detail::checksum(incomplete_page)});
        device.write_at((superblock.wal_start + cursor++) * fs::kBlockSize, incomplete_page);
        device.flush();

        consistency.recover();
        device.read_at(target * fs::kBlockSize, readback);
        if (readback != zero_page ||
            consistency.superblock().last_checkpoint_lsn != base_lsn) {
            throw std::runtime_error("recovery replayed an incomplete, uncommitted transaction");
        }

        cursor = 0;
        const uint64_t corrupt_lsn = base_lsn + 1;
        const uint64_t corrupt_tx = 72;
        Page expected_page{};
        expected_page[0] = std::byte{0xe2};
        Page corrupt_wal_page{};
        corrupt_wal_page[0] = std::byte{0x19};
        Page corrupt_double_write_page{};
        corrupt_double_write_page[0] = std::byte{0x27};
        write_record(device, superblock.wal_start, cursor,
                     {fs::detail::kWalMagic, static_cast<uint32_t>(fs::detail::WalType::Begin),
                      0, corrupt_lsn, corrupt_tx});
        write_record(device, superblock.wal_start, cursor,
                     {fs::detail::kWalMagic, static_cast<uint32_t>(fs::detail::WalType::Update),
                      0, corrupt_lsn, corrupt_tx, target,
                      fs::detail::checksum(expected_page)});
        device.write_at((superblock.wal_start + cursor++) * fs::kBlockSize, corrupt_wal_page);
        write_record(device, superblock.wal_start, cursor,
                     {fs::detail::kWalMagic, static_cast<uint32_t>(fs::detail::WalType::Commit),
                      0, corrupt_lsn, corrupt_tx, 1});
        const uint64_t slot = superblock.double_write_start +
                              corrupt_lsn % superblock.double_write_blocks;
        device.write_at(slot * fs::kBlockSize, corrupt_double_write_page);
        device.flush();

        bool corrupt_pages_rejected = false;
        try {
            consistency.recover();
        } catch (const std::runtime_error& error) {
            corrupt_pages_rejected =
                std::string(error.what()).find("checksums failed") != std::string::npos;
        }
        device.read_at(target * fs::kBlockSize, readback);
        if (!corrupt_pages_rejected || readback != zero_page ||
            consistency.superblock().last_checkpoint_lsn != base_lsn) {
            throw std::runtime_error("recovery accepted corrupt committed page images or modified home state");
        }

        std::cout << "WAL recovery edge tests passed: incomplete transaction ignored and corrupt page images rejected\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "WAL recovery edge test error: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
