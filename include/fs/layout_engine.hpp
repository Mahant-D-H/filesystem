#pragma once

#include "fs/disk_format.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace fs {

inline constexpr uint64_t kHugePageSize = 2 * 1024 * 1024;

enum class LayoutKind : uint32_t {
    FixedPage = 1,
    AppendOnlySegment = 2,
    DenseArray = 3,
};

// Fixed-size, in-place-update pages for random-access database workloads.
class FixedPageLayout {
public:
    static FixedPageLayout create(BlockDevice& device, const SuperblockDisk& superblock,
                                  uint64_t page_count, uint32_t page_size = kBlockSize);
    static FixedPageLayout open(BlockDevice& device, const SuperblockDisk& superblock, uint64_t inode_number);

    uint64_t inode_number() const noexcept;
    uint64_t page_count() const noexcept;
    uint32_t page_size() const noexcept;
    void read_page(uint64_t page_id, std::span<std::byte> buffer) const;
    void write_page(uint64_t page_id, std::span<const std::byte> buffer);

private:
    FixedPageLayout(BlockDevice& device, SuperblockDisk superblock, InodeDisk inode, uint32_t page_size);
    uint64_t page_offset(uint64_t page_id) const;

    BlockDevice& device_;
    SuperblockDisk superblock_;
    InodeDisk inode_;
    uint32_t page_size_;
};

// A preallocated, sequential-write segment for SSTables and immutable indexes.
class AppendOnlySegment {
public:
    static AppendOnlySegment create(BlockDevice& device, const SuperblockDisk& superblock,
                                    uint64_t capacity_blocks);
    static AppendOnlySegment open(BlockDevice& device, const SuperblockDisk& superblock, uint64_t inode_number);

    uint64_t inode_number() const noexcept;
    uint64_t size() const noexcept;
    uint64_t capacity() const noexcept;
    uint64_t append(std::span<const std::byte> data);

private:
    AppendOnlySegment(BlockDevice& device, SuperblockDisk superblock, InodeDisk inode);

    BlockDevice& device_;
    SuperblockDisk superblock_;
    InodeDisk inode_;
};

// A huge-page-aligned, preallocated dense region for vectors and time series.
class DenseArrayLayout {
public:
    static DenseArrayLayout create(BlockDevice& device, const SuperblockDisk& superblock,
                                   uint64_t capacity_bytes);
    static DenseArrayLayout open(BlockDevice& device, const SuperblockDisk& superblock, uint64_t inode_number);

    uint64_t inode_number() const noexcept;
    uint64_t capacity() const noexcept;
    void read_at(uint64_t offset, std::span<std::byte> buffer) const;
    void write_at(uint64_t offset, std::span<const std::byte> buffer);

private:
    DenseArrayLayout(BlockDevice& device, SuperblockDisk superblock, InodeDisk inode);

    BlockDevice& device_;
    SuperblockDisk superblock_;
    InodeDisk inode_;
};

} // namespace fs
