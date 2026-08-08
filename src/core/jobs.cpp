#include "core/jobs.hpp"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "core/assert.hpp"
#include "core/log.hpp"

namespace ws {

struct JobSystem::Impl {
    struct Entry {
        std::function<void()> fn;
        JobCounter counter;
        bool has_counter = false;
    };

    std::mutex mutex;
    std::condition_variable wake;
    std::deque<Entry> queue;
    std::vector<std::thread> workers;
    bool shutting_down = false;
};

JobSystem::JobSystem(u32 worker_count) : impl_(std::make_unique<Impl>()) {
    if (worker_count == 0) {
        const u32 hardware = std::thread::hardware_concurrency();
        // TWO held back, and the second one is not waste.
        //
        // Taking it was tried, on the argument that the submitting thread waits during a clip build
        // and a ten core machine was giving twenty per cent to nobody. That argument is only about
        // the build. For the whole rest of the program the submitting thread is the RENDER thread,
        // and it does not wait — it competes, along with the driver's own threads, for whatever the
        // pool has left. Nine workers on ten cores buys a one-off build a few per cent and charges
        // every frame after it for the privilege.
        worker_count = (hardware > 3) ? hardware - 2 : 1;
    }
    worker_count_ = worker_count;

    impl_->workers.reserve(worker_count_);
    for (u32 i = 0; i < worker_count_; ++i) {
        impl_->workers.emplace_back([this] {
            for (;;) {
                Impl::Entry entry;
                {
                    std::unique_lock<std::mutex> lock(impl_->mutex);
                    impl_->wake.wait(lock, [this] {
                        return impl_->shutting_down || !impl_->queue.empty();
                    });
                    if (impl_->shutting_down && impl_->queue.empty()) return;
                    entry = std::move(impl_->queue.front());
                    impl_->queue.pop_front();
                }
                entry.fn();
                executed_.fetch_add(1, std::memory_order_relaxed);
                if (entry.has_counter) entry.counter.finish_one();
            }
        });
    }

    WS_LOG_INFO("jobs", "started {} worker threads", worker_count_);
}

JobSystem::~JobSystem() {
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->shutting_down = true;
    }
    impl_->wake.notify_all();
    for (std::thread& t : impl_->workers) {
        if (t.joinable()) t.join();
    }
}

void JobSystem::dispatch(std::function<void()> job, const JobCounter& counter) {
    counter.add(1);
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->queue.push_back(Impl::Entry{std::move(job), counter, true});
    }
    impl_->wake.notify_one();
}

void JobSystem::dispatch(std::function<void()> job) {
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->queue.push_back(Impl::Entry{std::move(job), JobCounter{}, false});
    }
    impl_->wake.notify_one();
}

bool JobSystem::try_execute_one() {
    Impl::Entry entry;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (impl_->queue.empty()) return false;
        entry = std::move(impl_->queue.front());
        impl_->queue.pop_front();
    }
    entry.fn();
    executed_.fetch_add(1, std::memory_order_relaxed);
    if (entry.has_counter) entry.counter.finish_one();
    return true;
}

void JobSystem::wait(const JobCounter& counter) {
    while (!counter.done()) {
        if (!try_execute_one()) std::this_thread::yield();
    }
}

void JobSystem::parallel_for(usize count, usize min_chunk,
                             const std::function<void(usize, usize)>& body) {
    if (count == 0) return;
    if (min_chunk == 0) min_chunk = 1;

    const usize workers = (worker_count_ > 0) ? worker_count_ + 1 : 1;

    // Handed out on demand rather than divided up in advance.
    //
    // Cutting the range into one contiguous piece per worker is the obvious thing and it assumes
    // the work is spread evenly through the range. Almost nothing here is. Sampling a clip walks
    // a box of space in which the geometry sits in a layer; the workers given the sky finish
    // immediately and then sit idle while the one given the ground does everything. The measured
    // effect on the facility was most of a factor of three — eight cores doing the work of three.
    //
    // So the range is cut into many more pieces than there are workers, and each worker takes the
    // next one when it has finished the last. A worker that draws an easy piece comes back for
    // another; the range finishes when the work does, not when the unluckiest slice does.
    usize chunk = (count + workers * 8 - 1) / (workers * 8);
    if (chunk < min_chunk) chunk = min_chunk;
    if (chunk >= count) {
        body(0, count);
        return;
    }

    auto next = std::make_shared<std::atomic<usize>>(0);
    const auto take = [&body, next, chunk, count] {
        for (;;) {
            const usize begin = next->fetch_add(chunk, std::memory_order_relaxed);
            if (begin >= count) return;
            const usize end = (begin + chunk < count) ? begin + chunk : count;
            body(begin, end);
        }
    };

    JobCounter counter;
    for (usize i = 1; i < workers; ++i) dispatch(take, counter);
    // The calling thread pulls its share too rather than sitting idle.
    take();
    wait(counter);
}

}  // namespace ws
