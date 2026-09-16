#include "fs/async_io.hpp"

#include <cerrno>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <system_error>
#include <liburing.h>

using namespace std;

namespace fs {
namespace {

constexpr size_t kIoUringAlignment = 4096;

bool is_aligned(const void* pointer, size_t alignment) {
    return pointer != nullptr && (reinterpret_cast<uintptr_t>(pointer) % alignment) == 0;
}

[[noreturn]] void throw_errno(const char* operation) {
    throw system_error(errno, std::generic_category(), operation);
}

} // namespace

AsyncIo::AsyncIo(size_t queue_depth): queue_depth_(queue_depth) {
    if (queue_depth_ == 0) {
        throw invalid_argument("io_uring queue depth must be greater than zero");
    }
    const int result = io_uring_queue_init(static_cast<unsigned>(queue_depth_), &ring_, 0);
    if (result < 0) {
        throw system_error(-result, generic_category(), "io_uring_queue_init");
    }
}

AsyncIo::~AsyncIo() {
    io_uring_queue_exit(&ring_);
}

size_t AsyncIo::queue_depth() const noexcept {
    return queue_depth_;
}

AsyncIo::Request* AsyncIo::submit_read(int fd, uint64_t offset, void* buffer, size_t length) {
    if (fd < 0) {
        throw invalid_argument("async read requires a valid file descriptor");
    }
    if (buffer == nullptr || length == 0) {
        throw invalid_argument("async read requires a non-empty aligned buffer");
    }
    if (!is_aligned(buffer, kIoUringAlignment) || (length % kIoUringAlignment) != 0) {
        throw invalid_argument("async read requires a 4 KiB aligned buffer and length");
    }

    requests_.push_back(Request{});
    Request& request = requests_.back();
    request.fd = fd;
    request.offset = offset;
    request.buffer = buffer;
    request.length = length;
    request.is_write = false;
    request.completed = false;
    request.result = 0;

    io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
    if (sqe == nullptr) {
        requests_.pop_back();
        throw runtime_error("io_uring submission queue is full");
    }

    io_uring_prep_read(sqe, fd, buffer, length, static_cast<off_t>(offset));
    io_uring_sqe_set_data(sqe, &request);
    return &request;
}

AsyncIo::Request* AsyncIo::submit_write(int fd, uint64_t offset, const void* buffer, size_t length) {
    if (fd < 0) {
        throw invalid_argument("async write requires a valid file descriptor");
    }
    if (buffer == nullptr || length == 0) {
        throw invalid_argument("async write requires a non-empty aligned buffer");
    }
    if (!is_aligned(buffer, kIoUringAlignment) || (length % kIoUringAlignment) != 0) {
        throw invalid_argument("async write requires a 4 KiB aligned buffer and length");
    }

    requests_.push_back(Request{});
    Request& request = requests_.back();
    request.fd = fd;
    request.offset = offset;
    request.buffer = const_cast<void*>(buffer);
    request.length = length;
    request.is_write = true;
    request.completed = false;
    request.result = 0;

    io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
    if (sqe == nullptr) {
        requests_.pop_back();
        throw runtime_error("io_uring submission queue is full");
    }

    io_uring_prep_write(sqe, fd, buffer, length, static_cast<off_t>(offset));
    io_uring_sqe_set_data(sqe, &request);
    return &request;
}

void AsyncIo::submit() {
    const int result = io_uring_submit(&ring_);
    if (result < 0) {
        throw_errno("io_uring_submit");
    }
}

bool AsyncIo::wait_for(Request& request) {
    while (!request.completed) {
        io_uring_cqe* cqe = nullptr;
        const int result = io_uring_wait_cqe(&ring_, &cqe);
        if (result < 0) {
            if (result == -EINTR) {
                continue;
            }
            throw system_error(-result, generic_category(), "io_uring_wait_cqe");
        }
        process_completion(cqe);
        io_uring_cqe_seen(&ring_, cqe);
    }
    return request.result == static_cast<int>(request.length);
}

void AsyncIo::wait_all() {
    while (true) {
        io_uring_cqe* cqe = nullptr;
        const int result = io_uring_wait_cqe(&ring_, &cqe);
        if (result < 0) {
            if (result == -EINTR) {
                continue;
            }
            throw system_error(-result, generic_category(), "io_uring_wait_cqe");
        }
        process_completion(cqe);
        io_uring_cqe_seen(&ring_, cqe);

        bool pending = false;
        for (const Request& request : requests_) {
            if (!request.completed) {
                pending = true;
                break;
            }
        }
        if (!pending) {
            break;
        }
    }
}

void AsyncIo::clear_completed() {
    for (const Request& request : requests_) {
        if (!request.completed) {
            throw logic_error("cannot clear incomplete async I/O requests");
        }
    }
    requests_.clear();
}

void* AsyncIo::allocate_aligned_buffer(size_t length) {
    size_t aligned_length = length == 0 ? kIoUringAlignment : length;
    void* buffer = nullptr;
    if (posix_memalign(&buffer, kIoUringAlignment, aligned_length) != 0) {
        throw bad_alloc();
    }
    return buffer;
}

void AsyncIo::free_aligned_buffer(void* buffer) {
    free(buffer);
}

void AsyncIo::process_completion(io_uring_cqe* cqe) {
    if (cqe == nullptr) {
        return;
    }
    auto* request = static_cast<Request*>(io_uring_cqe_get_data(cqe));
    if (request == nullptr) {
        return;
    }
    request->result = cqe->res;
    request->completed = true;
}

} // namespace fs
