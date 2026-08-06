#pragma once
// Linear (bump) allocator.
//
// documentation/03-voxel-data-model.md §8: the game never allocates during play. Every
// pool is sized at startup from detected memory. An Arena hands out memory by moving a
// cursor and frees everything at once — no fragmentation, no per-allocation bookkeeping,
// and a reset that costs one store.

#include <cstdlib>
#include <memory>
#include <new>
#include <utility>

#include "core/assert.hpp"
#include "core/types.hpp"

namespace ws {

class Arena {
public:
    Arena() = default;

    explicit Arena(usize capacity_bytes) { reserve(capacity_bytes); }

    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;

    Arena(Arena&& other) noexcept
        : base_(other.base_), capacity_(other.capacity_), used_(other.used_),
          high_water_(other.high_water_) {
        other.base_ = nullptr;
        other.capacity_ = 0;
        other.used_ = 0;
        other.high_water_ = 0;
    }

    Arena& operator=(Arena&& other) noexcept {
        if (this != &other) {
            release();
            base_ = other.base_;
            capacity_ = other.capacity_;
            used_ = other.used_;
            high_water_ = other.high_water_;
            other.base_ = nullptr;
            other.capacity_ = 0;
            other.used_ = 0;
            other.high_water_ = 0;
        }
        return *this;
    }

    ~Arena() { release(); }

    void reserve(usize capacity_bytes) {
        release();
        if (capacity_bytes == 0) return;
        base_ = static_cast<u8*>(::operator new(capacity_bytes, std::align_val_t{64}));
        capacity_ = capacity_bytes;
        used_ = 0;
        high_water_ = 0;
    }

    // Returns nullptr when the arena is exhausted. Callers that cannot handle exhaustion
    // should use alloc_checked instead, which fails loudly.
    void* alloc(usize size, usize alignment = 16) noexcept {
        WS_ASSERT((alignment & (alignment - 1)) == 0, "alignment must be a power of two");
        const usize aligned = (used_ + alignment - 1) & ~(alignment - 1);
        if (aligned + size > capacity_) return nullptr;
        used_ = aligned + size;
        if (used_ > high_water_) high_water_ = used_;
        return base_ + aligned;
    }

    void* alloc_checked(usize size, usize alignment = 16) {
        void* p = alloc(size, alignment);
        WS_CHECK(p != nullptr, "arena exhausted: wanted {} bytes, {} of {} used", size,
                 used_, capacity_);
        return p;
    }

    template <typename T>
    T* alloc_array(usize count) {
        return static_cast<T*>(alloc_checked(sizeof(T) * count, alignof(T)));
    }

    template <typename T, typename... Args>
    T* create(Args&&... args) {
        void* p = alloc_checked(sizeof(T), alignof(T));
        return new (p) T(std::forward<Args>(args)...);
    }

    // Note: does not run destructors. Arena types are expected to be trivially
    // destructible; anything else must be torn down explicitly before reset.
    void reset() noexcept { used_ = 0; }

    usize used() const noexcept { return used_; }
    usize capacity() const noexcept { return capacity_; }
    usize high_water() const noexcept { return high_water_; }

    // Scoped rollback: everything allocated inside the scope is released on exit.
    class Scope {
    public:
        explicit Scope(Arena& arena) noexcept : arena_(arena), mark_(arena.used_) {}
        ~Scope() noexcept { arena_.used_ = mark_; }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;

    private:
        Arena& arena_;
        usize mark_;
    };

private:
    void release() noexcept {
        if (base_ != nullptr) {
            ::operator delete(base_, std::align_val_t{64});
            base_ = nullptr;
        }
        capacity_ = 0;
        used_ = 0;
    }

    u8* base_ = nullptr;
    usize capacity_ = 0;
    usize used_ = 0;
    usize high_water_ = 0;
};

}  // namespace ws
