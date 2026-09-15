#include "fs/async_io.hpp"
#include "fs/block_device.hpp"

#include <array>
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>

int main() {
    const std::string image_path = "/tmp/demo.img";
    std::remove(image_path.c_str());

    constexpr size_t kBlockSize = 4 * 1024;
    std::array<std::byte, kBlockSize> test_pattern{};
    for (size_t i = 0; i < test_pattern.size(); ++i) {
        test_pattern[i] = static_cast<std::byte>((i * 7u) & 0xffu);
    }

    const int fd = ::open(image_path.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_DIRECT | O_SYNC, 0644);
    if (fd < 0) {
        std::cerr << "demo: failed to create test image\n";
        return EXIT_FAILURE;
    }
    if (ftruncate(fd, static_cast<off_t>(kBlockSize * 2)) != 0) {
        std::cerr << "demo: failed to size test image\n";
        ::close(fd);
        return EXIT_FAILURE;
    }

    void* raw_write = fs::AsyncIo::allocate_aligned_buffer(kBlockSize);
    std::memcpy(raw_write, test_pattern.data(), kBlockSize);

    void* raw_read = fs::AsyncIo::allocate_aligned_buffer(kBlockSize);
    std::memset(raw_read, 0, kBlockSize);

    fs::AsyncIo io(4);
    auto* write_req = io.submit_write(fd, 0, raw_write, kBlockSize);
    io.submit();
    if (!io.wait_for(*write_req)) {
        fs::AsyncIo::free_aligned_buffer(raw_write);
        fs::AsyncIo::free_aligned_buffer(raw_read);
        ::close(fd);
        std::cerr << "demo: async write failed\n";
        return EXIT_FAILURE;
    }

    auto* read_req = io.submit_read(fd, 0, raw_read, kBlockSize);
    io.submit();
    if (!io.wait_for(*read_req)) {
        fs::AsyncIo::free_aligned_buffer(raw_write);
        fs::AsyncIo::free_aligned_buffer(raw_read);
        ::close(fd);
        std::cerr << "demo: async read failed\n";
        return EXIT_FAILURE;
    }

    const bool matches = std::memcmp(raw_read, test_pattern.data(), kBlockSize) == 0;
    fs::AsyncIo::free_aligned_buffer(raw_write);
    fs::AsyncIo::free_aligned_buffer(raw_read);
    ::close(fd);

    if (!matches) {
        std::cerr << "demo: async read/write verification failed\n";
        return EXIT_FAILURE;
    }

    std::cout << "async I/O demo: verified 4 KiB aligned read/write via io_uring\n";
    return EXIT_SUCCESS;
}
