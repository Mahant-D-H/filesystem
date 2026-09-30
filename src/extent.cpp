#include "fs/extent.hpp"

#include <stdexcept>

using namespace std;

namespace fs {

ExtentAllocator::ExtentAllocator(uint64_t total_blocks):
    used_(static_cast<size_t>(total_blocks)),
    total_blocks_(total_blocks),
    free_blocks_(total_blocks) {}

uint64_t ExtentAllocator::total_blocks() const noexcept {
    return total_blocks_;
}

uint64_t ExtentAllocator::free_blocks() const noexcept {
    return free_blocks_;
}

optional<Extent> ExtentAllocator::allocate(uint64_t length) {
    if (length == 0) {
        return nullopt;
    }
    if (length > free_blocks_) {
        return nullopt;
    }

    uint64_t start = 0;
    uint64_t run = 0;
    while (start < total_blocks_) {
        if (!used_.get(start)) {
            ++run;
            if (run == length) {
                for (uint64_t i = start - (length - 1); i <= start; ++i) {
                    used_.set(i, true);
                }
                free_blocks_ -= length;
                return Extent{start - (length - 1), length};
            }
        } else {
            run = 0;
        }
        ++start;
    }

    return nullopt;
}

bool ExtentAllocator::release(uint64_t start, uint64_t length) {
    if (start >= total_blocks_ || length == 0) {
        return false;
    }
    if (length > total_blocks_ - start) {
        return false;
    }
    for (uint64_t i = start; i < start + length; ++i) {
        if (!used_.get(i)) {
            return false;
        }
    }
    for (uint64_t i = start; i < start + length; ++i) {
        used_.set(i, false);
    }
    free_blocks_ += length;
    return true;
}

bool ExtentAllocator::is_allocated(uint64_t start, uint64_t length) const {
    if (start >= total_blocks_ || length == 0) {
        return false;
    }
    if (length > total_blocks_ - start) {
        throw out_of_range("extent exceeds allocator bounds");
    }
    for (uint64_t i = start; i < start + length; ++i) {
        if (!used_.get(i)) {
            return false;
        }
    }
    return true;
}

vector<Extent> ExtentAllocator::free_extents() const {
    vector<Extent> extents;
    uint64_t start = 0;
    bool in_run = false;

    while (start < total_blocks_) {
        if (!used_.get(start)) {
            if (!in_run) {
                in_run = true;
                Extent extent{};
                extent.start = start;
                extent.length = 1;
                extents.push_back(extent);
            } else {
                extents.back().length += 1;
            }
        } else if (in_run) {
            in_run = false;
        }
        ++start;
    }

    return extents;
}

} // namespace fs
