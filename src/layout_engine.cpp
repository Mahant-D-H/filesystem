#include "fs/layout_engine.hpp"

#include <chrono>
#include <limits>
#include <stdexcept>

namespace fs {
namespace {

constexpr uint32_t kLayoutFlagPrefix = 0x46530000U; // "FS" layout metadata.

uint32_t layout_flags(LayoutKind kind) {
    return kLayoutFlagPrefix | static_cast<uint32_t>(kind);
}

void require_layout(const InodeDisk& inode, LayoutKind kind) {
    if (inode.file_type != static_cast<uint8_t>(FileType::Regular) || inode.flags != layout_flags(kind)) {
        throw std::runtime_error("inode does not contain the requested layout type");
    }
}

ExtentDisk single_extent(const BlockDevice& device, const SuperblockDisk& superblock, const InodeDisk& inode) {
    const auto extents = read_inode_extents(device, superblock, inode);
    if (extents.size() != 1) {
        throw std::runtime_error("layout requires one contiguous extent");
    }
    return extents.front();
}

uint64_t checked_multiply(uint64_t left, uint64_t right, const char* message) {
    if (left != 0 && right > numeric_limits<uint64_t>::max() / left) {
        throw std::overflow_error(message);
    }
    return left * right;
}

void set_modified_time(InodeDisk& inode) {
    inode.mtime = chrono::system_clock::to_time_t(chrono::system_clock::now());
    inode.ctime = inode.mtime;
}

} // namespace

FixedPageLayout::FixedPageLayout(BlockDevice& device, SuperblockDisk superblock, InodeDisk inode,
                                 uint32_t page_size):
    device_(device), superblock_(superblock), inode_(inode), page_size_(page_size) {}

FixedPageLayout FixedPageLayout::create(BlockDevice& device, const SuperblockDisk& superblock,
                                        uint64_t page_count, uint32_t page_size) {
    if (page_count == 0 || page_size < kBlockSize || page_size % kBlockSize != 0) {
        throw invalid_argument("fixed-page layout requires a non-zero page count and a 4 KiB multiple page size");
    }
    const uint64_t blocks = checked_multiply(page_count, page_size / kBlockSize,
                                             "fixed-page layout is too large");
    InodeDisk inode = allocate_inode(device, superblock, FileType::Regular);
    try {
        allocate_inode_extents(device, superblock, inode, blocks);
        inode.size = checked_multiply(page_count, page_size, "fixed-page layout is too large");
        inode.flags = layout_flags(LayoutKind::FixedPage);
        inode.reserved1 = page_size;
        write_inode(device, superblock, inode);
    } catch (...) {
        release_inode(device, superblock, inode);
        throw;
    }
    return FixedPageLayout(device, superblock, inode, page_size);
}

FixedPageLayout FixedPageLayout::open(BlockDevice& device, const SuperblockDisk& superblock, uint64_t inode_number) {
    InodeDisk inode = read_inode(device, superblock, inode_number);
    require_layout(inode, LayoutKind::FixedPage);
    if (inode.reserved1 < kBlockSize || inode.reserved1 % kBlockSize != 0 || inode.size % inode.reserved1 != 0) {
        throw runtime_error("invalid fixed-page layout geometry");
    }
    single_extent(device, superblock, inode);
    return FixedPageLayout(device, superblock, inode, inode.reserved1);
}

uint64_t FixedPageLayout::inode_number() const noexcept { return inode_.inode_number; }
uint64_t FixedPageLayout::page_count() const noexcept { return inode_.size / page_size_; }
uint32_t FixedPageLayout::page_size() const noexcept { return page_size_; }

uint64_t FixedPageLayout::page_offset(uint64_t page_id) const {
    if (page_id >= page_count()) {
        throw out_of_range("fixed-page id is outside the layout");
    }
    const ExtentDisk extent = single_extent(device_, superblock_, inode_);
    return extent.start * kBlockSize + page_id * page_size_;
}

void FixedPageLayout::read_page(uint64_t page_id, span<byte> buffer) const {
    if (buffer.size() != page_size_) {
        throw invalid_argument("fixed-page read buffer has the wrong size");
    }
    device_.read_at(page_offset(page_id), buffer);
}

void FixedPageLayout::write_page(uint64_t page_id, span<const byte> buffer) {
    if (buffer.size() != page_size_) {
        throw invalid_argument("fixed-page write buffer has the wrong size");
    }
    device_.write_at(page_offset(page_id), buffer);
    set_modified_time(inode_);
    write_inode(device_, superblock_, inode_);
}

AppendOnlySegment::AppendOnlySegment(BlockDevice& device, SuperblockDisk superblock, InodeDisk inode):
    device_(device), superblock_(superblock), inode_(inode) {}

AppendOnlySegment AppendOnlySegment::create(BlockDevice& device, const SuperblockDisk& superblock,
                                            uint64_t capacity_blocks) {
    if (capacity_blocks == 0) {
        throw invalid_argument("append-only segment capacity must be non-zero");
    }
    InodeDisk inode = allocate_inode(device, superblock, FileType::Regular);
    try {
        allocate_inode_extents(device, superblock, inode, capacity_blocks);
        inode.size = 0;
        inode.flags = layout_flags(LayoutKind::AppendOnlySegment);
        write_inode(device, superblock, inode);
    } catch (...) {
        release_inode(device, superblock, inode);
        throw;
    }
    return AppendOnlySegment(device, superblock, inode);
}

AppendOnlySegment AppendOnlySegment::open(BlockDevice& device, const SuperblockDisk& superblock, uint64_t inode_number) {
    InodeDisk inode = read_inode(device, superblock, inode_number);
    require_layout(inode, LayoutKind::AppendOnlySegment);
    if (inode.size > checked_multiply(inode.allocated_blocks, kBlockSize, "segment capacity overflow")) {
        throw runtime_error("append-only segment size exceeds its capacity");
    }
    single_extent(device, superblock, inode);
    return AppendOnlySegment(device, superblock, inode);
}

uint64_t AppendOnlySegment::inode_number() const noexcept { return inode_.inode_number; }
uint64_t AppendOnlySegment::size() const noexcept { return inode_.size; }
uint64_t AppendOnlySegment::capacity() const noexcept { return inode_.allocated_blocks * kBlockSize; }

uint64_t AppendOnlySegment::append(span<const byte> data) {
    if (data.empty() || data.size() % kBlockSize != 0) {
        throw invalid_argument("append data must be a non-empty multiple of 4 KiB");
    }
    if (data.size() > capacity() - inode_.size) {
        throw out_of_range("append exceeds segment capacity");
    }
    const ExtentDisk extent = single_extent(device_, superblock_, inode_);
    const uint64_t offset = inode_.size;
    device_.write_at(extent.start * kBlockSize + offset, data);
    inode_.size += data.size();
    set_modified_time(inode_);
    write_inode(device_, superblock_, inode_);
    return offset;
}

DenseArrayLayout::DenseArrayLayout(BlockDevice& device, SuperblockDisk superblock, InodeDisk inode):
    device_(device), superblock_(superblock), inode_(inode) {}

DenseArrayLayout DenseArrayLayout::create(BlockDevice& device, const SuperblockDisk& superblock,
                                          uint64_t capacity_bytes) {
    if (capacity_bytes == 0 || capacity_bytes % kHugePageSize != 0) {
        throw invalid_argument("dense array capacity must be a non-zero multiple of 2 MiB");
    }
    const uint64_t blocks = capacity_bytes / kBlockSize;
    const uint64_t alignment_blocks = kHugePageSize / kBlockSize;
    InodeDisk inode = allocate_inode(device, superblock, FileType::Regular);
    try {
        allocate_inode_extents_aligned(device, superblock, inode, blocks, alignment_blocks);
        inode.size = capacity_bytes;
        inode.flags = layout_flags(LayoutKind::DenseArray);
        write_inode(device, superblock, inode);
    } catch (...) {
        release_inode(device, superblock, inode);
        throw;
    }
    return DenseArrayLayout(device, superblock, inode);
}

DenseArrayLayout DenseArrayLayout::open(BlockDevice& device, const SuperblockDisk& superblock, uint64_t inode_number) {
    InodeDisk inode = read_inode(device, superblock, inode_number);
    require_layout(inode, LayoutKind::DenseArray);
    const ExtentDisk extent = single_extent(device, superblock, inode);
    if (inode.size != extent.length * kBlockSize || inode.size % kHugePageSize != 0 ||
        extent.start % (kHugePageSize / kBlockSize) != 0) {
        throw runtime_error("invalid dense-array layout geometry");
    }
    return DenseArrayLayout(device, superblock, inode);
}

uint64_t DenseArrayLayout::inode_number() const noexcept { return inode_.inode_number; }
uint64_t DenseArrayLayout::capacity() const noexcept { return inode_.size; }

void DenseArrayLayout::read_at(uint64_t offset, span<byte> buffer) const {
    if (offset % kBlockSize != 0 || buffer.empty() || buffer.size() % kBlockSize != 0 ||
        offset > capacity() || buffer.size() > capacity() - offset) {
        throw invalid_argument("dense-array reads must be in-bounds 4 KiB multiples");
    }
    const ExtentDisk extent = single_extent(device_, superblock_, inode_);
    device_.read_at(extent.start * kBlockSize + offset, buffer);
}

void DenseArrayLayout::write_at(uint64_t offset, span<const byte> buffer) {
    if (offset % kBlockSize != 0 || buffer.empty() || buffer.size() % kBlockSize != 0 ||
        offset > capacity() || buffer.size() > capacity() - offset) {
        throw invalid_argument("dense-array writes must be in-bounds 4 KiB multiples");
    }
    const ExtentDisk extent = single_extent(device_, superblock_, inode_);
    device_.write_at(extent.start * kBlockSize + offset, buffer);
    set_modified_time(inode_);
    write_inode(device_, superblock_, inode_);
}

} // namespace fs
