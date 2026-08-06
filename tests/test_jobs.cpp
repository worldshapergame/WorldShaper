#include <doctest/doctest.h>

#include <atomic>
#include <numeric>
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
