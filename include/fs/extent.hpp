#pragma once

#include "fs/bitmap.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

using namespace std;

namespace fs {

struct Extent {
    uint64_t start = 0;
    uint64_t length = 0;

    bool empty() const noexcept {
        return length == 0;
    }
};

class ExtentAllocator {
public:
    explicit ExtentAllocator(uint64_t total_blocks = 0);

    uint64_t total_blocks() const noexcept;
    uint64_t free_blocks() const noexcept;

    optional<Extent> allocate(uint64_t length);
    bool release(uint64_t start, uint64_t length);
    bool is_allocated(uint64_t start, uint64_t length) const;
    vector<Extent> free_extents() const;

private:
    Bitmap used_;
    uint64_t total_blocks_ = 0;
    uint64_t free_blocks_ = 0;
};

} // namespace fs
