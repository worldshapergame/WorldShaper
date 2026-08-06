#include "core/block_pool.hpp"

#include <algorithm>
#include <unordered_set>

#include "core/assert.hpp"

namespace ws {

const std::vector<u32>& BlockPool::default_classes() {
    // Shaped around the payload sizes bricks actually produce (world/gpu_brick.hpp):
    // 1-bit indices need 64 B, 2-bit 128 B plus a small palette, 4-bit 256 B, 8-bit 512 B
    // plus up to 1 KB of palette, and a fully unique brick needs 2 KB. The in-between
    // classes exist so a 144-byte payload does not round up to 256.
    static const std::vector<u32> classes{64, 128, 192, 256, 384, 512, 768, 1024, 1536, 2048};
    return classes;
}

void BlockPool::create(u64 capacity_bytes, const std::vector<u32>& classes) {
    WS_CHECK(!classes.empty(), "a block pool needs at least one size class");
    for (usize i = 0; i < classes.size(); ++i) {
        WS_CHECK(classes[i] % kBlockGranularity == 0,
                 "class {} ({} bytes) is not a multiple of {}", i, classes[i],
                 kBlockGranularity);
        WS_CHECK(i == 0 || classes[i] > classes[i - 1], "classes must ascend");
    }

    classes_ = classes;
    free_lists_.assign(classes_.size(), {});
    upgraded_.clear();
    upgrades_ = 0;
    capacity_ = (capacity_bytes / kBlockGranularity) * kBlockGranularity;
    cursor_ = 0;
    in_use_ = 0;
    requested_ = 0;
    high_water_ = 0;
    live_ = 0;
    allocations_ = 0;
    releases_ = 0;
    failures_ = 0;
}

void BlockPool::reset() {
    for (std::vector<u32>& list : free_lists_) list.clear();
    upgraded_.clear();
    cursor_ = 0;
    in_use_ = 0;
    requested_ = 0;
    live_ = 0;
}

u32 BlockPool::class_index_for(u32 size_bytes) const {
    for (u32 i = 0; i < classes_.size(); ++i) {
        if (size_bytes <= classes_[i]) return i;
    }
    return 0xFFFFFFFFu;
}

u32 BlockPool::class_size_for(u32 size_bytes) const {
    const u32 index = class_index_for(size_bytes);
    return (index == 0xFFFFFFFFu) ? 0 : classes_[index];
}

u32 BlockPool::allocate(u32 size_bytes) {
    if (size_bytes == 0) return kNoOffset;

    const u32 index = class_index_for(size_bytes);
    if (index == 0xFFFFFFFFu) {
        // Larger than the biggest class. This is a programming error rather than pool
        // pressure, so it is loud even in release.
        ++failures_;
        WS_LOG_ERROR("pool", "allocation of {} bytes exceeds the largest class {}",
                     size_bytes, classes_.back());
        return kNoOffset;
    }

    const u32 block = classes_[index];
    u32 offset;

    u32 used_index = index;

    if (!free_lists_[index].empty()) {
        offset = free_lists_[index].back();
        free_lists_[index].pop_back();
    } else if (cursor_ + block <= capacity_) {
        offset = static_cast<u32>(cursor_);
        cursor_ += block;
    } else {
        // The exact class is empty and the pool is carved out. Take a bigger free block
        // rather than failing.
        //
        // Without this the pool reports "full" while holding a great deal of free space in
        // the wrong classes, and the caller — which evicts and retries — churns forever:
        // evicting a chunk with a small brick run does nothing for a chunk that needs a
        // large one. On screen that is chunks blinking in and out as you move, and it is
        // what a large edit provokes, because it makes chunks dense enough to jump several
        // classes at once.
        //
        // The waste is the difference between the two classes and is given back on release.
        // Wasting some of a block beats refusing to draw the world.
        used_index = 0xFFFFFFFFu;
        for (u32 bigger = index + 1; bigger < classes_.size(); ++bigger) {
            if (!free_lists_[bigger].empty()) {
                used_index = bigger;
                break;
            }
        }
        if (used_index == 0xFFFFFFFFu) {
            ++failures_;
            return kNoOffset;   // genuinely full: the caller evicts and retries
        }
        offset = free_lists_[used_index].back();
        free_lists_[used_index].pop_back();
        // Remembered so release puts it back on the list it actually came from. Returning a
        // large block to a small class's list would hand out overlapping allocations.
        upgraded_[offset] = used_index;
        ++upgrades_;
    }

    in_use_ += classes_[used_index];
    requested_ += size_bytes;
    ++live_;
    ++allocations_;
    if (in_use_ > high_water_) high_water_ = in_use_;
    return offset;
}

void BlockPool::release(u32 offset, u32 size_bytes) {
    if (offset == kNoOffset || size_bytes == 0) return;

    u32 index = class_index_for(size_bytes);
    WS_ASSERT(index != 0xFFFFFFFFu, "releasing a size that was never allocatable");
    if (index == 0xFFFFFFFFu) return;

    // A block that was upgraded to a larger class goes back on that larger class's list.
    // Putting it on the list its *requested* size implies would leave the rest of it
    // unaccounted for, and the next allocation from the neighbouring class would overlap it.
    const auto upgraded = upgraded_.find(offset);
    if (upgraded != upgraded_.end()) {
        index = upgraded->second;
        upgraded_.erase(upgraded);
    }

    const u32 block = classes_[index];
    WS_ASSERT(in_use_ >= block, "block pool accounting underflow");
    in_use_ -= block;
    requested_ -= std::min<u64>(requested_, size_bytes);
    --live_;
    ++releases_;
    free_lists_[index].push_back(offset);
}

BlockPoolStats BlockPool::stats() const {
    BlockPoolStats out;
    out.capacity = capacity_;
    out.in_use = in_use_;
    out.requested = requested_;
    out.upgrades = upgrades_;
    out.high_water = high_water_;
    out.live_allocations = live_;
    out.allocations = allocations_;
    out.releases = releases_;
    out.failures = failures_;
    for (u32 i = 0; i < free_lists_.size(); ++i) {
        out.free_listed += static_cast<u64>(free_lists_[i].size()) * classes_[i];
    }
    return out;
}

bool BlockPool::validate() const {
    std::unordered_set<u32> seen;
    u64 free_bytes = 0;

    for (u32 i = 0; i < free_lists_.size(); ++i) {
        for (u32 offset : free_lists_[i]) {
            if (offset % kBlockGranularity != 0) return false;
            if (offset + classes_[i] > cursor_) return false;
            if (!seen.insert(offset).second) return false;   // the same block freed twice
            free_bytes += classes_[i];
        }
    }

    if (cursor_ > capacity_) return false;
    if (in_use_ + free_bytes != cursor_) return false;
    return true;
}

}  // namespace ws
