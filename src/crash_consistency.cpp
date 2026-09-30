#include "fs/crash_consistency.hpp"

#include "fs/bitmap.hpp"
#include "fs/wal_format.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <set>
#include <stdexcept>

namespace fs {
namespace {

using detail::WalHeader;
using detail::WalType;
using detail::checksum;
using detail::kWalMagic;
using detail::record_block;

bool valid_header(const WalHeader& header) {
    return header.magic == kWalMagic && header.header_checksum == detail::header_checksum(header);
}

bool valid_target(const SuperblockDisk& superblock, uint64_t block) {
    return block > 1 && block < superblock.total_blocks &&
           (block < superblock.wal_start || block >= superblock.wal_start + superblock.wal_blocks) &&
           (block < superblock.double_write_start ||
            block >= superblock.double_write_start + superblock.double_write_blocks);
}

WalHeader read_header(BlockDevice& device, uint64_t block) {
    std::array<std::byte, kBlockSize> raw{};
    device.read_at(block * kBlockSize, raw);
    WalHeader header{};
    std::memcpy(&header, raw.data(), sizeof(header));
    return header;
}

} // namespace

CrashConsistency::CrashConsistency(BlockDevice& device, SuperblockDisk superblock):
    device_(device), superblock_(superblock) {}

CrashConsistency CrashConsistency::open(BlockDevice& device) {
    CrashConsistency consistency(device, read_superblock(device));
    consistency.superblock_.clean_shutdown = 0;
    write_superblock(device, consistency.superblock_);
    device.flush();
    consistency.recover(true);
    return consistency;
}

CrashConsistency CrashConsistency::open_for_update(BlockDevice& device) {
    CrashConsistency consistency(device, read_superblock(device));
    consistency.superblock_.clean_shutdown = 0;
    write_superblock(device, consistency.superblock_);
    device.flush();
    consistency.recover(false);
    return consistency;
}

CrashConsistency::Transaction CrashConsistency::begin() {
    return Transaction(*this, next_transaction_id_++);
}

void CrashConsistency::Transaction::write_block(uint64_t block_number, std::span<const std::byte> data) {
    if (owner_ == nullptr || committed_) {
        throw std::logic_error("transaction is not active");
    }
    if (data.size() != kBlockSize) {
        throw std::invalid_argument("transactions write exactly one filesystem block");
    }
    if (!valid_target(owner_->superblock_, block_number)) {
        throw std::out_of_range("transaction target is outside writable filesystem regions");
    }
    updates_.push_back({block_number, std::vector<std::byte>(data.begin(), data.end())});
}

void CrashConsistency::Transaction::commit() {
    if (owner_ == nullptr || committed_) {
        throw std::logic_error("transaction is not active");
    }
    owner_->commit(*this);
    committed_ = true;
}

void CrashConsistency::clear_wal() {
    std::array<std::byte, kBlockSize> zeroes{};
    // Recovery starts at the first record, making any stale tail unreachable.
    device_.write_at(superblock_.wal_start * kBlockSize, zeroes);
    device_.flush();
}

void CrashConsistency::commit(Transaction& transaction) {
    if (transaction.updates_.empty()) {
        throw std::invalid_argument("cannot commit an empty transaction");
    }
    const uint64_t required_records = 2 + transaction.updates_.size() * 2;
    if (required_records > superblock_.wal_blocks ||
        transaction.updates_.size() > superblock_.double_write_blocks) {
        throw std::invalid_argument("transaction does not fit in the WAL and double-write regions");
    }
    std::set<uint64_t> targets;
    for (const auto& update : transaction.updates_) {
        if (!valid_target(superblock_, update.block_number)) {
            throw std::out_of_range("transaction target is outside writable filesystem regions");
        }
        if (!targets.insert(update.block_number).second) {
            throw std::invalid_argument("transaction cannot update the same block more than once");
        }
    }

    if (superblock_.last_checkpoint_lsn == std::numeric_limits<uint64_t>::max()) {
        throw std::overflow_error("filesystem checkpoint LSN is exhausted");
    }
    clear_wal();
    const uint64_t lsn = superblock_.last_checkpoint_lsn + 1;
    uint64_t cursor = 0;
    auto write_record = [&](WalHeader header) {
        device_.write_at((superblock_.wal_start + cursor++) * kBlockSize, record_block(header));
    };
    write_record({kWalMagic, static_cast<uint32_t>(WalType::Begin), 0, lsn, transaction.id_});
    for (const auto& update : transaction.updates_) {
        write_record({kWalMagic, static_cast<uint32_t>(WalType::Update), 0, lsn, transaction.id_,
                      update.block_number, checksum(update.data)});
        device_.write_at((superblock_.wal_start + cursor++) * kBlockSize, update.data);
    }
    device_.flush();
    write_record({kWalMagic, static_cast<uint32_t>(WalType::Commit), 0, lsn, transaction.id_,
                  static_cast<uint64_t>(transaction.updates_.size())});
    device_.flush();

    for (size_t index = 0; index < transaction.updates_.size(); ++index) {
        const auto& update = transaction.updates_[index];
        const uint64_t slot = superblock_.double_write_start +
                              (lsn + index) % superblock_.double_write_blocks;
        device_.write_at(slot * kBlockSize, update.data);
        device_.flush();
        device_.write_at(update.block_number * kBlockSize, update.data);
    }
    device_.flush();
    superblock_.last_checkpoint_lsn = lsn;
    write_superblock(device_, superblock_);
    device_.flush();
    clear_wal();
}

void CrashConsistency::recover() {
    recover(true);
}

void CrashConsistency::shutdown() {
    superblock_ = read_superblock(device_);
    device_.flush();
    superblock_.clean_shutdown = 1;
    write_superblock(device_, superblock_);
    device_.flush();
}

void CrashConsistency::recover(bool reclaim_orphans) {
    struct PendingUpdate {
        WalHeader header;
        uint64_t data_block;
    };
    std::vector<PendingUpdate> updates;
    uint64_t transaction_id = 0;
    uint64_t lsn = 0;
    bool begun = false;
    bool committed = false;
    uint64_t index = 0;
    while (index < superblock_.wal_blocks) {
        const WalHeader header = read_header(device_, superblock_.wal_start + index);
        if (!valid_header(header)) {
            break;
        }
        if (!begun) {
            if (header.type != static_cast<uint32_t>(WalType::Begin) ||
                header.transaction_id == 0 || header.lsn == 0) {
                break;
            }
            begun = true;
            transaction_id = header.transaction_id;
            lsn = header.lsn;
            ++index;
            continue;
        }
        if (header.transaction_id != transaction_id || header.lsn != lsn) {
            break;
        }
        if (header.type == static_cast<uint32_t>(WalType::Update) &&
            index + 1 < superblock_.wal_blocks && valid_target(superblock_, header.target_block)) {
            updates.push_back({header, superblock_.wal_start + index + 1});
            index += 2;
            continue;
        }
        if (header.type == static_cast<uint32_t>(WalType::Commit) &&
            header.target_block == updates.size() && !updates.empty()) {
            committed = true;
        }
        break;
    }

    if (committed && lsn > superblock_.last_checkpoint_lsn) {
        for (size_t update_index = 0; update_index < updates.size(); ++update_index) {
            const auto& update = updates[update_index];
            std::array<std::byte, kBlockSize> page{};
            const uint64_t slot = superblock_.double_write_start +
                                  (lsn + update_index) % superblock_.double_write_blocks;
            device_.read_at(slot * kBlockSize, page);
            if (checksum(page) != update.header.page_checksum) {
                device_.read_at(update.data_block * kBlockSize, page);
            }
            if (checksum(page) != update.header.page_checksum) {
                throw std::runtime_error("WAL and double-write page checksums failed during recovery");
            }
            device_.write_at(update.header.target_block * kBlockSize, page);
        }
        device_.flush();
        superblock_.last_checkpoint_lsn = lsn;
        write_superblock(device_, superblock_);
        device_.flush();
    }
    clear_wal();
    if (reclaim_orphans) {
        reclaim_orphaned_extents();
    }
}

void CrashConsistency::reclaim_orphaned_extents() {
    Bitmap expected_inodes(superblock_.total_inodes);
    Bitmap expected_data(superblock_.data_blocks);
    expected_inodes.set(superblock_.root_inode, true);

    for (uint64_t inode_index = 0; inode_index < superblock_.total_inodes; ++inode_index) {
        const InodeDisk inode = read_inode(device_, superblock_, inode_index);
        if (inode.file_type == static_cast<uint8_t>(FileType::Free)) {
            continue;
        }
        if ((inode.file_type != static_cast<uint8_t>(FileType::Regular) &&
             inode.file_type != static_cast<uint8_t>(FileType::Directory)) ||
            inode.inode_number != inode_index) {
            throw std::runtime_error("invalid allocated inode encountered during recovery");
        }
        if (inode.single_indirect_block != 0 &&
            (inode.single_indirect_block < superblock_.data_start ||
             inode.single_indirect_block - superblock_.data_start >= superblock_.data_blocks)) {
            throw std::runtime_error("inode extent table is outside the data region");
        }
        expected_inodes.set(inode_index, true);
        for (const ExtentDisk& extent : read_inode_extents(device_, superblock_, inode)) {
            const uint64_t start = extent.start - superblock_.data_start;
            for (uint64_t block = start; block < start + extent.length; ++block) {
                if (expected_data.get(block)) {
                    throw std::runtime_error("overlapping inode extents encountered during recovery");
                }
                expected_data.set(block, true);
            }
        }
        if (inode.single_indirect_block != 0) {
            const uint64_t indirect = inode.single_indirect_block - superblock_.data_start;
            if (expected_data.get(indirect)) {
                throw std::runtime_error("overlapping inode extent metadata encountered during recovery");
            }
            expected_data.set(indirect, true);
        }
    }

    if (!expected_data.get(0)) {
        expected_data.set(0, true);
    }
    auto repair_bitmap = [&](uint64_t start_block, uint64_t block_count, const Bitmap& expected) {
        std::vector<std::byte> desired(block_count * kBlockSize, std::byte{});
        std::copy(expected.bytes().begin(), expected.bytes().end(), desired.begin());
        for (uint64_t block_index = 0; block_index < block_count; ++block_index) {
            std::array<std::byte, kBlockSize> current{};
            device_.read_at((start_block + block_index) * kBlockSize, current);
            const auto desired_block = std::span<const std::byte>(
                desired.data() + block_index * kBlockSize, kBlockSize);
            if (!std::equal(current.begin(), current.end(), desired_block.begin())) {
                Transaction repair(*this, next_transaction_id_++);
                repair.write_block(start_block + block_index, desired_block);
                commit(repair);
            }
        }
    };

    repair_bitmap(superblock_.inode_bitmap_start, superblock_.inode_bitmap_blocks, expected_inodes);
    repair_bitmap(superblock_.data_bitmap_start, superblock_.data_bitmap_blocks, expected_data);
}

} // namespace fs
