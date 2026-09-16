#include "fs/extent.hpp"

#include <cstdlib>
#include <iostream>
#include <optional>

using namespace std;

int main() {
    fs::ExtentAllocator allocator(32);

    auto first = allocator.allocate(4);
    if (!first || first->start != 0 || first->length != 4) {
        cerr << "extent demo: first allocation failed\n";
        return EXIT_FAILURE;
    }

    auto second = allocator.allocate(3);
    if (!second || second->start != 4 || second->length != 3) {
        cerr << "extent demo: second allocation failed\n";
        return EXIT_FAILURE;
    }

    if (!allocator.release(0, 4)) {
        cerr << "extent demo: release failed\n";
        return EXIT_FAILURE;
    }

    auto third = allocator.allocate(4);
    if (!third || third->start != 0 || third->length != 4) {
        cerr << "extent demo: reallocation failed\n";
        return EXIT_FAILURE;
    }

    cout << "extent allocator demo: contiguous 4-block and 3-block extents allocated/reused successfully\n";
    return EXIT_SUCCESS;
}
