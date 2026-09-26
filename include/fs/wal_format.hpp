#pragma once

#include "fs/disk_format.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace fs::detail {

inline constexpr uint64_t kWalMagic = 0x314c41574653ULL;
enum class WalType : uint32_t { Begin = 1, Update = 2, Commit = 3 };

struct WalHeader {
    uint64_t magic = kWalMagic;
    uint32_t type = 0;
    uint32_t reserved = 0;
    uint64_t lsn = 0;
    uint64_t transaction_id = 0;
    uint64_t target_block = 0;
    uint64_t page_checksum = 0;
    uint64_t header_checksum = 0;
};
static_assert(sizeof(WalHeader) == 56);

inline uint64_t checksum(std::span<const std::byte> bytes) {
    uint64_t value = 1469598103934665603ULL;
    for (const auto byte : bytes) {
        value ^= std::to_integer<unsigned char>(byte);
        value *= 1099511628211ULL;
    }
    return value;
}

inline uint64_t header_checksum(WalHeader header) {
    header.header_checksum = 0;
    return checksum(std::as_bytes(std::span{&header, size_t{1}}));
}

inline std::array<std::byte, kBlockSize> record_block(WalHeader header) {
    header.header_checksum = header_checksum(header);
    std::array<std::byte, kBlockSize> block{};
    std::memcpy(block.data(), &header, sizeof(header));
    return block;
}

} // namespace fs::detail
