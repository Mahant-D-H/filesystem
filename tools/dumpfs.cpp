#include "fs/block_device.hpp"
#include "fs/disk_format.hpp"

#include <cstdlib>
#include <iomanip>
#include <iostream>

using namespace std;

int main(int argc, char* argv[]) {
    if (argc != 2) {
        cerr << "usage: ./filesystem-dumpfs <image_name>\n";
        return EXIT_FAILURE;
    }
    try {
        auto device = fs::BlockDevice::open(argv[1], fs::BlockDevice::Mode::ReadOnly);
        const auto superblock = fs::read_superblock(device);
        cout << "magic:              0x" << hex << superblock.magic << dec << '\n'
             << "version:            " << superblock.version << '\n'
             << "block size:         " << superblock.block_size << '\n'
             << "total blocks:       " << superblock.total_blocks << '\n'
             << "total inodes:       " << superblock.total_inodes << '\n'
             << "inode bitmap:       " << superblock.inode_bitmap_start << " (" << superblock.inode_bitmap_blocks << " blocks)\n"
             << "data bitmap:        " << superblock.data_bitmap_start << " (" << superblock.data_bitmap_blocks << " blocks)\n"
             << "inode table:        " << superblock.inode_table_start << " (" << superblock.inode_table_blocks << " blocks)\n"
             << "write-ahead log:    " << superblock.wal_start << " (" << superblock.wal_blocks << " blocks)\n"
             << "double-write zone:  " << superblock.double_write_start << " (" << superblock.double_write_blocks << " blocks)\n"
             << "data region:        " << superblock.data_start << " (" << superblock.data_blocks << " blocks)\n"
             << "root inode:         " << superblock.root_inode << '\n'
             << "mount count:        " << superblock.mount_count << '\n'
             << "clean shutdown:     " << (superblock.clean_shutdown ? "yes" : "no") << '\n'
             << "checkpoint LSN:     " << superblock.last_checkpoint_lsn << '\n'
             << "uuid:               " << fs::uuid_string(superblock.uuid) << '\n';
        return EXIT_SUCCESS;
    } catch (const exception& error) {
        cerr << "dumpfs: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
