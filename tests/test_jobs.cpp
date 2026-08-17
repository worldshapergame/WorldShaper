#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <numeric>
#include <thread>
#include <vector>

#include "core/jobs.hpp"

using namespace ws;

TEST_CASE("dispatched jobs all run") {
    JobSystem jobs(4);
    std::atomic<i32> counter{0};
    JobCounter handle;
    for (i32 i = 0; i < 1000; ++i) {
        jobs.dispatch([&counter] { counter.fetch_add(1, std::memory_order_relaxed); }, handle);
    }
    jobs.wait(handle);
    CHECK(counter.load() == 1000);
}

TEST_CASE("parallel_for covers every index exactly once") {
    JobSystem jobs(4);
    constexpr usize kCount = 100000;
    std::vector<i32> touched(kCount, 0);

    jobs.parallel_for(kCount, 64, [&touched](usize begin, usize end) {
        for (usize i = begin; i < end; ++i) touched[i] += 1;
    });

    for (usize i = 0; i < kCount; ++i) REQUIRE(touched[i] == 1);
}

TEST_CASE("parallel_for produces the same result as a serial loop") {
    JobSystem jobs(4);
    constexpr usize kCount = 50000;
    std::vector<i64> values(kCount);
    std::iota(values.begin(), values.end(), 1);

    std::atomic<i64> parallel_sum{0};
    jobs.parallel_for(kCount, 128, [&values, &parallel_sum](usize begin, usize end) {
        i64 local = 0;
        for (usize i = begin; i < end; ++i) local += values[i];
        parallel_sum.fetch_add(local, std::memory_order_relaxed);
    });

    const i64 serial_sum = std::accumulate(values.begin(), values.end(), i64{0});
    CHECK(parallel_sum.load() == serial_sum);
}

TEST_CASE("small workloads stay on the calling thread") {
    JobSystem jobs(4);
    const u64 before = jobs.jobs_executed();
    jobs.parallel_for(10, 1000, [](usize, usize) {});
    // min_chunk exceeds the count, so nothing should have been queued.
    CHECK(jobs.jobs_executed() == before);
}

TEST_CASE("zero-length work is a no-op") {
    JobSystem jobs(2);
    bool ran = false;
    jobs.parallel_for(0, 1, [&ran](usize, usize) { ran = true; });
    CHECK_FALSE(ran);
}

TEST_CASE("worker count defaults to leaving headroom for main and sim threads") {
    JobSystem jobs;  // 0 means auto
    CHECK(jobs.worker_count() >= 1);
}

TEST_CASE("nested waiting does not deadlock") {
    // A thread that waits must keep helping, or a job that enqueues more work stalls the
    // whole pool. This is the classic job-system deadlock and it must be impossible here.
    JobSystem jobs(2);
    std::atomic<i32> done{0};
    JobCounter outer;
    for (i32 i = 0; i < 32; ++i) {
        jobs.dispatch(
            [&jobs, &done] {
                JobCounter inner;
                for (i32 k = 0; k < 8; ++k) {
                    jobs.dispatch([&done] { done.fetch_add(1, std::memory_order_relaxed); },
                                  inner);
                }
                jobs.wait(inner);
            },
            outer);
    }
    jobs.wait(outer);
    CHECK(done.load() == 32 * 8);
}

// The pool notices when two threads submit to it at once, and a second pool is the cure.
//
// This is D511's gate. `parallel_for` queues a take-LOOP over the whole range rather than a slice
// of it, so a worker that picks one up is inside it until that submitter's range is exhausted --
// which means a second submitter gets no workers at all until the first one has finished, and
// `wait()` hands it the first one's jobs to run on its own thread. On the region paste, on the main
// thread, that was 7,076 ms of stall for 75 ms of work.
//
// Written the way trap 15 says to write it: the case that must be clean is checked against a case
// that must NOT be, because a detector that never fires and a pool that never collides look
// identical from the clean side alone.
TEST_CASE("a pool with two submitters says so, and two pools do not collide") {
    // The overlap is arranged rather than raced for. A first version simply started both threads
    // and hoped they were inside at the same moment, which passes on this machine and is a
    // coin-toss on a busier or a smaller one -- and a flaky test in a suite this size is worse
    // than no test, because the next person to see it fail will re-run it rather than read it.
    //
    // So the first submitter's own body refuses to finish until the collision has been recorded,
    // which is exactly the state under test, and the wait is bounded so a broken detector fails
    // the case instead of hanging the suite.
    JobSystem background(2);
    std::atomic<JobSystem*> watched{nullptr};
    std::atomic<bool> second_in{false};
    std::atomic<bool> holding{false};

    // Only the first chunk to arrive holds the pass open; the rest pass straight through. One
    // waiter rather than one per chunk, so a detector that never fires costs the suite one bounded
    // spin instead of one per piece of the range.
    const auto hold = [&](usize, usize) {
        if (holding.exchange(true, std::memory_order_acq_rel)) return;
        JobSystem* const pool = watched.load(std::memory_order_acquire);
        // THIRTY SECONDS AND NOT FIVE, and the number is about the machine rather than the code.
        //
        // The collision is recorded when the second thread REACHES `parallel_for`, so all this wait
        // has to do is hold the first pass open until that happens. Five seconds is forever on an
        // idle box and not always enough on a busy one: with twenty other processes on four cores
        // the second thread was not scheduled inside the deadline, the wait expired, the pass
        // closed, and `submitter_collisions() > 0` failed on a build with nothing wrong with it.
        // Measured here at load average 30: one run in three.
        //
        // A test that fails at random is worse than no test, because it teaches everybody to re-run
        // it and then to disbelieve it. This costs nothing when the detector works -- the loop
        // returns the instant the collision lands -- and only a broken detector ever pays the full
        // wait, which is exactly the case that should be slow and loud.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (std::chrono::steady_clock::now() < deadline) {
            if (second_in.load(std::memory_order_acquire) &&
                (pool == nullptr || pool->submitter_collisions() > 0)) {
                return;
            }
            std::this_thread::yield();
        }
    };

    // Comfortably past the point where parallel_for stops dispatching and just runs the body on
    // the calling thread, which would take the collision check out of the picture entirely.
    const usize kRange = 64;

    SUBCASE("one pool, two threads") {
        watched.store(&background, std::memory_order_release);
        std::thread other([&] {
            second_in.store(true, std::memory_order_release);
            background.parallel_for(kRange, 1, [](usize, usize) {});
        });
        background.parallel_for(kRange, 1, hold);
        other.join();
        CHECK(background.submitter_collisions() > 0);
    }

    SUBCASE("two pools, two threads") {
        // Nothing to watch: the point is that the foreground pool never sees the other submitter
        // at all, so `hold` waits only for the second thread to have started its own pass.
        JobSystem foreground(2);
        std::thread other([&] {
            second_in.store(true, std::memory_order_release);
            background.parallel_for(kRange, 1, [](usize, usize) {});
        });
        foreground.parallel_for(kRange, 1, hold);
        other.join();
        CHECK(background.submitter_collisions() == 0);
        CHECK(foreground.submitter_collisions() == 0);
    }
}
