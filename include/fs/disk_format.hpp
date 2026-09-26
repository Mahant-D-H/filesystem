#pragma once

#include "fs/bitmap.hpp"
#include "fs/block_device.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

using namespace std;

namespace fs {

inline constexpr uint32_t kBlockSize = 4 * 1024; // BlockSize = 4KB
inline constexpr uint32_t kFormatVersion = 3;
inline constexpr uint64_t kSuperblockMagic = 0x315359534653ULL;
inline constexpr uint32_t kInodeSize = 256;
inline constexpr uint32_t kDefaultInodesPer16KiB = 1;
inline constexpr uint32_t kExtentTableEntriesPerBlock = 255;

enum class FileType : uint8_t {
    Free = 0,
    Regular = 1,
    Directory = 2,
};

struct SuperblockDisk {
    uint64_t magic;
    uint32_t version;
    uint32_t block_size;
    uint64_t total_blocks;
    uint64_t total_inodes;
    uint64_t inode_bitmap_start;
    uint64_t inode_bitmap_blocks;
    uint64_t data_bitmap_start;
    uint64_t data_bitmap_blocks;
    uint64_t inode_table_start;
    uint64_t inode_table_blocks;
    uint64_t wal_start;
    uint64_t wal_blocks;
    uint64_t double_write_start;
    uint64_t double_write_blocks;
    uint64_t data_start;
    uint64_t data_blocks;
    uint64_t root_inode;
    uint64_t mount_count;
    uint8_t clean_shutdown;
    uint8_t reserved[7];
    uint64_t last_checkpoint_lsn;
    char uuid[16];
    uint64_t checksum;
};

struct ExtentDisk {
    uint64_t start = 0;
    uint64_t length = 0;
};

struct InodeDisk {
    uint64_t inode_number;
    uint8_t file_type;
    uint8_t reserved0[7];
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
    uint32_t link_count;
    uint64_t size;
    uint64_t allocated_blocks;
    int64_t atime;
    int64_t mtime;
    int64_t ctime;
    uint32_t flags;
    uint32_t reserved1;
    ExtentDisk direct_extents[10];
    uint64_t single_indirect_block;
    uint8_t reserved2[8];
};

static_assert(sizeof(InodeDisk) == kInodeSize);

struct Layout {
    uint64_t total_blocks;
    uint64_t total_inodes;
    uint64_t inode_bitmap_start;
    uint64_t inode_bitmap_blocks;
    uint64_t data_bitmap_start;
    uint64_t data_bitmap_blocks;
    uint64_t inode_table_start;
    uint64_t inode_table_blocks;
    uint64_t wal_start;
    uint64_t wal_blocks;
    uint64_t double_write_start;
    uint64_t double_write_blocks;
    uint64_t data_start;
    uint64_t data_blocks;
};

struct ExtentTableBlock {
    uint32_t entry_count = 0;
    uint32_t reserved = 0;
    ExtentDisk extents[kExtentTableEntriesPerBlock];
};

Layout calculate_layout(uint64_t image_size);
void format_image(BlockDevice& device);
SuperblockDisk read_superblock(BlockDevice& device);
void write_superblock(BlockDevice& device, const SuperblockDisk& superblock);
void validate_superblock(const SuperblockDisk& superblock, uint64_t image_size);

InodeDisk read_inode(BlockDevice& device, const SuperblockDisk& superblock, uint64_t index);
void write_inode(BlockDevice& device, const SuperblockDisk& superblock, const InodeDisk& inode);

vector<ExtentDisk> read_inode_extents(const BlockDevice& device, const SuperblockDisk& superblock, const InodeDisk& inode);
void write_inode_extents(BlockDevice& device, const SuperblockDisk& superblock, InodeDisk& inode, const vector<ExtentDisk>& extents);
vector<ExtentDisk> allocate_inode_extents(BlockDevice& device, const SuperblockDisk& superblock, InodeDisk& inode, uint64_t block_count);
vector<ExtentDisk> allocate_inode_extents_aligned(BlockDevice& device, const SuperblockDisk& superblock, InodeDisk& inode, uint64_t block_count, uint64_t alignment_blocks);
bool release_inode_extents(BlockDevice& device, const SuperblockDisk& superblock, InodeDisk& inode);

Bitmap read_inode_bitmap(const BlockDevice& device, const SuperblockDisk& superblock);
void write_inode_bitmap(BlockDevice& device, const SuperblockDisk& superblock, const Bitmap& bitmap);
InodeDisk allocate_inode(BlockDevice& device, const SuperblockDisk& superblock, FileType type, uint32_t mode = 0644);
void release_inode(BlockDevice& device, const SuperblockDisk& superblock, InodeDisk& inode);

Bitmap read_data_bitmap(const BlockDevice& device, const SuperblockDisk& superblock);
void write_data_bitmap(BlockDevice& device, const SuperblockDisk& superblock, const Bitmap& bitmap);

string file_type_name(FileType type);
string uuid_string(const char (&uuid)[16]);

} // namespace fs
