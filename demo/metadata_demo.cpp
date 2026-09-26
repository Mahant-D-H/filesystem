#include "fs/block_device.hpp"
#include "fs/disk_format.hpp"

#include <cstdlib>
#include <iostream>

using namespace std;

int main() {
    constexpr const char* image_path = "/tmp/demo.img";
    remove(image_path);
    try {
        auto device = fs::BlockDevice::create(image_path, 1024 * 1024);
        fs::format_image(device);
        const auto superblock = fs::read_superblock(device);

        fs::InodeDisk inode {};
        inode.inode_number = 1;
        inode.file_type = static_cast<uint8_t>(fs::FileType::Regular);
        const uint64_t checkpoint_before_allocation = fs::read_superblock(device).last_checkpoint_lsn;
        const auto allocated = fs::allocate_inode_extents(device, superblock, inode, 5);
        if (allocated.size() != 1 || allocated[0].length != 5) {
            throw runtime_error("allocation did not return a five-block extent");
        }
        if (fs::read_superblock(device).last_checkpoint_lsn <= checkpoint_before_allocation) {
            throw runtime_error("extent allocation metadata bypassed WAL checkpointing");
        }

        const auto persisted_inode = fs::read_inode(device, superblock, 1);
        const auto persisted_extents = fs::read_inode_extents(device, superblock, persisted_inode);
        if (persisted_extents.size() != 1 || persisted_extents[0].start != allocated[0].start ||
            persisted_extents[0].length != 5) {
            throw runtime_error("persisted extent differs from allocated extent");
        }

        auto released_inode = persisted_inode;
        fs::release_inode_extents(device, superblock, released_inode);
        const auto bitmap = fs::read_data_bitmap(device, superblock);
        for (uint64_t index = allocated[0].start - superblock.data_start;
             index < allocated[0].start - superblock.data_start + allocated[0].length; ++index) {
            if (bitmap.get(index)) {
                throw runtime_error("released extent remains allocated");
            }
        }

        fs::InodeDisk fragmented_inode {};
        fragmented_inode.inode_number = 2;
        fragmented_inode.file_type = static_cast<uint8_t>(fs::FileType::Regular);
        for (size_t count = 0; count < 11; ++count) {
            fs::allocate_inode_extents(device, superblock, fragmented_inode, 1);
        }
        const auto fragmented_on_disk = fs::read_inode(device, superblock, 2);
        const auto fragmented_extents = fs::read_inode_extents(device, superblock, fragmented_on_disk);
        if (fragmented_extents.size() != 11 || fragmented_on_disk.single_indirect_block == 0) {
            throw runtime_error("indirect extent table was not persisted");
        }
        const uint64_t indirect_index = fragmented_on_disk.single_indirect_block - superblock.data_start;
        auto released_fragmented_inode = fragmented_on_disk;
        fs::release_inode_extents(device, superblock, released_fragmented_inode);
        if (fs::read_data_bitmap(device, superblock).get(indirect_index)) {
            throw runtime_error("indirect extent table block remains allocated");
        }
        remove(image_path);
        cout << "metadata demo: direct and indirect persisted extent allocation/release verified\n";
        return EXIT_SUCCESS;
    } catch (const exception& error) {
        remove(image_path);
        cerr << "metadata demo: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
