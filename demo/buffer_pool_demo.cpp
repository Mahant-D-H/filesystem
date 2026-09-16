#include "fs/block_device.hpp"
#include "fs/buffer_pool.hpp"

#include <array>
#include <cstdlib>
#include <cstring>
#include <iostream>

using namespace std;

int main() {
    constexpr const char* image_path = "/tmp/demo.img";
    remove(image_path);
    try {
        auto device = fs::BlockDevice::create(image_path, 8 * fs::kBlockSize);
        array<byte, fs::kBlockSize> zeroes{};
        for (uint64_t block = 0; block < 8; ++block) {
            device.write_at(block * fs::kBlockSize, zeroes);
        }

        fs::BufferPool pool(device, 4);
        {
            auto page = pool.fetch(0);
            if (page.data()[0] != byte{}) {
                throw runtime_error("unexpected initial block contents");
            }
        }
        // A second access promotes block 0 to the 2Q hot queue.
        {
            auto page = pool.fetch(0);
        }
        // These one-pass scan pages stay probationary and evict one another.
        for (uint64_t block = 1; block < 8; ++block) {
            auto page = pool.fetch(block);
            (void)page;
        }
        array<byte, fs::kBlockSize> changed_on_disk{};
        changed_on_disk[0] = byte{0x7f};
        device.write_at(0, changed_on_disk);
        {
            auto page = pool.fetch(0);
            if (page.data()[0] != byte{}) {
                throw runtime_error("scan evicted a hot 2Q page");
            }
            memcpy(page.data(), "buffer-pool", 11);
            page.mark_dirty();
        }
        pool.flush_all();

        array<byte, fs::kBlockSize> readback{};
        device.read_at(0, readback);
        remove(image_path);
        if (memcmp(readback.data(), "buffer-pool", 11) != 0) {
            cerr << "buffer pool demo: dirty page was not persisted\n";
            return EXIT_FAILURE;
        }
        cout << "buffer pool demo: 2Q scan resistance and dirty flush verified\n";
        return EXIT_SUCCESS;
    } catch (const exception& error) {
        remove(image_path);
        cerr << "buffer pool demo: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
