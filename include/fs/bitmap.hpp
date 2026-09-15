#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

using namespace std;

namespace fs {

class Bitmap {
public:
    explicit Bitmap(size_t bit_count = 0);

    bool get(size_t index) const;
    void set(size_t index, bool value);
    optional<size_t> find_free() const;

    size_t bit_count() const {
        return bit_count_;
    }

    size_t byte_count() const {
        return bytes_.size();
    }

    span<const std::byte> bytes() const {
        return bytes_;
    }
    
    span<byte> bytes() {
        return bytes_;
    }

private:
    size_t bit_count_;
    vector<byte> bytes_;
};

} // namespace fs
