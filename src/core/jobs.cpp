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

    const usize slices = (worker_count_ > 0) ? worker_count_ + 1 : 1;
    usize chunk = (count + slices - 1) / slices;
    if (chunk < min_chunk) chunk = min_chunk;

    if (chunk >= count) {
        body(0, count);
        return;
    }

    JobCounter counter;
    for (usize begin = chunk; begin < count; begin += chunk) {
        const usize end = (begin + chunk < count) ? begin + chunk : count;
        dispatch([&body, begin, end] { body(begin, end); }, counter);
    }
    // The calling thread takes the first chunk instead of sitting idle.
    body(0, chunk);
    wait(counter);
}

}  // namespace ws
