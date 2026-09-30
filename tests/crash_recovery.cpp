#include "fs/block_device.hpp"
#include "fs/crash_consistency.hpp"
#include "fs/disk_format.hpp"
#include "fs/wal_format.hpp"

#include <array>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace std;

namespace {

using Page = array<byte, fs::kBlockSize>;

void wait_for_child(pid_t child, int& status) {
    while (waitpid(child, &status, 0) == -1) {
        if (errno != EINTR) {
            throw runtime_error("waitpid failed");
        }
    }
}

void stage_committed_transaction(const string& image_path,
                                 const fs::SuperblockDisk& superblock,
                                 uint64_t lsn,
                                 uint64_t first_target,
                                 const Page& first_page,
                                 const Page& second_page,
                                 int ready_fd) {
    try {
        auto device = fs::BlockDevice::open(image_path, fs::BlockDevice::Mode::ReadWrite);
        uint64_t wal_cursor = 0;
        const auto write_record = [&](fs::detail::WalHeader header) {
            const auto record = fs::detail::record_block(header);
            device.write_at((superblock.wal_start + wal_cursor++) * fs::kBlockSize, record);
        };

        write_record({fs::detail::kWalMagic,
                      static_cast<uint32_t>(fs::detail::WalType::Begin), 0, lsn, 1});
        const array<uint64_t, 2> targets{first_target, first_target + 1};
        const array<Page, 2> pages{first_page, second_page};
        for (size_t index = 0; index < pages.size(); ++index) {
            write_record({fs::detail::kWalMagic,
                          static_cast<uint32_t>(fs::detail::WalType::Update), 0, lsn, 1,
                          targets[index], fs::detail::checksum(pages[index])});
            device.write_at((superblock.wal_start + wal_cursor++) * fs::kBlockSize, pages[index]);
        }
        device.flush();
        write_record({fs::detail::kWalMagic,
                      static_cast<uint32_t>(fs::detail::WalType::Commit), 0, lsn, 1,
                      pages.size()});
        device.flush();

        // The production commit path has no injection hook, so stage its durable
        // WAL and double-write records directly, then stop before either home write.
        for (size_t index = 0; index < pages.size(); ++index) {
            const uint64_t slot = superblock.double_write_start +
                                  (lsn + index) % superblock.double_write_blocks;
            device.write_at(slot * fs::kBlockSize, pages[index]);
        }
        device.flush();

        const char ready = 'R';
        if (write(ready_fd, &ready, sizeof(ready)) != static_cast<ssize_t>(sizeof(ready))) {
            throw runtime_error("could not notify parent that crash point is ready");
        }
        for (;;) {
            pause();
        }
    } catch (const exception& error) {
        cerr << "crash-recovery child error: " << error.what() << '\n';
        _exit(EXIT_FAILURE);
    }
}

} // namespace

