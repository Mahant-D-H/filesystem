#include "fs/disk_format.hpp"

#include "fs/bitmap.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <random>
#include <stdexcept>
#include <sys/stat.h>

namespace fs {
namespace {

std::uint64_t ceil_div(std::uint64_t value, std::uint64_t divisor) {
    return (value + divisor - 1) / divisor;
}

std::uint64_t checksum(const SuperblockDisk& superblock) {
    const auto* bytes = reinterpret_cast<const std::byte*>(&superblock);
    std::uint64_t result = 1469598103934665603ULL;
    for (std::size_t i = 0; i < offsetof(SuperblockDisk, checksum); ++i) {
        result ^= std::to_integer<unsigned char>(bytes[i]);
        result *= 1099511628211ULL;
    }
    return result;
}

} // namespace

Layout calculate_layout(std::uint64_t image_size) {
    if (image_size < 16 * kBlockSize) {
        throw std::invalid_argument("image must be at least 64 KiB");
    }
    const std::uint64_t total_blocks = image_size / kBlockSize;
    const std::uint64_t total_inodes =
        std::max<std::uint64_t>(16, image_size / (16 * 1024));
    const std::uint64_t inode_bitmap_blocks =
        ceil_div(total_inodes, kBlockSize * 8);
    const std::uint64_t inode_table_blocks =
        ceil_div(total_inodes * kInodeSize, kBlockSize);
    const std::uint64_t data_bitmap_start = 1 + inode_bitmap_blocks;
    const std::uint64_t data_bitmap_blocks =
        ceil_div(total_blocks, kBlockSize * 8);
    const std::uint64_t inode_table_start = data_bitmap_start + data_bitmap_blocks;
    const std::uint64_t data_start = inode_table_start + inode_table_blocks;

    if (data_start >= total_blocks) {
        throw std::invalid_argument("image is too small for filesystem metadata");
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
    std::array<std::byte, kBlockSize> block {};
    std::memcpy(block.data(), &superblock, sizeof(superblock));
    device.write_at(0, block);
}

SuperblockDisk read_superblock(BlockDevice& device) {
    std::array<std::byte, kBlockSize> block {};
    device.read_at(0, block);
    SuperblockDisk superblock {};
    std::memcpy(&superblock, block.data(), sizeof(superblock));
    validate_superblock(superblock, device.size());
    return superblock;
}

void validate_superblock(const SuperblockDisk& superblock, std::uint64_t image_size) {
    if (superblock.magic != kSuperblockMagic) {
        throw std::runtime_error("invalid filesystem magic");
    }
    if (superblock.version != kFormatVersion) {
        throw std::runtime_error("unsupported filesystem format version");
    }
    if (superblock.block_size != kBlockSize) {
        throw std::runtime_error("unsupported filesystem block size");
    }
    if (superblock.total_blocks != image_size / kBlockSize ||
        superblock.total_blocks == 0 ||
        superblock.data_start >= superblock.total_blocks ||
        superblock.root_inode >= superblock.total_inodes) {
        throw std::runtime_error("invalid filesystem geometry");
    }
    SuperblockDisk copy = superblock;
    const auto expected = copy.checksum;
    copy.checksum = 0;
    if (checksum(copy) != expected) {
        throw std::runtime_error("superblock checksum mismatch");
    }
}

void format_image(BlockDevice& device) {
    const Layout layout = calculate_layout(device.size());
    std::array<std::byte, kBlockSize> zeros {};
    for (std::uint64_t block = 0; block < layout.total_blocks; ++block) {
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
    std::mt19937_64 generator(
        static_cast<std::uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    for (char& byte : superblock.uuid) {
        byte = static_cast<char>(generator() & 0xff);
    }
    write_superblock(device, superblock);

    Bitmap inode_bitmap(layout.total_inodes);
    inode_bitmap.set(0, true);
    device.write_at(layout.inode_bitmap_start * kBlockSize, inode_bitmap.bytes());

    Bitmap data_bitmap(layout.data_blocks);
    data_bitmap.set(0, true); // Reserve the first data block for the root directory.
    device.write_at(layout.data_bitmap_start * kBlockSize, data_bitmap.bytes());

    InodeDisk root {};
    root.inode_number = 0;
    root.file_type = static_cast<std::uint8_t>(FileType::Directory);
    root.mode = 0755;
    root.uid = 0;
    root.gid = 0;
    root.link_count = 2;
    root.allocated_blocks = 1;
    root.direct_blocks[0] = layout.data_start;
    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    root.atime = now;
    root.mtime = now;
    root.ctime = now;
    std::array<std::byte, kBlockSize> inode_block {};
    std::memcpy(inode_block.data(), &root, sizeof(root));
    device.write_at(layout.inode_table_start * kBlockSize, inode_block);

    std::array<std::byte, kBlockSize> root_directory {};
    const std::string contents = ".\n..\n";
    std::memcpy(root_directory.data(), contents.data(), contents.size());
    device.write_at(layout.data_start * kBlockSize, root_directory);
    device.flush();
}

std::string file_type_name(FileType type) {
    switch (type) {
    case FileType::Free: return "free";
    case FileType::Regular: return "regular";
    case FileType::Directory: return "directory";
    }
    return "unknown";
}

std::string uuid_string(const char (&uuid)[16]) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(32);
    for (const unsigned char byte : uuid) {
        result.push_back(digits[byte >> 4]);
        result.push_back(digits[byte & 0x0f]);
    }
    return result;
}

} // namespace fs
