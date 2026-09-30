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
    constexpr const char* image_path = "/tmp/test.img";
    remove(image_path);
    try {
        auto device = fs::BlockDevice::create(image_path, 16 * 1024 * 1024);
        fs::format_image(device);
        const auto superblock = fs::read_superblock(device);

        auto pages = fs::FixedPageLayout::create(device, superblock, 3, 8 * 1024);
        const uint64_t checkpoint_before_page_write = fs::read_superblock(device).last_checkpoint_lsn;
        vector<byte> page(8 * 1024, byte{0x2a});
        pages.write_page(2, page);
        const uint64_t checkpoint_after_page_write = fs::read_superblock(device).last_checkpoint_lsn;
        if (checkpoint_after_page_write < checkpoint_before_page_write + 2) {
            throw runtime_error("fixed-page updates did not pass through WAL checkpointing");
        }
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

        const auto expect_failure = [](auto&& operation) {
            try {
                operation();
            } catch (const exception&) {
                return;
            }
            throw runtime_error("invalid layout operation unexpectedly succeeded");
        };
        expect_failure([&] { fs::FixedPageLayout::create(device, superblock, 0); });
        expect_failure([&] { fs::FixedPageLayout::create(device, superblock, 1, fs::kBlockSize - 1); });
        expect_failure([&] { pages.read_page(pages.page_count(), page_readback); });
        expect_failure([&] { pages.write_page(pages.page_count(), page); });
        vector<byte> short_page(page.size() - 1);
        expect_failure([&] { pages.read_page(0, short_page); });

        vector<byte> oversized_append(5 * fs::kBlockSize);
        expect_failure([&] { segment.append(oversized_append); });
        expect_failure([&] { segment.append(span<const byte>(record.data(), record.size() - 1)); });

        expect_failure([&] { dense.read_at(1, vector_readback); });
        expect_failure([&] { dense.write_at(dense.capacity(), vector_block); });
        expect_failure([&] { fs::DenseArrayLayout::create(device, superblock, fs::kBlockSize); });

        remove(image_path);
        cout << "layout tests passed: fixed-page, append-only, dense layouts, and invalid geometry/bounds\n";
        return EXIT_SUCCESS;
    } catch (const exception& error) {
        remove(image_path);
        cerr << "error: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
