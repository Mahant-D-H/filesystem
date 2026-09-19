#include "fs/block_device.hpp"
#include "fs/disk_format.hpp"
#include "fs/layout_engine.hpp"

#include <array>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <vector>

using namespace std;

int main() {
    constexpr const char* image_path = "/tmp/layout-demo.img";
    remove(image_path);
    try {
        auto device = fs::BlockDevice::create(image_path, 16 * 1024 * 1024);
        fs::format_image(device);
        const auto superblock = fs::read_superblock(device);

        auto pages = fs::FixedPageLayout::create(device, superblock, 3, 8 * 1024);
        vector<byte> page(8 * 1024, byte{0x2a});
        pages.write_page(2, page);
        vector<byte> page_readback(page.size());
        pages.read_page(2, page_readback);
        if (page_readback != page) {
            throw runtime_error("fixed-page readback mismatch");
        }
        auto reopened_pages = fs::FixedPageLayout::open(device, superblock, pages.inode_number());
        reopened_pages.read_page(2, page_readback);
        if (page_readback != page) {
            throw runtime_error("reopened fixed-page readback mismatch");
        }

        auto segment = fs::AppendOnlySegment::create(device, superblock, 4);
        array<byte, fs::kBlockSize> record{};
        record[0] = byte{0x51};
        if (segment.append(record) != 0 || segment.size() != fs::kBlockSize) {
            throw runtime_error("append-only segment did not advance correctly");
        }
        if (fs::AppendOnlySegment::open(device, superblock, segment.inode_number()).size() != fs::kBlockSize) {
            throw runtime_error("reopened append-only segment has the wrong size");
        }

        auto dense = fs::DenseArrayLayout::create(device, superblock, fs::kHugePageSize);
        array<byte, fs::kBlockSize> vector_block{};
        vector_block[0] = byte{0xa5};
        dense.write_at(0, vector_block);
        array<byte, fs::kBlockSize> vector_readback{};
        dense.read_at(0, vector_readback);
        if (vector_readback != vector_block) {
            throw runtime_error("dense-array readback mismatch");
        }
        auto reopened_dense = fs::DenseArrayLayout::open(device, superblock, dense.inode_number());
        reopened_dense.read_at(0, vector_readback);
        if (vector_readback != vector_block) {
            throw runtime_error("reopened dense-array readback mismatch");
        }

        remove(image_path);
        cout << "layout demo: fixed-page, append-only, and huge-page-aligned dense layouts verified\n";
        return EXIT_SUCCESS;
    } catch (const exception& error) {
        remove(image_path);
        cerr << "layout demo: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
