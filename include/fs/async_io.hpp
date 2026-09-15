#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <liburing.h>

namespace fs {

class AsyncIo {
public:
    struct Request {
        int fd = -1;
        uint64_t offset = 0;
        void* buffer = nullptr;
        size_t length = 0;
        bool is_write = false;
        bool completed = false;
        int result = 0;
    };

    explicit AsyncIo(size_t queue_depth = 32);
    ~AsyncIo();

    AsyncIo(const AsyncIo&) = delete;
    AsyncIo& operator=(const AsyncIo&) = delete;
    AsyncIo(AsyncIo&&) = delete;
    AsyncIo& operator=(AsyncIo&&) = delete;

    size_t queue_depth() const noexcept;

    Request* submit_read(int fd, uint64_t offset, void* buffer, size_t length);
    Request* submit_write(int fd, uint64_t offset, const void* buffer, size_t length);
    void submit();
    bool wait_for(Request& request);
    void wait_all();

    static void* allocate_aligned_buffer(size_t length);
    static void free_aligned_buffer(void* buffer);

private:
    void process_completion(io_uring_cqe* cqe);

    size_t queue_depth_;
    io_uring ring_{};
    std::deque<Request> requests_;
};

} // namespace fs
