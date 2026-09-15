#include "fs/block_device.hpp"
#include "fs/disk_format.hpp"

#include <cstdlib>
#include <iostream>

using namespace std;

int main(int argc, char* argv[]) {
    if (argc != 3) {
        cerr << "usage: ./filesystem-mkfs <image_name> <size-in-bytes>\n";
        return EXIT_FAILURE;
    }
    try {
        const auto size = stoull(argv[2]);
        auto device = fs::BlockDevice::create(argv[1], size);
        fs::format_image(device);
        cout << "formatted " << argv[1] << " (" << device.size() << " bytes)\n";
        return EXIT_SUCCESS;
    } catch (const exception& error) {
        cerr << "mkfs: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
