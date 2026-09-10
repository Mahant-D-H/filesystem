#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace fs {

class Bitmap {
public:
    explicit Bitmap(std::size_t bit_count = 0);

    bool get(std::size_t index) const;
    void set(std::size_t index, bool value);
    std::optional<std::size_t> find_free() const;
    std::size_t bit_count() const { return bit_count_; }
    std::size_t byte_count() const { return bytes_.size(); }
    std::span<const std::byte> bytes() const { return bytes_; }
    std::span<std::byte> bytes() { return bytes_; }

private:
    std::size_t bit_count_;
    std::vector<std::byte> bytes_;
};

} // namespace fs
