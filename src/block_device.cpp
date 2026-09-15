#include "fs/block_device.hpp"

#include "fs/async_io.hpp"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>

namespace fs {
namespace {

bool is_aligned(const void* ptr, size_t alignment) {
    return (reinterpret_cast<uintptr_t>(ptr) % alignment) == 0;
}

void* allocate_aligned(size_t size) {
    if (size == 0) {
        size = kDeviceAlignment;
    }
    void* memory = nullptr;
    if (posix_memalign(&memory, kDeviceAlignment, size) != 0) {
        throw bad_alloc();
    }
    return memory;
}

[[noreturn]] void throw_errno(const char* operation) {
    throw system_error(errno, generic_category(), operation);
}

uint64_t file_size(int fd) {
    struct stat st {};
    if (fstat(fd, &st) != 0) {
        throw_errno("fstat");
    }
    if (st.st_size < 0) {
        throw runtime_error("image has a negative size");
    }
    return static_cast<uint64_t>(st.st_size);
}

} // namespace end

BlockDevice::BlockDevice(int fd, uint64_t size, Mode mode, string path): fd_(fd), size_(size), mode_(mode), path_(move(path)) {}

BlockDevice::BlockDevice(BlockDevice&& other) noexcept: fd_(other.fd_), size_(other.size_), mode_(other.mode_), path_(move(other.path_)) {
    other.fd_ = -1;
    other.size_ = 0;
    other.path_.clear();
}

BlockDevice& BlockDevice::operator=(BlockDevice&& other) noexcept {
    if (this != &other) {
        close();
        fd_ = other.fd_;
        size_ = other.size_;
        mode_ = other.mode_;
        path_ = move(other.path_);
        other.fd_ = -1;
        other.size_ = 0;
        other.path_.clear();
    }
    return *this;
}

BlockDevice::~BlockDevice() {
    close();
}

BlockDevice BlockDevice::create(const string& path, uint64_t size) {
    if (size == 0) {
        throw invalid_argument("image size must be greater than zero");
    }

    const int flags = O_RDWR | O_CREAT | O_TRUNC | O_DIRECT | O_SYNC;
    const int fd = ::open(path.c_str(), flags, 0644);
    if (fd < 0) {
        throw_errno("open image");
    }
    if (ftruncate(fd, static_cast<off_t>(size)) != 0) {
        const int saved_errno = errno;
        ::close(fd);
        errno = saved_errno;
        throw_errno("resize image");
    }
    return BlockDevice(fd, size, Mode::ReadWrite, path);
}

BlockDevice BlockDevice::open(const string& path, Mode mode) {
    const int flags = mode == Mode::ReadOnly ? (O_RDONLY | O_DIRECT | O_SYNC) : (O_RDWR | O_DIRECT | O_SYNC);
    const int fd = ::open(path.c_str(), flags);
    if (fd < 0) {
        throw_errno("open image");
    }

    try {
        return BlockDevice(fd, file_size(fd), mode, path);
    }
    catch (...) {
        ::close(fd);
        throw;
    }
}

void BlockDevice::reopen_without_direct_io() const {
    if (path_.empty()) {
        throw runtime_error("cannot reopen device without a backing path");
    }
    const int flags = mode_ == Mode::ReadOnly ? O_RDONLY : (O_RDWR | O_SYNC);
    const int reopened = ::open(path_.c_str(), flags);
    if (reopened < 0) {
        throw_errno("reopen image");
    }
    close();
    fd_ = reopened;
}

void BlockDevice::read_at(uint64_t offset, span<byte> buffer) const {
    if (offset > size_ || buffer.size() > size_ - offset) {
        throw out_of_range("read exceeds image bounds");
    }
    if (buffer.size() == 0) {
        return;
    }

    auto* source = buffer.data();
    unique_ptr<byte, void(*)(void*)> aligned_storage(nullptr, free);
    if (!is_aligned(source, kDeviceAlignment) || (buffer.size() % kDeviceAlignment) != 0) {
        void* raw = allocate_aligned(buffer.size());
        aligned_storage.reset(static_cast<byte*>(raw));
        source = aligned_storage.get();
    }

    size_t completed = 0;
    while (completed < buffer.size()) {
        const ssize_t result = pread(fd_, source + completed, buffer.size() - completed, static_cast<off_t>(offset + completed));

        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EINVAL || errno == EOPNOTSUPP) {
                reopen_without_direct_io();
                return read_at(offset, buffer);
            }
            throw_errno("read image");
        }
        if (result == 0) {
            throw runtime_error("unexpected end of image");
        }
        completed += static_cast<size_t>(result);
    }

    if (aligned_storage) {
        memcpy(buffer.data(), source, buffer.size());
    }
}

