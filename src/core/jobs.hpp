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
    //
    // ONE POOL, ONE SUBMITTER AT A TIME. What goes on the queue is not a slice of the range but a
    // take-LOOP over it, so a worker that picks one up stays inside it until the whole range is
    // consumed. Two threads calling this on one pool therefore do not share it -- the second one's
    // entries sit behind the first one's for the length of the FIRST one's work, and `wait` below
    // makes it worse by handing the waiting thread the other submitter's jobs to run.
    //
    // That is not theoretical: the region paste and the background clip sampler shared a pool, and
    // the paste -- on the main thread, in the frame the player is sitting in -- cost whatever the
    // sample beside it cost. 7,076 ms for 75 ms of work. It is measured in D511, and the second
    // call is now warned about rather than left to be discovered by its symptom.
    void parallel_for(usize count, usize min_chunk,
                      const std::function<void(usize begin, usize end)>& body);

    // Helps out with queued work while waiting, so a waiting thread is never idle.
    //
    // Whatever is queued, which is only the caller's own work if the caller is the only submitter.
    // See the warning above.
    void wait(const JobCounter& counter);

    u32 worker_count() const noexcept { return worker_count_; }
    u64 jobs_executed() const noexcept { return executed_.load(std::memory_order_relaxed); }

    // How many times a thread entered parallel_for while another one was already inside it.
    //
    // A count rather than a flag, and for the same reason `FaceStore::refusals` is (D507): "are
    // two threads in there *now*" is a state, and a pool that collides for four seconds of every
    // seven reads as idle at whatever moment somebody happens to ask. Nought here is the only
    // thing that means the pool was used the way it is meant to be.
    u64 submitter_collisions() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    u32 worker_count_ = 0;
    std::atomic<u64> executed_{0};

    bool try_execute_one();
};

}  // namespace ws
