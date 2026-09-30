#include <algorithm>
#include "fs/block_device.hpp"

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unistd.h>

namespace {

class TempImage {
public:
    TempImage() {
        std::array<char, 40> path{};
        const std::string pattern = "/tmp/filesystem-device-XXXXXX";
        std::copy(pattern.begin(), pattern.end(), path.begin());
        const int fd = mkstemp(path.data());
        if (fd < 0) {
            throw std::system_error(errno, std::generic_category(), "mkstemp");
        }
        close(fd);
        path_ = path.data();
    }

    ~TempImage() {
        std::remove(path_.c_str());
    }

    const std::string& path() const noexcept {
        return path_;
    }

private:
    std::string path_;
};

template<class Operation>
void expect_throw(Operation&& operation, const char* message) {
    try {
        operation();
    } catch (const std::exception&) {
        return;
    }
    throw std::runtime_error(message);
}

} // namespace

int main() {
    try {
        TempImage image;
        expect_throw([&] { fs::BlockDevice::create(image.path(), 0); },
                     "zero-sized image creation unexpectedly succeeded");

        constexpr size_t image_size = 3 * fs::kDeviceAlignment;
        {
            auto device = fs::BlockDevice::create(image.path(), image_size);
            std::array<std::byte, 513> payload{};
            for (size_t index = 0; index < payload.size(); ++index) {
                payload[index] = static_cast<std::byte>((index * 17) & 0xff);
            }

            device.write_at(137, payload);
            std::array<std::byte, 513> readback{};
            device.read_at(137, readback);
            if (readback != payload) {
                throw std::runtime_error("unaligned partial write/read did not round-trip");
            }

            std::array<std::byte, fs::kDeviceAlignment> block{};
            block.front() = std::byte{0xa5};
            block.back() = std::byte{0x5a};
            device.write_at(fs::kDeviceAlignment, block);
            std::array<std::byte, fs::kDeviceAlignment> block_readback{};
            device.read_at(fs::kDeviceAlignment, block_readback);
            if (block_readback != block) {
                throw std::runtime_error("aligned full-block write/read did not round-trip");
            }
            device.flush();

            expect_throw([&] {
                std::array<std::byte, 2> out_of_bounds{};
                device.read_at(image_size - 1, out_of_bounds);
            }, "out-of-bounds read unexpectedly succeeded");
            expect_throw([&] {
                std::array<std::byte, 2> out_of_bounds{};
                device.write_at(image_size - 1, out_of_bounds);
            }, "out-of-bounds write unexpectedly succeeded");
        }

        {
            auto device = fs::BlockDevice::open(image.path(), fs::BlockDevice::Mode::ReadOnly);
            std::array<std::byte, 513> readback{};
            device.read_at(137, readback);
            if (readback[0] != std::byte{0} || readback[1] != static_cast<std::byte>(17)) {
                throw std::runtime_error("data did not persist across close and reopen");
            }
            bool read_only_rejected = false;
            try {
                device.write_at(0, readback);
            } catch (const std::system_error& error) {
                read_only_rejected = error.code().value() == EROFS;
            }
            if (!read_only_rejected) {
                throw std::runtime_error("write through a read-only device was not rejected with EROFS");
            }
        }

        std::cout << "block-device tests passed: partial/aligned I/O, persistence, bounds, and read-only mode\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "block-device test error: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
