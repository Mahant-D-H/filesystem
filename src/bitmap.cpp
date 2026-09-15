#include "fs/bitmap.hpp"

#include <stdexcept>

namespace fs {

Bitmap::Bitmap(size_t bit_count): bit_count_(bit_count), bytes_((bit_count + 7) / 8) {}

bool Bitmap::get(size_t index) const {
    if (index >= bit_count_) {
        throw out_of_range("bitmap index out of range");
    }
    const auto byte = to_integer<unsigned char>(bytes_[index / 8]);
    return (byte & (1u << (index % 8))) != 0;
}

void Bitmap::set(size_t index, bool value) {
    if (index >= bit_count_) {
        throw out_of_range("bitmap index out of range");
    }
    auto byte = to_integer<unsigned char>(bytes_[index / 8]);
    const auto mask = static_cast<unsigned char>(1u << (index % 8));

    if (value) {
        byte = byte | mask;
    }
    else {
        byte = byte & ~mask;
    }

    bytes_[index / 8] = static_cast<std::byte>(byte);
}

optional<size_t> Bitmap::find_free() const {
    for (size_t index = 0; index < bit_count_; ++index) {
        if (!get(index)) {
            return index;
        }
    }
    return nullopt;
}

} // namespace fs end
