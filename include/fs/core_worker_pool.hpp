#pragma once

#include "fs/async_io.hpp"
#include "fs/extent.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <vector>

using namespace std;

namespace fs {

struct CoreWorkerContext {
    size_t worker_id;
    unsigned cpu_id;
    Extent block_shard;
    AsyncIo& io;
    ExtentAllocator& extents;
};

class CoreWorkerPool {
public:
    using Work = function<void(CoreWorkerContext&)>;

    CoreWorkerPool(size_t worker_count, uint64_t total_blocks, size_t queue_capacity = 64);
    ~CoreWorkerPool();

    CoreWorkerPool(const CoreWorkerPool&) = delete;
    CoreWorkerPool& operator=(const CoreWorkerPool&) = delete;

    future<void> submit(Work work);
    size_t worker_count() const noexcept;
    unsigned cpu_id(size_t worker_id) const;

private:
    struct Worker;
    void stop() noexcept;

    vector<unique_ptr<Worker>> workers_;
    atomic<size_t> next_worker_{0};
    atomic<bool> stopping_{false};
};

} // namespace fs
