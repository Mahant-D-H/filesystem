#include "fs/core_worker_pool.hpp"

#include <atomic>
#include <cerrno>
#include <memory>
#include <pthread.h>
#include <sched.h>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <utility>

using namespace std;

namespace fs {
namespace {

class TaskQueue {
public:
    struct Task {
        CoreWorkerPool::Work work;
    };

    explicit TaskQueue(size_t capacity):
        capacity_(capacity), mask_(capacity - 1), cells_(make_unique<Cell[]>(capacity)) {
            for (size_t i = 0; i < capacity_; ++i) {
                cells_[i].sequence.store(i, memory_order_relaxed);
            }
        }

    bool try_enqueue(Task&& task) noexcept {
        size_t position = enqueue_position_.load(memory_order_relaxed);
        Cell* cell = nullptr;
        while (true) {
            cell = &cells_[position & mask_];
            const size_t sequence = cell->sequence.load(memory_order_acquire);
            const intptr_t difference = static_cast<intptr_t>(sequence) - static_cast<intptr_t>(position);
            if (difference == 0) {
                if (enqueue_position_.compare_exchange_weak(position, position + 1, memory_order_relaxed)) {
                    break;
                }
            } else if (difference < 0) {
                return false;
            } else {
                position = enqueue_position_.load(memory_order_relaxed);
            }
        }

        cell->task = move(task);
        cell->sequence.store(position + 1, memory_order_release);
        return true;
    }

    bool try_dequeue(Task& task) noexcept {
        size_t position = dequeue_position_.load(memory_order_relaxed);
        Cell* cell = nullptr;
        while (true) {
            cell = &cells_[position & mask_];
            const size_t sequence = cell->sequence.load(memory_order_acquire);
            const intptr_t difference = static_cast<intptr_t>(sequence) - static_cast<intptr_t>(position + 1);
            if (difference == 0) {
                if (dequeue_position_.compare_exchange_weak(position, position + 1, memory_order_relaxed)) {
                    break;
                }
            } else if (difference < 0) {
                return false;
            } else {
                position = dequeue_position_.load(memory_order_relaxed);
            }
        }

        task = move(cell->task);
        cell->sequence.store(position + capacity_, memory_order_release);
        return true;
    }

private:
    struct Cell {
        atomic<size_t> sequence{0};
        Task task;
    };

    const size_t capacity_;
    const size_t mask_;
    unique_ptr<Cell[]> cells_;
    alignas(64) atomic<size_t> enqueue_position_{0};
    alignas(64) atomic<size_t> dequeue_position_{0};
};

vector<unsigned> available_cpus() {
    cpu_set_t affinity;
    CPU_ZERO(&affinity);
    if (sched_getaffinity(0, sizeof(affinity), &affinity) != 0) {
        throw system_error(errno, generic_category(), "sched_getaffinity");
    }

    vector<unsigned> cpus;
    for (unsigned cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
        if (CPU_ISSET(cpu, &affinity)) {
            cpus.push_back(cpu);
        }
    }
    return cpus;
}

void pin_to_cpu(unsigned cpu) {
    cpu_set_t affinity;
    CPU_ZERO(&affinity);
    CPU_SET(cpu, &affinity);
    const int result = pthread_setaffinity_np(pthread_self(), sizeof(affinity), &affinity);
    if (result != 0) {
        throw system_error(result, generic_category(), "pthread_setaffinity_np");
    }
}

bool is_power_of_two(size_t value) {
    return value != 0 && (value & (value - 1)) == 0;
}

} // namespace

struct CoreWorkerPool::Worker {
    Worker(size_t id, unsigned cpu, Extent shard, size_t queue_capacity):
        id(id), cpu(cpu), shard(shard), extents(shard.length), queue(queue_capacity), ready(started.get_future()) {}

    size_t id;
    unsigned cpu;
    Extent shard;
    ExtentAllocator extents;
    TaskQueue queue;
    promise<void> started;
    future<void> ready;
    std::thread thread;
};

CoreWorkerPool::CoreWorkerPool(size_t worker_count, uint64_t total_blocks, size_t queue_capacity) {
    if (worker_count == 0 || total_blocks == 0 || worker_count > total_blocks) {
        throw invalid_argument("worker count and block count must be non-zero and workers need a block shard");
    }
    if (!is_power_of_two(queue_capacity)) {
        throw invalid_argument("worker queue capacity must be a power of two");
    }
    const auto cpus = available_cpus();
    if (worker_count > cpus.size()) {
        throw invalid_argument("worker count exceeds the CPUs available to this process");
    }

    workers_.reserve(worker_count);
    const uint64_t blocks_per_worker = total_blocks / worker_count;
    const uint64_t extra_blocks = total_blocks % worker_count;
    uint64_t next_block = 0;
    for (size_t i = 0; i < worker_count; ++i) {
        const uint64_t shard_length = blocks_per_worker + (i < extra_blocks ? 1 : 0);
        workers_.push_back(make_unique<Worker>(i, cpus[i], Extent{next_block, shard_length}, queue_capacity));
        next_block += shard_length;
    }

    try {
        for (auto& worker : workers_) {
            Worker* const current = worker.get();
            current->thread = thread([this, current] {
                unique_ptr<AsyncIo> io;
                try {
                    pin_to_cpu(current->cpu);
                    io = make_unique<AsyncIo>();
                } catch (...) {
                    current->started.set_exception(current_exception());
                    return;
                }
                current->started.set_value();
                CoreWorkerContext context {
                    current->id,
                    current->cpu,
                    current->shard,
                    *io,
                    current->extents
                };

                TaskQueue::Task task;
                while (true) {
                    if (current->queue.try_dequeue(task)) {
                        task.work(context);
                        task = {};
                    } else if (stopping_.load(memory_order_acquire)) {
                        return;
                    } else {
                        this_thread::yield();
                    }
                }
            });
        }
        for (auto& worker : workers_) {
            worker->ready.get();
        }
    } catch (...) {
        stop();
        throw;
    }
}

CoreWorkerPool::~CoreWorkerPool() {
    stop();
}

future<void> CoreWorkerPool::submit(Work work) {
    if (!work) {
        throw invalid_argument("worker task cannot be empty");
    }
    if (stopping_.load(memory_order_acquire)) {
        throw runtime_error("worker pool is stopping");
    }

    auto completion = make_shared<promise<void>>();
    auto result = completion->get_future();
    TaskQueue::Task task{
        [work = move(work), completion](CoreWorkerContext& context) mutable {
            try {
                work(context);
                completion->set_value();
            } catch (...) {
                completion->set_exception(current_exception());
            }
        }};

    const size_t worker_index = next_worker_.fetch_add(1, memory_order_relaxed) % workers_.size();
    Worker& worker = *workers_[worker_index];
    while (!worker.queue.try_enqueue(move(task))) {
        if (stopping_.load(memory_order_acquire)) {
            throw runtime_error("worker pool stopped before accepting the task");
        }
        this_thread::yield();
    }
    return result;
}

size_t CoreWorkerPool::worker_count() const noexcept {
    return workers_.size();
}

unsigned CoreWorkerPool::cpu_id(size_t worker_id) const {
    if (worker_id >= workers_.size()) {
        throw out_of_range("worker id is outside the pool");
    }
    return workers_[worker_id]->cpu;
}

void CoreWorkerPool::stop() noexcept {
    stopping_.store(true, memory_order_release);
    for (auto& worker : workers_) {
        if (worker->thread.joinable()) {
            worker->thread.join();
        }
    }
}

} // namespace fs
