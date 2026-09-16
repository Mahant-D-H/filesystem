#include "fs/buffer_pool.hpp"

#include "fs/async_io.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <system_error>

namespace fs {

PageGuard::PageGuard(BufferPool* pool, size_t frame_index) noexcept:
    pool_(pool), frame_index_(frame_index) {}

PageGuard::PageGuard(PageGuard&& other) noexcept:
    pool_(other.pool_), frame_index_(other.frame_index_) {
    other.pool_ = nullptr;
}

PageGuard& PageGuard::operator=(PageGuard&& other) noexcept {
    if (this != &other) {
        reset();
        pool_ = other.pool_;
        frame_index_ = other.frame_index_;
        other.pool_ = nullptr;
    }
    return *this;
}

PageGuard::~PageGuard() {
    reset();
}

std::byte* PageGuard::data() noexcept {
    return pool_ == nullptr ? nullptr : pool_->frames_[frame_index_].data.get();
}

const std::byte* PageGuard::data() const noexcept {
    return pool_ == nullptr ? nullptr : pool_->frames_[frame_index_].data.get();
}

uint64_t PageGuard::block_number() const noexcept {
    return pool_ == nullptr ? 0 : pool_->frames_[frame_index_].block_number;
}

PageGuard::operator bool() const noexcept {
    return pool_ != nullptr;
}

void PageGuard::mark_dirty() noexcept {
    if (pool_ != nullptr) {
        pool_->mark_dirty(frame_index_);
    }
}

void PageGuard::reset() noexcept {
    if (pool_ != nullptr) {
        pool_->unpin(frame_index_);
        pool_ = nullptr;
    }
}

BufferPool::BufferPool(BlockDevice& device, size_t frame_count): device_(device) {
    if (frame_count == 0) {
        throw std::invalid_argument("buffer pool requires at least one frame");
    }
    frames_.resize(frame_count);
    for (Frame& frame : frames_) {
        frame.data.reset(static_cast<std::byte*>(AsyncIo::allocate_aligned_buffer(kBlockSize)));
    }
    // Keep a small FIFO probationary set. The remainder is the hot LRU set.
    cold_capacity_ = std::max<size_t>(1, frame_count / 4);
    flush_thread_ = std::thread(&BufferPool::flush_worker, this);
}

BufferPool::~BufferPool() {
    try {
        flush_all();
    } catch (...) {
        // Explicit callers should use flush_all() to receive I/O errors.
    }
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    flush_cv_.notify_all();
    if (flush_thread_.joinable()) {
        flush_thread_.join();
    }
}

PageGuard BufferPool::fetch(uint64_t block_number) {
    if (block_number >= device_.size() / kBlockSize) {
        throw std::out_of_range("buffer-pool block is outside the device");
    }

    std::lock_guard lock(mutex_);
    for (size_t index = 0; index < frames_.size(); ++index) {
        Frame& frame = frames_[index];
        if (frame.valid && frame.block_number == block_number) {
            ++frame.pin_count;
            record_access_locked(index);
            return PageGuard(this, index);
        }
    }

    const size_t victim_index = choose_victim_locked();
    Frame& victim = frames_[victim_index];
    const bool was_in_history = remove_from_history_locked(block_number);
    if (victim.valid) {
        if (victim.queue_class == QueueClass::Cold) {
            remember_cold_eviction_locked(victim.block_number);
        }
        remove_from_queue_locked(victim_index);
    }
    device_.read_at(block_number * kBlockSize, std::span<std::byte>(victim.data.get(), kBlockSize));
    victim.block_number = block_number;
    victim.pin_count = 1;
    victim.valid = true;
    victim.dirty = false;
    place_in_queue_locked(victim_index, was_in_history ? QueueClass::Hot : QueueClass::Cold);
    return PageGuard(this, victim_index);
}

void BufferPool::flush_all() {
    std::unique_lock lock(mutex_);
    rethrow_flush_error_locked();
    for (size_t index = 0; index < frames_.size(); ++index) {
        schedule_flush_locked(index);
    }
    flush_cv_.notify_one();
    flush_cv_.wait(lock, [this] {
        return (flush_queue_.empty() && active_flushes_ == 0) || flush_error_ != nullptr;
    });
    rethrow_flush_error_locked();
    for (const Frame& frame : frames_) {
        if (frame.dirty) {
            throw std::runtime_error("buffer pool page changed while flush_all was running");
        }
    }
    lock.unlock();
    device_.flush();
}

size_t BufferPool::frame_count() const noexcept {
    return frames_.size();
}

void BufferPool::unpin(size_t frame_index) noexcept {
    std::lock_guard lock(mutex_);
    Frame& frame = frames_[frame_index];
    if (frame.pin_count > 0) {
        --frame.pin_count;
    }
    if (frame.pin_count == 0) {
        schedule_flush_locked(frame_index);
        flush_cv_.notify_one();
    }
}

void BufferPool::mark_dirty(size_t frame_index) noexcept {
    std::lock_guard lock(mutex_);
    Frame& frame = frames_[frame_index];
    frame.dirty = true;
    ++frame.dirty_generation;
}

size_t BufferPool::choose_victim_locked() {
    for (size_t index = 0; index < frames_.size(); ++index) {
        const Frame& frame = frames_[index];
        if (!frame.valid) {
            return index;
        }
    }

    // 2Q gives cold FIFO pages first eviction priority. Its fixed small size
    // prevents a one-pass scan from taking over the hot working set.
    if (cold_queue_.size() >= cold_capacity_) {
        if (const auto victim = find_evictable_locked(cold_queue_)) {
            return *victim;
        }
        if (const auto victim = find_evictable_locked(hot_queue_)) {
            return *victim;
        }
    } else {
        if (const auto victim = find_evictable_locked(cold_queue_)) {
            return *victim;
        }
        if (const auto victim = find_evictable_locked(hot_queue_)) {
            return *victim;
        }
    }
    throw std::runtime_error("buffer pool has no unpinned frame available for eviction");
}

std::optional<size_t> BufferPool::find_evictable_locked(const std::deque<size_t>& queue) const {
    for (const size_t index : queue) {
        const Frame& frame = frames_[index];
        if (frame.pin_count == 0 && !frame.dirty && !frame.flush_queued) {
            return index;
        }
    }
    return std::nullopt;
}

void BufferPool::record_access_locked(size_t frame_index) {
    Frame& frame = frames_[frame_index];
    if (frame.queue_class == QueueClass::Cold) {
        remove_from_queue_locked(frame_index);
        place_in_queue_locked(frame_index, QueueClass::Hot);
    } else if (frame.queue_class == QueueClass::Hot) {
        auto position = std::find(hot_queue_.begin(), hot_queue_.end(), frame_index);
        hot_queue_.erase(position);
        hot_queue_.push_back(frame_index);
    }
}

void BufferPool::place_in_queue_locked(size_t frame_index, QueueClass queue_class) {
    Frame& frame = frames_[frame_index];
    frame.queue_class = queue_class;
    if (queue_class == QueueClass::Cold) {
        cold_queue_.push_back(frame_index);
    } else if (queue_class == QueueClass::Hot) {
        hot_queue_.push_back(frame_index);
    }
}

void BufferPool::remove_from_queue_locked(size_t frame_index) {
    Frame& frame = frames_[frame_index];
    auto& queue = frame.queue_class == QueueClass::Cold ? cold_queue_ : hot_queue_;
    if (frame.queue_class != QueueClass::None) {
        const auto position = std::find(queue.begin(), queue.end(), frame_index);
        if (position != queue.end()) {
            queue.erase(position);
        }
    }
    frame.queue_class = QueueClass::None;
}

bool BufferPool::remove_from_history_locked(uint64_t block_number) {
    const auto position = std::find(cold_history_.begin(), cold_history_.end(), block_number);
    if (position == cold_history_.end()) {
        return false;
    }
    cold_history_.erase(position);
    return true;
}

void BufferPool::remember_cold_eviction_locked(uint64_t block_number) {
    remove_from_history_locked(block_number);
    cold_history_.push_back(block_number);
    if (cold_history_.size() > frames_.size()) {
        cold_history_.pop_front();
    }
}

void BufferPool::schedule_flush_locked(size_t frame_index) {
    Frame& frame = frames_[frame_index];
    if (!frame.valid || !frame.dirty || frame.pin_count != 0 || frame.flush_queued) {
        return;
    }
    FlushJob job{frame_index, frame.block_number, frame.dirty_generation, std::vector<std::byte>(kBlockSize)};
    std::memcpy(job.snapshot.data(), frame.data.get(), kBlockSize);
    frame.flush_queued = true;
    flush_queue_.push_back(std::move(job));
}

void BufferPool::flush_worker() {
    // A restricted runtime can deny io_uring setup. Retain the background-flush
    // contract with synchronous device I/O in that case.
    std::unique_ptr<AsyncIo> async_io;
    try {
        async_io = std::make_unique<AsyncIo>();
    } catch (const std::system_error&) {
    }

    while (true) {
        FlushJob job{};
        {
            std::unique_lock lock(mutex_);
            flush_cv_.wait(lock, [this] { return stopping_ || !flush_queue_.empty(); });
            if (stopping_ && flush_queue_.empty()) {
                return;
            }
            job = std::move(flush_queue_.front());
            flush_queue_.pop_front();
            ++active_flushes_;
        }

        std::exception_ptr error;
        try {
            if (async_io) {
                device_.write_at_async(job.block_number * kBlockSize, job.snapshot, *async_io);
                async_io->clear_completed();
            } else {
                device_.write_at(job.block_number * kBlockSize, job.snapshot);
            }
        } catch (...) {
            error = std::current_exception();
        }

        {
            std::lock_guard lock(mutex_);
            --active_flushes_;
            Frame& frame = frames_[job.frame_index];
            if (frame.valid && frame.block_number == job.block_number) {
                frame.flush_queued = false;
                if (error == nullptr && frame.dirty_generation == job.dirty_generation) {
                    frame.dirty = false;
                }
                if (frame.pin_count == 0 && frame.dirty && error == nullptr) {
                    schedule_flush_locked(job.frame_index);
                }
            }
            if (error != nullptr && flush_error_ == nullptr) {
                flush_error_ = error;
            }
        }
        flush_cv_.notify_all();
    }
}

void BufferPool::rethrow_flush_error_locked() {
    if (flush_error_ != nullptr) {
        std::rethrow_exception(flush_error_);
    }
}

} // namespace fs
