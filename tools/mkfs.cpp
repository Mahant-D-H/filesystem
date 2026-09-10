#include "fs/block_device.hpp"
#include "fs/disk_format.hpp"

#include <cstdlib>
#include <iostream>

int main(int argc, char* argv[]) {
    if (argc != 3) {
        std::cerr << "usage: filesystem-mkfs <image> <size-in-bytes>\n";
        return EXIT_FAILURE;
    }
    try {
        const auto size = std::stoull(argv[2]);
        auto device = fs::BlockDevice::create(argv[1], size);
        fs::format_image(device);
        std::cout << "formatted " << argv[1] << " (" << device.size() << " bytes)\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "mkfs: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
