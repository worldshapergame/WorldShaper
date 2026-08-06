#pragma once
// Job system.
//
// documentation/02-architecture-overview.md, "Threading": the main thread never touches
// the disk and never waits on a job. Terrain generation, compression, save IO, network
// serialisation and brick packing all live here.
//
// Stage 0 uses a straightforward mutex + condition-variable queue. That is fast enough
// for hundreds of jobs per frame and, more importantly, obviously correct. It gets
// replaced with per-worker deques and work stealing when profiling says it matters —
// not before.

#include <atomic>
#include <functional>
#include <memory>

#include "core/types.hpp"

namespace ws {

// A handle you can wait on. Cheap to copy.
class JobCounter {
public:
    JobCounter() : remaining_(std::make_shared<std::atomic<i32>>(0)) {}

    void add(i32 n) const noexcept { remaining_->fetch_add(n, std::memory_order_relaxed); }
    void finish_one() const noexcept { remaining_->fetch_sub(1, std::memory_order_release); }
    bool done() const noexcept { return remaining_->load(std::memory_order_acquire) <= 0; }

private:
    std::shared_ptr<std::atomic<i32>> remaining_;
};

class JobSystem {
public:
    // worker_count == 0 means "hardware concurrency minus two", leaving room for the
    // main thread and the simulation thread.
    explicit JobSystem(u32 worker_count = 0);
    ~JobSystem();

    JobSystem(const JobSystem&) = delete;
    JobSystem& operator=(const JobSystem&) = delete;

    void dispatch(std::function<void()> job, const JobCounter& counter);
    void dispatch(std::function<void()> job);

    // Splits [0, count) into chunks and runs them across the pool. Blocks until done.
    // The callback receives a half-open range so it can process a batch at a time
    // instead of paying call overhead per element.
    void parallel_for(usize count, usize min_chunk,
                      const std::function<void(usize begin, usize end)>& body);

    // Helps out with queued work while waiting, so a waiting thread is never idle.
    void wait(const JobCounter& counter);

    u32 worker_count() const noexcept { return worker_count_; }
    u64 jobs_executed() const noexcept { return executed_.load(std::memory_order_relaxed); }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    u32 worker_count_ = 0;
    std::atomic<u64> executed_{0};

    bool try_execute_one();
};

}  // namespace ws
