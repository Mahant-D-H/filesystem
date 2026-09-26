#include "fs/block_device.hpp"
#include "fs/crash_consistency.hpp"
#include "fs/wal_format.hpp"
#include "fs/disk_format.hpp"

#include <array>
#include <cstdlib>
#include <iostream>

using namespace std;

int main() {
    constexpr const char* image_path = "/tmp/demo.img";
    remove(image_path);
    try {
        auto device = fs::BlockDevice::create(image_path, 2 * 1024 * 1024);
        fs::format_image(device);
        auto consistency = fs::CrashConsistency::open(device);
        const auto superblock = consistency.superblock();
        if (superblock.clean_shutdown != 0 || superblock.wal_blocks < 4 ||
            superblock.double_write_blocks == 0 ||
            superblock.wal_start + superblock.wal_blocks != superblock.double_write_start ||
            superblock.double_write_start + superblock.double_write_blocks != superblock.data_start) {
            throw runtime_error("WAL or double-write regions are missing from the filesystem layout");
        }
        const auto target = superblock.data_start + 8;

        array<byte, fs::kBlockSize> page{};
        page[0] = byte{0xc5};
        page[fs::kBlockSize - 1] = byte{0x5c};
        array<byte, fs::kBlockSize> second_page{};
        second_page[0] = byte{0x39};
        auto transaction = consistency.begin();
        transaction.write_block(target, page);
        transaction.write_block(target + 1, second_page);
        transaction.commit();

        array<byte, fs::kBlockSize> readback{};
        device.read_at(target * fs::kBlockSize, readback);
        if (readback != page || consistency.superblock().last_checkpoint_lsn != 1) {
            throw runtime_error("committed page was not durable");
        }
        device.read_at((target + 1) * fs::kBlockSize, readback);
        if (readback != second_page) {
            throw runtime_error("multi-block transaction did not persist every update");
        }
        const uint64_t double_write_slot =
            superblock.double_write_start + (consistency.superblock().last_checkpoint_lsn % superblock.double_write_blocks);
        device.read_at(double_write_slot * fs::kBlockSize, readback);
        if (readback != page) {
            throw runtime_error("double-write staging did not retain the first updated page");
        }

        array<byte, fs::kBlockSize> untouched{};
        const uint64_t recovery_target = target + 3;
        array<byte, fs::kBlockSize> redo_page{};
        redo_page[0] = byte{0xe1};
        redo_page[fs::kBlockSize - 1] = byte{0x1e};
        const uint64_t recovery_lsn = consistency.superblock().last_checkpoint_lsn + 1;
        uint64_t wal_cursor = 0;
        const auto write_record = [&](fs::detail::WalHeader header) {
            const auto record = fs::detail::record_block(header);
            device.write_at((superblock.wal_start + wal_cursor++) * fs::kBlockSize, record);
        };
        write_record({fs::detail::kWalMagic, static_cast<uint32_t>(fs::detail::WalType::Begin), 0, recovery_lsn, 99});
        write_record({fs::detail::kWalMagic, static_cast<uint32_t>(fs::detail::WalType::Update), 0, recovery_lsn, 99, recovery_target, fs::detail::checksum(redo_page)});
        device.write_at((superblock.wal_start + wal_cursor++) * fs::kBlockSize, redo_page);
        write_record({fs::detail::kWalMagic, static_cast<uint32_t>(fs::detail::WalType::Commit), 0, recovery_lsn, 99, 1});
        const uint64_t recovery_slot = superblock.double_write_start + recovery_lsn % superblock.double_write_blocks;
        device.write_at(recovery_slot * fs::kBlockSize, untouched);
        device.flush();
        device.read_at(recovery_target * fs::kBlockSize, readback);
        if (readback != untouched) {
            throw runtime_error("recovery fixture unexpectedly modified its home block");
        }
        consistency.recover();
        device.read_at(recovery_target * fs::kBlockSize, readback);
        if (readback != redo_page || consistency.superblock().last_checkpoint_lsn != recovery_lsn) {
            throw runtime_error("recovery did not replay a committed WAL page image");
        }

        auto abandoned = consistency.begin();
        abandoned.write_block(target + 2, page);
        device.read_at((target + 2) * fs::kBlockSize, readback);
        if (readback != untouched) {
            throw runtime_error("uncommitted transaction changed its home block");
        }
        auto empty = consistency.begin();
        const auto expect_failure = [](auto&& operation) {
            try {
                operation();
            } catch (const exception&) {
                return;
            }
            throw runtime_error("invalid transaction operation unexpectedly succeeded");
        };
        expect_failure([&] { empty.commit(); });
        expect_failure([&] {
            auto invalid = consistency.begin();
            invalid.write_block(superblock.wal_start, page);
        });

        auto orphaned_data = fs::read_data_bitmap(device, superblock);
        orphaned_data.set(5, true);
        fs::write_data_bitmap(device, superblock, orphaned_data);
        auto orphaned_inodes = fs::read_inode_bitmap(device, superblock);
        orphaned_inodes.set(1, true);
        fs::write_inode_bitmap(device, superblock, orphaned_inodes);

        consistency.shutdown();
        // Mount recovery replays/checkpoints and remove allocations without inode owners.
        auto remounted = fs::CrashConsistency::open(device);
        device.read_at(target * fs::kBlockSize, readback);
        if (readback != page || fs::read_data_bitmap(device, superblock).get(5) ||
            fs::read_inode_bitmap(device, superblock).get(1) ||
            remounted.superblock().last_checkpoint_lsn <= 1 ||
            remounted.superblock().clean_shutdown != 0) {
            throw runtime_error("recovery failed to preserve committed data or reclaim orphan allocations");
        }

        auto recovered_again = fs::CrashConsistency::open(device);
        device.read_at(target * fs::kBlockSize, readback);
        if (readback != page || recovered_again.superblock().last_checkpoint_lsn !=
                                  remounted.superblock().last_checkpoint_lsn) {
            throw runtime_error("recovery is not idempotent after checkpointing");
        }
        recovered_again.shutdown();
        array<byte, fs::kBlockSize> damaged_superblock{};
        device.write_at(0, damaged_superblock);
        const auto backup_recovered = fs::read_superblock(device);
        if (backup_recovered.last_checkpoint_lsn != remounted.superblock().last_checkpoint_lsn ||
            backup_recovered.clean_shutdown != 1) {
            throw runtime_error("backup superblock did not recover a damaged primary copy");
        }
        fs::write_superblock(device, backup_recovered);
        device.write_at(fs::kBlockSize, damaged_superblock);
        const auto primary_recovered = fs::read_superblock(device);
        if (primary_recovered.last_checkpoint_lsn != remounted.superblock().last_checkpoint_lsn) {
            throw runtime_error("primary superblock did not recover a damaged backup copy");
        }
        fs::write_superblock(device, primary_recovered);
        remove(image_path);
        cout << "crash consistency demo: WAL transactions, double-write staging, orphan reclamation, and recovery verified\n";
        return EXIT_SUCCESS;
    } catch (const exception& error) {
        remove(image_path);
        cerr << "crash consistency demo: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
