#include "fs/disk_format.hpp"
#include "fs/bitmap.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <random>
#include <stdexcept>
#include <sys/stat.h>

using namespace std;

namespace fs {
namespace {

uint64_t ceil_div(uint64_t value, uint64_t divisor) {
    return (value + divisor - 1) / divisor;
}

uint64_t checksum(const SuperblockDisk& superblock) {
    const auto* bytes = reinterpret_cast<const byte*>(&superblock);
    uint64_t result = 1469598103934665603ULL;
    for (size_t i = 0; i < offsetof(SuperblockDisk, checksum); ++i) {
        result ^= to_integer<unsigned char>(bytes[i]);
        result *= 1099511628211ULL;
    }
    return result;
}

void write_inode_table_entry(BlockDevice& device, const SuperblockDisk& superblock, uint64_t inode_index, const InodeDisk& inode) {
    const uint64_t inode_offset = inode_index * kInodeSize;
    const uint64_t block_index = superblock.inode_table_start + (inode_offset / kBlockSize);
    const uint64_t block_offset = inode_offset % kBlockSize;
    array<byte, kBlockSize> block {};
    device.read_at(block_index * kBlockSize, block);
    memcpy(block.data() + block_offset, &inode, sizeof(inode));
    device.write_at(block_index * kBlockSize, block);
}

void read_inode_table_entry(const BlockDevice& device, const SuperblockDisk& superblock, uint64_t inode_index, InodeDisk& inode) {
    const uint64_t inode_offset = inode_index * kInodeSize;
    const uint64_t block_index = superblock.inode_table_start + (inode_offset / kBlockSize);
    const uint64_t block_offset = inode_offset % kBlockSize;
    array<byte, kBlockSize> block {};
    device.read_at(block_index * kBlockSize, block);
    memcpy(&inode, block.data() + block_offset, sizeof(inode));
}

bool try_reserve_data_range(Bitmap& bitmap, uint64_t required, uint64_t& start_index) {
    if (required == 0) {
        return true;
    }
    uint64_t run_start = 0;
    uint64_t run_length = 0;
    for (uint64_t index = 0; index < bitmap.bit_count(); ++index) {
        if (!bitmap.get(index)) {
            if (run_length == 0) {
                run_start = index;
            }
            ++run_length;
            if (run_length == required) {
                start_index = run_start;
                for (uint64_t i = run_start; i < run_start + required; ++i) {
                    bitmap.set(i, true);
                }
                return true;
            }
        } else {
            run_length = 0;
        }
    }
    return false;
}

void release_data_range(Bitmap& bitmap, uint64_t start_index, uint64_t length) {
    if (start_index > bitmap.bit_count() || length > bitmap.bit_count() - start_index) {
        throw out_of_range("extent exceeds data bitmap bounds");
    }
    for (uint64_t i = start_index; i < start_index + length; ++i) {
        bitmap.set(i, false);
    }
}

uint64_t data_bitmap_index_to_block(const SuperblockDisk& superblock, uint64_t index) {
    return superblock.data_start + index;
}

void validate_data_extent(const SuperblockDisk& superblock, const ExtentDisk& extent) {
    if (extent.length == 0 || extent.start < superblock.data_start ||
        extent.start - superblock.data_start >= superblock.data_blocks ||
        extent.length > superblock.data_blocks - (extent.start - superblock.data_start)) {
        throw invalid_argument("extent is outside the data region");
    }
}

vector<byte> read_bitmap_blocks(const BlockDevice& device, uint64_t start_block, uint64_t block_count) {
    vector<byte> raw(block_count * kBlockSize, byte{});
    if (!raw.empty()) {
        device.read_at(start_block * kBlockSize, raw);
    }
    return raw;
}

void write_bitmap_blocks(BlockDevice& device, uint64_t start_block, uint64_t block_count,
                         span<const byte> bitmap_bytes) {
    vector<byte> raw(block_count * kBlockSize, byte{});
    if (bitmap_bytes.size() > raw.size()) {
        throw invalid_argument("bitmap does not fit its on-disk allocation");
    }
    copy(bitmap_bytes.begin(), bitmap_bytes.end(), raw.begin());
    if (!raw.empty()) {
        device.write_at(start_block * kBlockSize, raw);
    }
}

} // namespace end

Layout calculate_layout(uint64_t image_size) {
    if (image_size < 16 * kBlockSize) {
        throw invalid_argument("image must be at least 64 KiB");
    }
    if (image_size % kBlockSize != 0) {
        throw invalid_argument("image size must be a multiple of 4 KiB");
    }
    const uint64_t total_blocks = image_size / kBlockSize;
    const uint64_t total_inodes =
        max<uint64_t>(16, image_size / (16 * 1024));
    const uint64_t inode_bitmap_blocks =
        ceil_div(total_inodes, kBlockSize * 8);
    const uint64_t inode_table_blocks =
        ceil_div(total_inodes * kInodeSize, kBlockSize);
    const uint64_t data_bitmap_start = 1 + inode_bitmap_blocks;
    const uint64_t data_bitmap_blocks =
        ceil_div(total_blocks, kBlockSize * 8);
    const uint64_t inode_table_start = data_bitmap_start + data_bitmap_blocks;
    const uint64_t data_start = inode_table_start + inode_table_blocks;

    if (data_start >= total_blocks) {
        throw invalid_argument("image is too small for filesystem metadata");
    }
    return {
        total_blocks,
        total_inodes,
        1,
        inode_bitmap_blocks,
        data_bitmap_start,
        data_bitmap_blocks,
        inode_table_start,
        inode_table_blocks,
        data_start,
        total_blocks - data_start
    };
}

void write_superblock(BlockDevice& device, const SuperblockDisk& input) {
    SuperblockDisk superblock = input;
    superblock.checksum = 0;
    superblock.checksum = checksum(superblock);
    array<byte, kBlockSize> block {};
    memcpy(block.data(), &superblock, sizeof(superblock));
    device.write_at(0, block);
}

SuperblockDisk read_superblock(BlockDevice& device) {
    array<byte, kBlockSize> block {};
    device.read_at(0, block);
    SuperblockDisk superblock {};
    memcpy(&superblock, block.data(), sizeof(superblock));
    validate_superblock(superblock, device.size());
    return superblock;
}

void validate_superblock(const SuperblockDisk& superblock, uint64_t image_size) {
    if (superblock.magic != kSuperblockMagic) {
        throw runtime_error("invalid filesystem magic");
    }
    if (superblock.version != kFormatVersion) {
        throw runtime_error("unsupported filesystem format version");
    }
    if (superblock.block_size != kBlockSize) {
        throw runtime_error("unsupported filesystem block size");
    }
    if (image_size % kBlockSize != 0 ||
        superblock.total_blocks != image_size / kBlockSize ||
        superblock.total_blocks == 0 ||
        superblock.inode_bitmap_start != 1 ||
        superblock.inode_bitmap_blocks == 0 ||
        superblock.data_bitmap_start != superblock.inode_bitmap_start + superblock.inode_bitmap_blocks ||
        superblock.data_bitmap_blocks == 0 ||
        superblock.inode_table_start != superblock.data_bitmap_start + superblock.data_bitmap_blocks ||
        superblock.inode_table_blocks == 0 ||
        superblock.data_start != superblock.inode_table_start + superblock.inode_table_blocks ||
        superblock.data_start >= superblock.total_blocks ||
        superblock.data_blocks != superblock.total_blocks - superblock.data_start ||
        superblock.total_inodes == 0 ||
        superblock.root_inode >= superblock.total_inodes) {
        throw runtime_error("invalid filesystem geometry");
    }
    SuperblockDisk copy = superblock;
    const auto expected = copy.checksum;
    copy.checksum = 0;
    if (checksum(copy) != expected) {
        throw runtime_error("superblock checksum mismatch");
    }
}

InodeDisk read_inode(BlockDevice& device, const SuperblockDisk& superblock, uint64_t index) {
    if (index >= superblock.total_inodes) {
        throw out_of_range("inode index out of range");
    }
    InodeDisk inode {};
    read_inode_table_entry(device, superblock, index, inode);
    return inode;
}

void write_inode(BlockDevice& device, const SuperblockDisk& superblock, const InodeDisk& inode) {
    if (inode.inode_number >= superblock.total_inodes) {
        throw out_of_range("inode number out of range");
    }
    write_inode_table_entry(device, superblock, inode.inode_number, inode);
}

Bitmap read_data_bitmap(const BlockDevice& device, const SuperblockDisk& superblock) {
    const uint64_t byte_count = (superblock.data_blocks + 7) / 8;
    vector<byte> raw = read_bitmap_blocks(device, superblock.data_bitmap_start,
                                          superblock.data_bitmap_blocks);
    Bitmap bitmap(superblock.data_blocks);
    for (size_t i = 0; i < byte_count; ++i) {
        const auto value = to_integer<unsigned char>(raw[i]);
        for (size_t bit_index = 0; bit_index < 8; ++bit_index) {
            const size_t absolute_index = (i * 8) + bit_index;
            if (absolute_index >= superblock.data_blocks) {
                break;
            }
            bitmap.set(absolute_index, (value & (1u << bit_index)) != 0);
        }
    }
    return bitmap;
}

void write_data_bitmap(BlockDevice& device, const SuperblockDisk& superblock, const Bitmap& bitmap) {
    if (bitmap.bit_count() != superblock.data_blocks) {
        throw invalid_argument("data bitmap has the wrong number of bits");
    }
    write_bitmap_blocks(device, superblock.data_bitmap_start, superblock.data_bitmap_blocks,
                        bitmap.bytes());
}

vector<ExtentDisk> read_inode_extents(const BlockDevice& device, const SuperblockDisk& superblock, const InodeDisk& inode) {
    vector<ExtentDisk> extents;
    for (uint64_t i = 0; i < 10; ++i) {
        if (inode.direct_extents[i].length != 0) {
            validate_data_extent(superblock, inode.direct_extents[i]);
            extents.push_back(inode.direct_extents[i]);
        }
    }
    if (inode.single_indirect_block != 0) {
        array<byte, kBlockSize> block {};
        device.read_at(inode.single_indirect_block * kBlockSize, block);
        const auto* table = reinterpret_cast<const ExtentTableBlock*>(block.data());
        const uint32_t count = table->entry_count;
        if (count > kExtentTableEntriesPerBlock) {
            throw runtime_error("invalid inode extent table entry count");
        }
        for (uint32_t i = 0; i < count; ++i) {
            const auto& entry = table->extents[i];
            if (entry.length != 0) {
                validate_data_extent(superblock, entry);
                extents.push_back(entry);
            }
        }
    }
    return extents;
}

void write_inode_extents(BlockDevice& device, const SuperblockDisk& superblock, InodeDisk& inode, const vector<ExtentDisk>& extents) {
    if (extents.size() > 10 + kExtentTableEntriesPerBlock) {
        throw invalid_argument("too many extents for inode metadata");
    }
    for (const auto& extent : extents) {
        validate_data_extent(superblock, extent);
    }

    const uint64_t old_indirect_block = inode.single_indirect_block;
    for (uint64_t i = 0; i < 10; ++i) {
        inode.direct_extents[i] = {};
    }
    inode.single_indirect_block = 0;
    inode.allocated_blocks = 0;

    const uint64_t direct_count = min<uint64_t>(extents.size(), 10);
    for (uint64_t i = 0; i < direct_count; ++i) {
        inode.direct_extents[i] = extents[i];
        inode.allocated_blocks += extents[i].length;
    }

    if (extents.size() > 10) {
        ExtentTableBlock table {};
        table.entry_count = static_cast<uint32_t>(extents.size() - 10);
        for (size_t i = 10; i < extents.size() && i - 10 < kExtentTableEntriesPerBlock; ++i) {
            table.extents[i - 10] = extents[i];
            inode.allocated_blocks += extents[i].length;
        }
        Bitmap bitmap = read_data_bitmap(device, superblock);
        if (old_indirect_block >= superblock.data_start &&
            old_indirect_block - superblock.data_start < superblock.data_blocks) {
            inode.single_indirect_block = old_indirect_block;
        } else {
            uint64_t indirect_index = 0;
            if (!try_reserve_data_range(bitmap, 1, indirect_index)) {
                throw runtime_error("no free blocks for inode indirect extent table");
            }
            inode.single_indirect_block = data_bitmap_index_to_block(superblock, indirect_index);
        }
        array<byte, kBlockSize> block {};
        memcpy(block.data(), &table, sizeof(table));
        device.write_at(inode.single_indirect_block * kBlockSize, block);
        write_data_bitmap(device, superblock, bitmap);
    } else if (old_indirect_block >= superblock.data_start &&
               old_indirect_block - superblock.data_start < superblock.data_blocks) {
        Bitmap bitmap = read_data_bitmap(device, superblock);
        release_data_range(bitmap, old_indirect_block - superblock.data_start, 1);
        write_data_bitmap(device, superblock, bitmap);
    }

    write_inode(device, superblock, inode);
}

vector<ExtentDisk> allocate_inode_extents(BlockDevice& device, const SuperblockDisk& superblock, InodeDisk& inode, uint64_t block_count) {
    if (block_count == 0) {
        return {};
    }

    vector<ExtentDisk> extents = read_inode_extents(device, superblock, inode);
    if (extents.size() == 10 + kExtentTableEntriesPerBlock) {
        throw runtime_error("inode has no remaining extent metadata slots");
    }

    Bitmap bitmap = read_data_bitmap(device, superblock);
    uint64_t start_index = 0;
    if (!try_reserve_data_range(bitmap, block_count, start_index)) {
        throw runtime_error("not enough contiguous blocks available for allocation");
    }

    ExtentDisk allocated{data_bitmap_index_to_block(superblock, start_index), block_count};
    extents.push_back(allocated);
    write_data_bitmap(device, superblock, bitmap);
    write_inode_extents(device, superblock, inode, extents);
    return {allocated};
}

bool release_inode_extents(BlockDevice& device, const SuperblockDisk& superblock, InodeDisk& inode) {
    vector<ExtentDisk> extents = read_inode_extents(device, superblock, inode);
    if (extents.empty()) {
        return true;
    }

    Bitmap bitmap = read_data_bitmap(device, superblock);
    for (const auto& extent : extents) {
        if (extent.start == 0 || extent.length == 0) {
            continue;
        }
        const uint64_t start_index = extent.start - superblock.data_start;
        if (start_index >= superblock.data_blocks) {
            continue;
        }
        release_data_range(bitmap, start_index, extent.length);
    }

    if (inode.single_indirect_block != 0) {
        const uint64_t indirect_index = inode.single_indirect_block - superblock.data_start;
        if (indirect_index < superblock.data_blocks) {
            release_data_range(bitmap, indirect_index, 1);
        }
    }

    write_data_bitmap(device, superblock, bitmap);
    for (auto& extent : inode.direct_extents) {
        extent = {};
    }
    inode.single_indirect_block = 0;
    inode.allocated_blocks = 0;
    write_inode(device, superblock, inode);
    return true;
}

void format_image(BlockDevice& device) {
    const Layout layout = calculate_layout(device.size());
    array<byte, kBlockSize> zeros {};
    for (uint64_t block = 0; block < layout.total_blocks; ++block) {
        device.write_at(block * kBlockSize, zeros);
    }

    SuperblockDisk superblock {};
    superblock.magic = kSuperblockMagic;
    superblock.version = kFormatVersion;
    superblock.block_size = kBlockSize;
    superblock.total_blocks = layout.total_blocks;
    superblock.total_inodes = layout.total_inodes;
    superblock.inode_bitmap_start = layout.inode_bitmap_start;
    superblock.inode_bitmap_blocks = layout.inode_bitmap_blocks;
    superblock.data_bitmap_start = layout.data_bitmap_start;
    superblock.data_bitmap_blocks = layout.data_bitmap_blocks;
    superblock.inode_table_start = layout.inode_table_start;
    superblock.inode_table_blocks = layout.inode_table_blocks;
    superblock.data_start = layout.data_start;
    superblock.data_blocks = layout.data_blocks;
    superblock.root_inode = 0;
    superblock.clean_shutdown = 1;
    mt19937_64 generator(
        static_cast<uint64_t>(
            chrono::steady_clock::now().time_since_epoch().count()));
    for (char& byte : superblock.uuid) {
        byte = static_cast<char>(generator() & 0xff);
    }
    write_superblock(device, superblock);

    Bitmap inode_bitmap(layout.total_inodes);
    inode_bitmap.set(0, true);
    write_bitmap_blocks(device, layout.inode_bitmap_start, layout.inode_bitmap_blocks,
                        inode_bitmap.bytes());

    Bitmap data_bitmap(layout.data_blocks);
    data_bitmap.set(0, true); // Reserve the first data block for the root directory.
    write_bitmap_blocks(device, layout.data_bitmap_start, layout.data_bitmap_blocks,
                        data_bitmap.bytes());

    InodeDisk root {};
    root.inode_number = 0;
    root.file_type = static_cast<uint8_t>(FileType::Directory);
    root.mode = 0755;
    root.uid = 0;
    root.gid = 0;
    root.link_count = 2;
    root.allocated_blocks = 1;
    root.direct_extents[0] = ExtentDisk{layout.data_start, 1};
    const auto now = chrono::system_clock::to_time_t(chrono::system_clock::now());
    root.atime = now;
    root.mtime = now;
    root.ctime = now;
    array<byte, kBlockSize> inode_block {};
    memcpy(inode_block.data(), &root, sizeof(root));
    device.write_at(layout.inode_table_start * kBlockSize, inode_block);

    array<byte, kBlockSize> root_directory {};
    const string contents = ".\n..\n";
    memcpy(root_directory.data(), contents.data(), contents.size());
    device.write_at(layout.data_start * kBlockSize, root_directory);
    device.flush();
}

string file_type_name(FileType type) {
    switch (type) {
    case FileType::Free: return "free";
    case FileType::Regular: return "regular";
    case FileType::Directory: return "directory";
    }
    return "unknown";
}

string uuid_string(const char (&uuid)[16]) {
    static constexpr char digits[] = "0123456789abcdef";
    string result;
    result.reserve(32);
    for (const unsigned char byte : uuid) {
        result.push_back(digits[byte >> 4]);
        result.push_back(digits[byte & 0x0f]);
    }
    return result;
}

} // namespace fs end
