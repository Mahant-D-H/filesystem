#pragma once

#include "fs/block_device.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

namespace fs {

inline constexpr std::uint32_t kBlockSize = 4096;
inline constexpr std::uint32_t kFormatVersion = 1;
inline constexpr std::uint64_t kSuperblockMagic = 0x315359534653ULL;
inline constexpr std::uint32_t kInodeSize = 256;
inline constexpr std::uint32_t kDefaultInodesPer16KiB = 1;

enum class FileType : std::uint8_t {
    Free = 0,
    Regular = 1,
    Directory = 2,
};

struct SuperblockDisk {
    std::uint64_t magic;
    std::uint32_t version;
    std::uint32_t block_size;
    std::uint64_t total_blocks;
    std::uint64_t total_inodes;
    std::uint64_t inode_bitmap_start;
    std::uint64_t inode_bitmap_blocks;
    std::uint64_t data_bitmap_start;
    std::uint64_t data_bitmap_blocks;
    std::uint64_t inode_table_start;
    std::uint64_t inode_table_blocks;
    std::uint64_t data_start;
    std::uint64_t data_blocks;
    std::uint64_t root_inode;
    std::uint64_t mount_count;
    std::uint8_t clean_shutdown;
    std::uint8_t reserved[7];
    char uuid[16];
    std::uint64_t checksum;
};

struct InodeDisk {
    std::uint64_t inode_number;
    std::uint8_t file_type;
    std::uint8_t reserved0[7];
    std::uint32_t mode;
    std::uint32_t uid;
    std::uint32_t gid;
    std::uint32_t link_count;
    std::uint64_t size;
    std::uint64_t allocated_blocks;
    std::int64_t atime;
    std::int64_t mtime;
    std::int64_t ctime;
    std::uint32_t flags;
    std::uint32_t reserved1;
    std::uint64_t direct_blocks[10];
    std::uint64_t single_indirect_block;
    std::uint8_t reserved2[88];
};

static_assert(sizeof(InodeDisk) == kInodeSize);

struct Layout {
    std::uint64_t total_blocks;
    std::uint64_t total_inodes;
    std::uint64_t inode_bitmap_start;
    std::uint64_t inode_bitmap_blocks;
    std::uint64_t data_bitmap_start;
    std::uint64_t data_bitmap_blocks;
    std::uint64_t inode_table_start;
    std::uint64_t inode_table_blocks;
    std::uint64_t data_start;
    std::uint64_t data_blocks;
};

Layout calculate_layout(std::uint64_t image_size);
void format_image(BlockDevice& device);
SuperblockDisk read_superblock(BlockDevice& device);
void write_superblock(BlockDevice& device, const SuperblockDisk& superblock);
void validate_superblock(const SuperblockDisk& superblock, std::uint64_t image_size);
std::string file_type_name(FileType type);
std::string uuid_string(const char (&uuid)[16]);

} // namespace fs
