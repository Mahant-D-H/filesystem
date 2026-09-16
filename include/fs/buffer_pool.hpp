#pragma once

#include "fs/block_device.hpp"
#include "fs/disk_format.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <condition_variable>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace fs {

// A pinned, move-only view of a 4 KiB block cached by BufferPool.
class BufferPool;

class PageGuard {
public:
    PageGuard() = default;
    PageGuard(const PageGuard&) = delete;
    PageGuard& operator=(const PageGuard&) = delete;
    PageGuard(PageGuard&& other) noexcept;
    PageGuard& operator=(PageGuard&& other) noexcept;
    ~PageGuard();

    std::byte* data() noexcept;
    const std::byte* data() const noexcept;
    uint64_t block_number() const noexcept;
    explicit operator bool() const noexcept;

    void mark_dirty() noexcept;

private:
    friend class BufferPool;
    PageGuard(BufferPool* pool, size_t frame_index) noexcept;
    void reset() noexcept;

    BufferPool* pool_ = nullptr;
    size_t frame_index_ = 0;
};

class BufferPool {
public:
    explicit BufferPool(BlockDevice& device, size_t frame_count);
    ~BufferPool();

    BufferPool(const BufferPool&) = delete;
    BufferPool& operator=(const BufferPool&) = delete;

    // Fetches and pins one filesystem block. Throws when every frame is pinned.
    PageGuard fetch(uint64_t block_number);
    void flush_all();
    size_t frame_count() const noexcept;

private:
    friend class PageGuard;

    enum class QueueClass : uint8_t { None, Cold, Hot };

    struct Frame {
        std::unique_ptr<std::byte, decltype(&std::free)> data{nullptr, &std::free};
        uint64_t block_number = 0;
        size_t pin_count = 0;
        bool valid = false;
        bool dirty = false;
        QueueClass queue_class = QueueClass::None;
        bool flush_queued = false;
        uint64_t dirty_generation = 0;
    };

    struct FlushJob {
        size_t frame_index;
        uint64_t block_number;
        uint64_t dirty_generation;
        std::vector<std::byte> snapshot;
    };

    void unpin(size_t frame_index) noexcept;
    void mark_dirty(size_t frame_index) noexcept;
    size_t choose_victim_locked();
    std::optional<size_t> find_evictable_locked(const std::deque<size_t>& queue) const;
    void record_access_locked(size_t frame_index);
    void place_in_queue_locked(size_t frame_index, QueueClass queue_class);
    void remove_from_queue_locked(size_t frame_index);
    bool remove_from_history_locked(uint64_t block_number);
    void remember_cold_eviction_locked(uint64_t block_number);
    void schedule_flush_locked(size_t frame_index);
    void flush_worker();
    void rethrow_flush_error_locked();

    BlockDevice& device_;
    std::vector<Frame> frames_;
    std::deque<size_t> cold_queue_;
    std::deque<size_t> hot_queue_;
    std::deque<uint64_t> cold_history_;
    size_t cold_capacity_ = 1;
    mutable std::mutex mutex_;
    std::condition_variable flush_cv_;
    std::deque<FlushJob> flush_queue_;
    std::thread flush_thread_;
    size_t active_flushes_ = 0;
    bool stopping_ = false;
    std::exception_ptr flush_error_;
};

} // namespace fs
