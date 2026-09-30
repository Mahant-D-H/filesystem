#include "fs/core_worker_pool.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <latch>
#include <sched.h>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace std;

namespace {

struct AllocationResult {
    size_t worker_id = 0;
    unsigned cpu_id = 0;
    uint64_t block = 0;
};

} // namespace

int main() {
    try {
        fs::CoreWorkerPool pool(2, 2048, 4);
        if (pool.worker_count() != 2) {
            throw runtime_error("worker pool started the wrong number of workers");
        }

        constexpr size_t producer_count = 4;
        constexpr size_t tasks_per_producer = 256;
        constexpr size_t total_tasks = producer_count * tasks_per_producer;
        array<AllocationResult, total_tasks> results{};
        array<vector<future<void>>, producer_count> futures;
        atomic<size_t> completed{0};
        vector<thread> producers;
        producers.reserve(producer_count);

        for (size_t producer = 0; producer < producer_count; ++producer) {
            producers.emplace_back([&, producer] {
                for (size_t index = 0; index < tasks_per_producer; ++index) {
                    const size_t result_index = producer * tasks_per_producer + index;
                    futures[producer].push_back(pool.submit([&, result_index](fs::CoreWorkerContext& context) {
                        if (context.io.queue_depth() == 0 ||
                            sched_getcpu() != static_cast<int>(context.cpu_id)) {
                            throw runtime_error("worker did not use its own ring or remain pinned to its CPU");
                        }
                        const auto allocation = context.extents.allocate(1);
                        if (!allocation) {
                            throw runtime_error("worker-local extent allocation failed");
                        }
                        results[result_index] = {
                            context.worker_id, context.cpu_id, context.block_shard.start + allocation->start};
                            completed.fetch_add(1, memory_order_relaxed);
                        }
                    ));
                }
            });
        }

        for (auto& producer : producers) {
            producer.join();
        }
        for (auto& producer_futures : futures) {
            for (auto& result : producer_futures) {
                result.get();
            }
        }

        if (completed.load(memory_order_relaxed) != total_tasks) {
            throw runtime_error("worker pool lost submitted tasks");
        }

        array<size_t, 2> allocations_per_worker{};
        vector<uint64_t> allocated_blocks;
        allocated_blocks.reserve(total_tasks);
        for (const auto& result : results) {
            if (result.worker_id >= allocations_per_worker.size() || result.cpu_id != pool.cpu_id(result.worker_id)) {
                throw runtime_error("task was dispatched outside its assigned worker");
            }
            const uint64_t shard_start = result.worker_id == 0 ? 0 : 1024;
            const uint64_t shard_end = result.worker_id == 0 ? 1024 : 2048;
            if (result.block < shard_start || result.block >= shard_end) {
                throw runtime_error("worker allocated a block outside its private shard");
            }
            ++allocations_per_worker[result.worker_id];
            allocated_blocks.push_back(result.block);
        }
        sort(allocated_blocks.begin(), allocated_blocks.end());
        if (adjacent_find(allocated_blocks.begin(), allocated_blocks.end()) != allocated_blocks.end() ||
            allocations_per_worker[0] == 0 ||
            allocations_per_worker[1] == 0) {
            throw runtime_error("worker shards overlapped or a worker received no work");
        }

        bool task_error_reported = false;
        try {
            pool.submit(
                [](fs::CoreWorkerContext&) {
                    throw runtime_error("expected worker failure");
                }
            ).get();
        } catch (const runtime_error&) {
            task_error_reported = true;
        }
        if (!task_error_reported) {
            throw runtime_error("worker task exception was not propagated to its future");
        }

        latch both_workers_started(2);
        auto first_concurrent_task = pool.submit([&](fs::CoreWorkerContext&) {
            both_workers_started.count_down();
            both_workers_started.wait();
        });
        auto second_concurrent_task = pool.submit([&](fs::CoreWorkerContext&) {
            both_workers_started.count_down();
            both_workers_started.wait();
        });
        first_concurrent_task.get();
        second_concurrent_task.get();

        array<future<void>, 64> shutdown_tasks;
        {
            fs::CoreWorkerPool shutdown_pool(2, 64, 4);
            for (auto& task : shutdown_tasks) {
                task = shutdown_pool.submit([](fs::CoreWorkerContext&) {
                    this_thread::sleep_for(chrono::milliseconds(1));
                });
            }
        }
        for (auto& task : shutdown_tasks) {
            task.get();
        }

        cout << "core worker pool test success: pinned workers, private io_uring and extent "
                "shards, concurrent execution, concurrent producers, shutdown draining, and "
                "task error propagation verified\n";
        return EXIT_SUCCESS;
    } catch (const exception& error) {
        cerr << "error: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
