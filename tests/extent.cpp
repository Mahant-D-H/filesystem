#include "fs/extent.hpp"

#include <cstdlib>
#include <iostream>
#include <limits>
#include <optional>

using namespace std;

int main() {
    try {
        fs::ExtentAllocator allocator(32);

        if (allocator.allocate(0) || allocator.allocate(33) || allocator.free_blocks() != 32) {
            throw runtime_error("invalid allocations changed allocator state");
        }
        auto first = allocator.allocate(4);
        auto second = allocator.allocate(3);
        if (!first || first->start != 0 || first->length != 4 ||
            !second || second->start != 4 || second->length != 3) {
            throw runtime_error("contiguous allocations returned unexpected extents");
        }
        if (!allocator.is_allocated(0, 4) || allocator.is_allocated(0, 0)) {
            throw runtime_error("allocation query returned an incorrect result");
        }
        if (!allocator.release(0, 4) || allocator.release(0, 4) ||
            allocator.is_allocated(0, 4) || allocator.free_blocks() != 29) {
            throw runtime_error("release or repeated-release accounting failed");
        }
        if (!allocator.release(4, 3)) {
            throw runtime_error("failed to release second test extent");
        }

        auto reused = allocator.allocate(4);
        if (!reused || reused->start != 0 || reused->length != 4) {
            throw runtime_error("released extent was not reused");
        }
        const uint64_t free_before_invalid_release = allocator.free_blocks();
        if (allocator.release(3, 2) || !allocator.is_allocated(0, 4) ||
            allocator.free_blocks() != free_before_invalid_release) {
            throw runtime_error("failed partial release modified allocated blocks");
        }
        if (allocator.release(1, numeric_limits<uint64_t>::max()) ||
            allocator.free_blocks() != free_before_invalid_release) {
            throw runtime_error("overflowing release modified allocator state");
        }

        bool overflow_query_rejected = false;
        try {
            allocator.is_allocated(1, numeric_limits<uint64_t>::max());
        } catch (const out_of_range&) {
            overflow_query_rejected = true;
        }
        if (!overflow_query_rejected) {
            throw runtime_error("overflowing allocation query was not rejected");
        }

        fs::ExtentAllocator fragmented(16);
        if (!fragmented.allocate(4) || !fragmented.allocate(4) || !fragmented.allocate(4) ||
            !fragmented.release(4, 4) || fragmented.allocate(5)) {
            throw runtime_error("fragmented space incorrectly satisfied a contiguous allocation");
        }
        if (!fragmented.release(0, 4) || !fragmented.release(8, 4)) {
            throw runtime_error("failed to release fragmented extents");
        }
        auto coalesced = fragmented.allocate(12);
        if (!coalesced || coalesced->start != 0 || coalesced->length != 12) {
            throw runtime_error("adjacent free space was not reusable as one extent");
        }

        cout << "extent allocator tests passed: contiguous allocation, reuse, fragmentation, "
                "range validation, and failed-release atomicity\n";
        return EXIT_SUCCESS;
    } catch (const exception& error) {
        cerr << "extent allocator test error: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
