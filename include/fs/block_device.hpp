#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace fs {

class BlockDevice {
public:
    enum class Mode { ReadOnly, ReadWrite };

    BlockDevice() = default;
    BlockDevice(const BlockDevice&) = delete;
    BlockDevice& operator=(const BlockDevice&) = delete;
    BlockDevice(BlockDevice&& other) noexcept;
    BlockDevice& operator=(BlockDevice&& other) noexcept;
    ~BlockDevice();

    static BlockDevice create(const std::string& path, std::uint64_t size);
    static BlockDevice open(const std::string& path, Mode mode);

    void read_at(std::uint64_t offset, std::span<std::byte> buffer) const;
    void write_at(std::uint64_t offset, std::span<const std::byte> buffer);
    void flush();

    std::uint64_t size() const { return size_; }
    int native_handle() const { return fd_; }

private:
    BlockDevice(int fd, std::uint64_t size, Mode mode);
    void close();

    int fd_ = -1;
    std::uint64_t size_ = 0;
    Mode mode_ = Mode::ReadOnly;
};

} // namespace fs
