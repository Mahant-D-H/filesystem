#pragma once

#include "fs/disk_format.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace fs {

// A single-writer WAL manager and fixed-size page updates.
class CrashConsistency {
public:
    class Transaction {
    public:
        Transaction() = default;
        Transaction(const Transaction&) = delete;
        Transaction& operator=(const Transaction&) = delete;
        Transaction(Transaction&&) noexcept = default;
        Transaction& operator=(Transaction&&) noexcept = default;

        void write_block(uint64_t block_number, std::span<const std::byte> data);
        void commit();

    private:
        friend class CrashConsistency;
        struct Update { uint64_t block_number; std::vector<std::byte> data; };
        Transaction(CrashConsistency& owner, uint64_t id) : owner_(&owner), id_(id) {}
        CrashConsistency* owner_ = nullptr;
        uint64_t id_ = 0;
        std::vector<Update> updates_;
        bool committed_ = false;
    };

    static CrashConsistency open(BlockDevice& device);
    static CrashConsistency open_for_update(BlockDevice& device);

    Transaction begin();
    void recover();
    void shutdown();
    const SuperblockDisk& superblock() const noexcept { return superblock_; }

private:
    CrashConsistency(BlockDevice& device, SuperblockDisk superblock);
    void commit(Transaction& transaction);
    void clear_wal();
    void recover(bool reclaim_orphans);
    void reclaim_orphaned_extents();

    BlockDevice& device_;
    SuperblockDisk superblock_;
    uint64_t next_transaction_id_ = 1;
};

} // fs end
