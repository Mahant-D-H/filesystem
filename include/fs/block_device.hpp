#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

using namespace std;

namespace fs {

class AsyncIo;

inline constexpr size_t kDeviceAlignment = 4096;

class BlockDevice {
public:
    enum class Mode { ReadOnly, ReadWrite };

    BlockDevice() = default;
    BlockDevice(const BlockDevice&) = delete;
    BlockDevice& operator=(const BlockDevice&) = delete;
    BlockDevice(BlockDevice&& other) noexcept;
    BlockDevice& operator=(BlockDevice&& other) noexcept;
    ~BlockDevice();

    static BlockDevice create(const string& path, uint64_t size);
    static BlockDevice open(const string& path, Mode mode);

    void read_at(uint64_t offset, span<byte> buffer) const;
    void write_at(uint64_t offset, span<const byte> buffer);
    void read_at_async(uint64_t offset, span<byte> buffer, AsyncIo& io) const;
    void write_at_async(uint64_t offset, span<const byte> buffer, AsyncIo& io);
    void flush();

    uint64_t size() const { return size_; }
    int native_handle() const { return fd_; }

private:
    BlockDevice(int fd, uint64_t size, Mode mode, string path = {});
    void close() const;
    void reopen_without_direct_io() const;

    mutable int fd_ = -1;
    uint64_t size_ = 0;
    Mode mode_ = Mode::ReadOnly;
    string path_;
};

} // namespace fs
