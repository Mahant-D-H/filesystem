#include "fs/block_device.hpp"
#include "fs/disk_format.hpp"

#include <cstdlib>
#include <iomanip>
#include <iostream>

int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::cerr << "usage: filesystem-dumpfs <image>\n";
        return EXIT_FAILURE;
    }
    try {
        auto device = fs::BlockDevice::open(argv[1], fs::BlockDevice::Mode::ReadOnly);
        const auto superblock = fs::read_superblock(device);
        std::cout << "magic:              0x" << std::hex << superblock.magic << std::dec << '\n'
                  << "version:            " << superblock.version << '\n'
                  << "block size:         " << superblock.block_size << '\n'
                  << "total blocks:       " << superblock.total_blocks << '\n'
                  << "total inodes:       " << superblock.total_inodes << '\n'
                  << "inode bitmap:       " << superblock.inode_bitmap_start
                  << " (" << superblock.inode_bitmap_blocks << " blocks)\n"
                  << "data bitmap:        " << superblock.data_bitmap_start
                  << " (" << superblock.data_bitmap_blocks << " blocks)\n"
                  << "inode table:        " << superblock.inode_table_start
                  << " (" << superblock.inode_table_blocks << " blocks)\n"
                  << "data region:        " << superblock.data_start
                  << " (" << superblock.data_blocks << " blocks)\n"
                  << "root inode:         " << superblock.root_inode << '\n'
                  << "mount count:        " << superblock.mount_count << '\n'
                  << "clean shutdown:     " << (superblock.clean_shutdown ? "yes" : "no") << '\n'
                  << "uuid:               " << fs::uuid_string(superblock.uuid) << '\n';
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "dumpfs: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
