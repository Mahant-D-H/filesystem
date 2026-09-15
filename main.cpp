#include "fs/block_device.hpp"
#include "fs/disk_format.hpp"

#include <cstdlib>
#include <iostream>

using namespace std;

int main(int argc, char* argv[]) {
    (void)argv;
    if (argc < 2) {
        cout << "filesystem: storage library bootstrap\n"
                     "commands:\n"
                     "  mkfs <image> <size-in-bytes>\n"
                     "  dumpfs <image>\n";
        return EXIT_SUCCESS;
    }
    cerr << "use filesystem-mkfs or filesystem-dumpfs for the current commands\n";
    return EXIT_FAILURE;
}