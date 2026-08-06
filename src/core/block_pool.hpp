#pragma once
// Segregated-fit block allocator over a fixed byte budget.
//
// documentation/03-voxel-data-model.md §8: all pools are fixed-size, allocated at startup
// from detected memory, and the game never allocates GPU memory during play. Pool
// pressure causes eviction, never a stall.
//
// This is the allocator that sits inside those pools. Brick payloads come in a small
// number of sizes, so a free list per size class is exactly right: allocation and release
// are both O(1), there is no coalescing pass to hitch on, and fragmentation is bounded by
// the class table rather than unbounded.
//
// The pool hands out *offsets*, not pointers. It never touches the memory it describes,
// which is what lets the same code manage a GPU buffer it cannot dereference.

#include <unordered_map>
#include <vector>

#include "core/types.hpp"

namespace ws {

inline constexpr u32 kBlockGranularity = 64;   // every offset is 64-byte aligned
inline constexpr u32 kNoOffset = 0xFFFFFFFFu;

struct BlockPoolStats {
    u64 capacity = 0;
    u64 in_use = 0;        // bytes handed out, counting class rounding
    u64 requested = 0;     // bytes actually asked for
    u64 upgrades = 0;      // times a bigger class was used because the exact one was empty
    u64 high_water = 0;
    u64 free_listed = 0;   // bytes sitting on free lists, available for reuse
    u32 live_allocations = 0;
    u64 allocations = 0;
    u64 releases = 0;
    u64 failures = 0;

    // How much of what we hold is wasted on class rounding. Budgets are written against
    // in_use, so this is the number that says whether the class table is well chosen.
    f64 waste_fraction() const {
        return (in_use > 0) ? 1.0 - static_cast<f64>(requested) / static_cast<f64>(in_use)
                            : 0.0;
    }
};

class BlockPool {
public:
    // Class sizes in bytes. Must be ascending multiples of kBlockGranularity. The default
    // table is shaped around real brick payload sizes — see world/gpu_brick.hpp.
    static const std::vector<u32>& default_classes();

    void create(u64 capacity_bytes, const std::vector<u32>& classes = default_classes());
    void reset();

    // Returns kNoOffset when the pool is full. Callers evict and retry; they never grow
    // the pool, because growing it during play is the stall this design exists to avoid.
    u32 allocate(u32 size_bytes);
    void release(u32 offset, u32 size_bytes);

    u32 class_size_for(u32 size_bytes) const;
    u64 capacity() const { return capacity_; }
    BlockPoolStats stats() const;

    // CI invariant: no offset appears on two free lists, every free offset is inside the
    // pool and aligned, and the accounting adds up.
    bool validate() const;

private:
    u32 class_index_for(u32 size_bytes) const;

    std::vector<u32> classes_;
    std::vector<std::vector<u32>> free_lists_;
    // Blocks handed out from a larger class than their size asked for, so release can put
    // them back where they came from. Only populated once the pool is carved out and a
    // class runs dry, so the common path never touches it.
    std::unordered_map<u32, u32> upgraded_;
    u64 upgrades_ = 0;
    u64 capacity_ = 0;
    u64 cursor_ = 0;          // bump pointer for never-before-used space
    u64 in_use_ = 0;
    u64 requested_ = 0;
    u64 high_water_ = 0;
    u32 live_ = 0;
    u64 allocations_ = 0;
    u64 releases_ = 0;
    u64 failures_ = 0;
};

}  // namespace ws