int main() {
    const string image_path = "/tmp/test.img";
    remove(image_path.c_str());

    try {
        fs::SuperblockDisk superblock{};
        {
            auto device = fs::BlockDevice::create(image_path, 2 * 1024 * 1024);
            fs::format_image(device);
            superblock = fs::read_superblock(device);
            auto update_session = fs::CrashConsistency::open_for_update(device);
            if (update_session.superblock().clean_shutdown != 0 || fs::read_superblock(device).clean_shutdown != 0) {
                throw runtime_error("opening for update did not mark the filesystem dirty");
            }
        }

        const uint64_t target = superblock.data_start + 8;

        const Page zero_page{};

        const Page first_page = [] {
            Page page{};
            page[0] = byte{0xc5};
            page[fs::kBlockSize - 1] = byte{0x5c};
            return page;
        }();

        const Page second_page = [] {
            Page page{};
            page[0] = byte{0x39};
            page[fs::kBlockSize / 2] = byte{0xa7};
            return page;
        }();

        {
            auto device = fs::BlockDevice::open(image_path, fs::BlockDevice::Mode::ReadWrite);
            device.write_at(target * fs::kBlockSize, zero_page);
            device.write_at((target + 1) * fs::kBlockSize, zero_page);
            device.flush();
        }

        int ready_pipe[2]{};
        if (pipe(ready_pipe) != 0) {
            throw runtime_error("could not create child synchronization pipe");
        }
        const pid_t child = fork();
        if (child == -1) {
            close(ready_pipe[0]);
            close(ready_pipe[1]);
            throw runtime_error("fork failed");
        }
        if (child == 0) {
            close(ready_pipe[0]);
            stage_committed_transaction(image_path, superblock, superblock.last_checkpoint_lsn + 1, target, first_page, second_page, ready_pipe[1]);
            _exit(EXIT_SUCCESS);
        }

        close(ready_pipe[1]);
        char ready = 0;
        ssize_t bytes_read;
        do {
            bytes_read = read(ready_pipe[0], &ready, sizeof(ready));
        } while(bytes_read == -1 && errno == EINTR);

        close(ready_pipe[0]);

        if (bytes_read != static_cast<ssize_t>(sizeof(ready)) || ready != 'R') {
            kill(child, SIGKILL);
            int child_status = 0;
            wait_for_child(child, child_status);
            throw runtime_error("child did not reach the intended crash point");
        }
        if (kill(child, SIGKILL) != 0) {
            int child_status = 0;
            wait_for_child(child, child_status);
            throw runtime_error("could not kill child at the intended crash point");
        }
        int child_status = 0;
        wait_for_child(child, child_status);
        if (!WIFSIGNALED(child_status) || WTERMSIG(child_status) != SIGKILL) {
            throw runtime_error("child was not killed at the intended crash point");
        }

        {
            auto device = fs::BlockDevice::open(image_path, fs::BlockDevice::Mode::ReadWrite);
            Page readback{};
            device.read_at(target * fs::kBlockSize, readback);
            if (readback != zero_page) {
                throw runtime_error("crash fixture wrote the first home block prematurely");
            }
            device.read_at((target + 1) * fs::kBlockSize, readback);
            if (readback != zero_page) {
                throw runtime_error("crash fixture wrote the second home block prematurely");
            }
        }

        {
            auto device = fs::BlockDevice::open(image_path, fs::BlockDevice::Mode::ReadWrite);
            auto recovered = fs::CrashConsistency::open(device);
            Page readback{};
            device.read_at(target * fs::kBlockSize, readback);
            if (readback != first_page) {
                throw runtime_error("recovery did not replay the first committed page");
            }
            device.read_at((target + 1) * fs::kBlockSize, readback);
            if (readback != second_page) {
                throw runtime_error("recovery did not replay the second committed page");
            }

            const auto recovered_superblock = recovered.superblock();
            if (recovered_superblock.last_checkpoint_lsn <= superblock.last_checkpoint_lsn ||
                recovered_superblock.wal_start + recovered_superblock.wal_blocks != recovered_superblock.double_write_start ||
                recovered_superblock.double_write_start + recovered_superblock.double_write_blocks != recovered_superblock.data_start) {
                throw runtime_error("recovery left filesystem layout or checkpoint invariants invalid");
            }
            const auto root = fs::read_inode(device, recovered_superblock, recovered_superblock.root_inode);
            const auto inode_bitmap = fs::read_inode_bitmap(device, recovered_superblock);
            const auto data_bitmap = fs::read_data_bitmap(device, recovered_superblock);
            if (root.inode_number != recovered_superblock.root_inode ||
                root.file_type != static_cast<uint8_t>(fs::FileType::Directory) ||
                !inode_bitmap.get(recovered_superblock.root_inode) ||
                !data_bitmap.get(0)) {
                throw runtime_error("recovery left root inode or allocation bitmap invariants invalid");
            }

            const uint64_t checkpoint_lsn = recovered_superblock.last_checkpoint_lsn;
            recovered.recover();
            if (recovered.superblock().last_checkpoint_lsn != checkpoint_lsn) {
                throw runtime_error("recovery was not idempotent after replay");
            }
            recovered.shutdown();
            if (fs::read_superblock(device).clean_shutdown != 1) {
                throw runtime_error("clean shutdown marker was not persisted");
            }
        }

        remove(image_path.c_str());
        cout << "crash recovery test success: killed process after WAL commit and double-write staging (replay and invariants verified)\n";
        return EXIT_SUCCESS;
    } catch (const exception& error) {
        remove(image_path.c_str());
        cerr << "error: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