void BlockDevice::write_at(uint64_t offset, span<const byte> buffer) {
    if (mode_ == Mode::ReadOnly) {
        throw system_error(EROFS, generic_category(), "write read-only image");
    }
    if (offset > size_ || buffer.size() > size_ - offset) {
        throw out_of_range("write exceeds image bounds");
    }
    if (buffer.size() == 0) {
        return;
    }

    const byte* source = buffer.data();
    unique_ptr<byte, void(*)(void*)> aligned_storage(nullptr, free);
    if (!is_aligned(source, kDeviceAlignment) || (buffer.size() % kDeviceAlignment) != 0) {
        void* raw = allocate_aligned(buffer.size());
        aligned_storage.reset(static_cast<byte*>(raw));
        memcpy(aligned_storage.get(), source, buffer.size());
        source = aligned_storage.get();
    }

    size_t completed = 0;
    while (completed < buffer.size()) {
        const ssize_t result = pwrite(fd_, source + completed,
                                      buffer.size() - completed,
                                      static_cast<off_t>(offset + completed));
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EINVAL || errno == EOPNOTSUPP) {
                reopen_without_direct_io();
                return write_at(offset, buffer);
            }
            throw_errno("write image");
        }
        completed += static_cast<size_t>(result);
    }
}

void BlockDevice::read_at_async(uint64_t offset, span<byte> buffer, AsyncIo& io) const {
    if (offset > size_ || buffer.size() > size_ - offset) {
        throw out_of_range("async read exceeds image bounds");
    }
    if (buffer.empty()) {
        return;
    }
    if ((buffer.size() % kDeviceAlignment) != 0) {
        throw invalid_argument("async read requires a 4 KiB-aligned length");
    }

    void* aligned = AsyncIo::allocate_aligned_buffer(buffer.size());
    auto* request = io.submit_read(fd_, offset, aligned, buffer.size());
    io.submit();
    if (!io.wait_for(*request)) {
        AsyncIo::free_aligned_buffer(aligned);
        throw runtime_error("async read failed");
    }
    std::memcpy(buffer.data(), aligned, buffer.size());
    AsyncIo::free_aligned_buffer(aligned);
}

void BlockDevice::write_at_async(uint64_t offset, span<const byte> buffer, AsyncIo& io) {
    if (mode_ == Mode::ReadOnly) {
        throw system_error(EROFS, generic_category(), "write read-only image");
    }
    if (offset > size_ || buffer.size() > size_ - offset) {
        throw out_of_range("async write exceeds image bounds");
    }
    if (buffer.empty()) {
        return;
    }
    if ((buffer.size() % kDeviceAlignment) != 0) {
        throw invalid_argument("async write requires a 4 KiB-aligned length");
    }

    void* aligned = AsyncIo::allocate_aligned_buffer(buffer.size());
    std::memcpy(aligned, buffer.data(), buffer.size());
    auto* request = io.submit_write(fd_, offset, aligned, buffer.size());
    io.submit();
    if (!io.wait_for(*request)) {
        AsyncIo::free_aligned_buffer(aligned);
        throw runtime_error("async write failed");
    }
    AsyncIo::free_aligned_buffer(aligned);
}

void BlockDevice::flush() {
    if (fd_ >= 0 && fsync(fd_) != 0) {
        throw_errno("flush image");
    }
}

void BlockDevice::close() const {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

} // namespace fs end
