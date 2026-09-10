#include "fs/bitmap.hpp"

#include <stdexcept>

namespace fs {

Bitmap::Bitmap(std::size_t bit_count)
    : bit_count_(bit_count), bytes_((bit_count + 7) / 8) {}

bool Bitmap::get(std::size_t index) const {
    if (index >= bit_count_) {
        throw std::out_of_range("bitmap index out of range");
    }
    const auto byte = std::to_integer<unsigned char>(bytes_[index / 8]);
    return (byte & (1u << (index % 8))) != 0;
}

void Bitmap::set(std::size_t index, bool value) {
    if (index >= bit_count_) {
        throw std::out_of_range("bitmap index out of range");
    }
    auto byte = std::to_integer<unsigned char>(bytes_[index / 8]);
    const auto mask = static_cast<unsigned char>(1u << (index % 8));
    byte = value ? static_cast<unsigned char>(byte | mask)
                 : static_cast<unsigned char>(byte & ~mask);
    bytes_[index / 8] = static_cast<std::byte>(byte);
}

std::optional<std::size_t> Bitmap::find_free() const {
    for (std::size_t index = 0; index < bit_count_; ++index) {
        if (!get(index)) {
            return index;
        }
    }
    return std::nullopt;
}

} // namespace fs
