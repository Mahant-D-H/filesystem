#include "fs/block_device.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <system_error>

namespace fs {
namespace {

[[noreturn]] void throw_errno(const char* operation) {
    throw std::system_error(errno, std::generic_category(), operation);
}

std::uint64_t file_size(int fd) {
    struct stat st {};
    if (fstat(fd, &st) != 0) {
        throw_errno("fstat");
    }
    if (st.st_size < 0) {
        throw std::runtime_error("image has a negative size");
    }
    return static_cast<std::uint64_t>(st.st_size);
}

} // namespace

BlockDevice::BlockDevice(int fd, std::uint64_t size, Mode mode)
    : fd_(fd), size_(size), mode_(mode) {}

BlockDevice::BlockDevice(BlockDevice&& other) noexcept
    : fd_(other.fd_), size_(other.size_), mode_(other.mode_) {
    other.fd_ = -1;
    other.size_ = 0;
}

BlockDevice& BlockDevice::operator=(BlockDevice&& other) noexcept {
    if (this != &other) {
        close();
        fd_ = other.fd_;
        size_ = other.size_;
        mode_ = other.mode_;
        other.fd_ = -1;
        other.size_ = 0;
    }
    return *this;
}

BlockDevice::~BlockDevice() {
    close();
}

BlockDevice BlockDevice::create(const std::string& path, std::uint64_t size) {
    if (size == 0) {
        throw std::invalid_argument("image size must be greater than zero");
    }

    const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        throw_errno("open image");
    }
    if (ftruncate(fd, static_cast<off_t>(size)) != 0) {
        const int saved_errno = errno;
        ::close(fd);
        errno = saved_errno;
        throw_errno("resize image");
    }
    return BlockDevice(fd, size, Mode::ReadWrite);
}

BlockDevice BlockDevice::open(const std::string& path, Mode mode) {
    const int flags = mode == Mode::ReadOnly ? O_RDONLY : O_RDWR;
    const int fd = ::open(path.c_str(), flags);
    if (fd < 0) {
        throw_errno("open image");
    }
    try {
        return BlockDevice(fd, file_size(fd), mode);
    } catch (...) {
        ::close(fd);
        throw;
    }
}

void BlockDevice::read_at(std::uint64_t offset, std::span<std::byte> buffer) const {
    if (offset > size_ || buffer.size() > size_ - offset) {
        throw std::out_of_range("read exceeds image bounds");
    }

    std::size_t completed = 0;
    while (completed < buffer.size()) {
        const ssize_t result = pread(fd_, buffer.data() + completed,
                                     buffer.size() - completed,
                                     static_cast<off_t>(offset + completed));
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw_errno("read image");
        }
        if (result == 0) {
            throw std::runtime_error("unexpected end of image");
        }
        completed += static_cast<std::size_t>(result);
    }
}

void BlockDevice::write_at(std::uint64_t offset, std::span<const std::byte> buffer) {
    if (mode_ == Mode::ReadOnly) {
        throw std::system_error(EROFS, std::generic_category(), "write read-only image");
    }
    if (offset > size_ || buffer.size() > size_ - offset) {
        throw std::out_of_range("write exceeds image bounds");
    }

    std::size_t completed = 0;
    while (completed < buffer.size()) {
        const ssize_t result = pwrite(fd_, buffer.data() + completed,
                                      buffer.size() - completed,
                                      static_cast<off_t>(offset + completed));
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw_errno("write image");
        }
        completed += static_cast<std::size_t>(result);
    }
}

void BlockDevice::flush() {
    if (fd_ >= 0 && fsync(fd_) != 0) {
        throw_errno("flush image");
    }
}

void BlockDevice::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

} // namespace fs
