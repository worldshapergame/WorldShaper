#pragma once
// Single-producer / single-consumer lock-free ring buffer.
//
// documentation/02-architecture-overview.md, "Threading": cross-thread data moves through
// SPSC rings. There are no mutexes on hot paths — a mutex held during a frame is a
// hitch waiting to happen, and hitches are the thing this project cannot afford.

#include <atomic>
#include <new>
#include <type_traits>

#include "core/assert.hpp"
#include "core/types.hpp"

namespace ws {

// Capacity must be a power of two. One slot is left unused so full and empty are
// distinguishable without an extra counter.
template <typename T, usize Capacity>
class SpscRing {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");
    static_assert(Capacity >= 2, "Capacity must be at least 2");
    static_assert(std::is_trivially_copyable_v<T>,
                  "SPSC ring holds trivially copyable payloads only");

public:
    bool push(const T& value) noexcept {
        const usize head = head_.load(std::memory_order_relaxed);
        const usize next = (head + 1) & kMask;
        if (next == tail_.load(std::memory_order_acquire)) return false;  // full
        slots_[head] = value;
        head_.store(next, std::memory_order_release);
        return true;
    }

    bool pop(T& out) noexcept {
        const usize tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire)) return false;  // empty
        out = slots_[tail];
        tail_.store((tail + 1) & kMask, std::memory_order_release);
        return true;
    }

    bool empty() const noexcept {
        return head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_acquire);
    }

    usize size() const noexcept {
        const usize head = head_.load(std::memory_order_acquire);
        const usize tail = tail_.load(std::memory_order_acquire);
        return (head - tail) & kMask;
    }

    static constexpr usize capacity() noexcept { return Capacity - 1; }

private:
    static constexpr usize kMask = Capacity - 1;
    // Keep the two cursors on separate cache lines so the producer and consumer never
    // fight over the same line.
    static constexpr usize kCacheLine = 64;

    alignas(kCacheLine) std::atomic<usize> head_{0};
    alignas(kCacheLine) std::atomic<usize> tail_{0};
    alignas(kCacheLine) T slots_[Capacity]{};
};

}  // namespace ws
