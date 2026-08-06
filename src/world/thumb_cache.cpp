#include "world/thumb_cache.hpp"

#include <algorithm>
#include <cstdlib>

#include "core/assert.hpp"
#include "world/voxel_type.hpp"

namespace ws {

namespace {

i64 floor_mod(i64 value, i64 modulus) {
    const i64 rem = value % modulus;
    return (rem < 0) ? rem + modulus : rem;
}

i64 distance_sq(const ChunkCoord& a, const ChunkCoord& b) {
    const i64 dx = a.x - b.x;
    const i64 dy = a.y - b.y;
    const i64 dz = a.z - b.z;
    return dx * dx + dy * dy + dz * dz;
}

}  // namespace

void ThumbnailBatch::clear() {
    slots.clear();
    grid_cells.clear();
    built = 0;
    evicted = 0;
    wanted = 0;
}

void ThumbnailCache::create(const ThumbnailBudget& budget, const VoxelTypeTable& types) {
    budget_ = budget;
    types_ = &types;

    const usize cells = static_cast<usize>(budget_.grid_width) * budget_.grid_height *
                        budget_.grid_depth;
    grid_.assign(cells, kNoThumb);
    records_.assign(budget_.max_thumbs, GpuThumbRecord{});
    cells_.assign(static_cast<usize>(budget_.max_thumbs) * kThumbCells, 0u);
    free_slots_.clear();
    slot_cursor_ = 0;
    slot_of_.clear();
    wanted_.clear();
    scanned_ = false;
}

u32 ThumbnailCache::grid_index(i64 chunk_x, i64 chunk_y, i64 chunk_z) const {
    const i64 gx = floor_mod(chunk_x, budget_.grid_width);
    const i64 gy = floor_mod(chunk_y, budget_.grid_height);
    const i64 gz = floor_mod(chunk_z, budget_.grid_depth);
    return static_cast<u32>((gz * budget_.grid_height + gy) * budget_.grid_width + gx);
}

bool ThumbnailCache::resident(const ChunkCoord& coord) const {
    return slot_of_.find(coord) != slot_of_.end();
}

void ThumbnailCache::invalidate(const ChunkCoord& coord) {
    const auto found = slot_of_.find(coord);
    // Zeroing the revision rather than dropping the slot: the thumbnail stays on screen,
    // one edit out of date, until the new one is built. Dropping it would blink the chunk
    // out of the distance for a frame or two, which is more noticeable than being slightly
    // stale — and a big edit invalidates hundreds at once.
    if (found != slot_of_.end()) found->second.revision = 0;
}

void ThumbnailCache::defer_last_batch() {
    pending_slots_.insert(pending_slots_.end(), batch_.slots.begin(), batch_.slots.end());
    pending_grid_.insert(pending_grid_.end(), batch_.grid_cells.begin(),
                         batch_.grid_cells.end());
}

void ThumbnailCache::release(const ChunkCoord& coord) {
    const auto found = slot_of_.find(coord);
    if (found == slot_of_.end()) return;

    const u32 slot = found->second.slot;
    records_[slot] = GpuThumbRecord{};
    free_slots_.push_back(slot);
    batch_.slots.push_back(slot);

    const u32 cell = grid_index(coord.x, coord.y, coord.z);
    if (grid_[cell] == slot) {
        grid_[cell] = kNoThumb;
        batch_.grid_cells.push_back(cell);
    }
    slot_of_.erase(found);
}

u32 ThumbnailCache::acquire_slot(i64 distance, const ChunkCoord& centre) {
    if (!free_slots_.empty()) {
        const u32 slot = free_slots_.back();
        free_slots_.pop_back();
        return slot;
    }
    if (slot_cursor_ < budget_.max_thumbs) return slot_cursor_++;

    // Full. Give the slot to whichever chunk is furthest away, and only if this one is
    // nearer — otherwise the nearest chunks would be evicted by ones behind them and the
    // set would churn without ever settling.
    ChunkCoord victim{};
    bool found_victim = false;
    i64 furthest_distance = distance;
    for (const auto& entry : slot_of_) {
        const i64 d = distance_sq(entry.first, centre);
        if (d > furthest_distance) {
            furthest_distance = d;
            victim = entry.first;
            found_victim = true;
        }
    }
    if (!found_victim) return kNoThumb;

    release(victim);
    ++batch_.evicted;
    WS_ASSERT(!free_slots_.empty(), "eviction must free a slot");
    const u32 slot = free_slots_.back();
    free_slots_.pop_back();
    return slot;
}

void ThumbnailCache::rescan(const World& world, const ChunkCoord& centre) {
    wanted_.clear();
    const i64 radius = budget_.radius_chunks;
    const i64 radius_sq = radius * radius;

    world.for_each_chunk([&](const ChunkCoord& coord, const Chunk& chunk) {
        if (chunk.empty()) return;
        const i64 d = distance_sq(coord, centre);
        if (d > radius_sq) return;
        wanted_.emplace_back(d, coord);
    });

    // Nearest first. What you are standing next to matters more than what is on the horizon,
    // and with a budget that cannot hold everything the order is the whole policy.
    std::sort(wanted_.begin(), wanted_.end(), [](const auto& a, const auto& b) {
        if (a.first != b.first) return a.first < b.first;
        if (a.second.z != b.second.z) return a.second.z < b.second.z;
        if (a.second.y != b.second.y) return a.second.y < b.second.y;
        return a.second.x < b.second.x;
    });

    scanned_at_ = centre;
    scanned_ = true;
}

const ThumbnailBatch& ThumbnailCache::update(const World& world, const ChunkCoord& centre,
                                             u64 frame) {
    batch_.clear();
    if (types_ == nullptr) return batch_;

    // Anything a previous frame could not fit goes out again first.
    batch_.slots.swap(pending_slots_);
    batch_.grid_cells.swap(pending_grid_);
    pending_slots_.clear();
    pending_grid_.clear();

    const bool moved = std::abs(centre.x - scanned_at_.x) > budget_.rescan_margin ||
                       std::abs(centre.y - scanned_at_.y) > budget_.rescan_margin ||
                       std::abs(centre.z - scanned_at_.z) > budget_.rescan_margin;
    const bool world_settled = world_dirty_ && frame >= last_scan_frame_ + kRescanInterval;
    if (!scanned_ || moved || world_settled) {
        rescan(world, centre);
        last_scan_frame_ = frame;
        world_dirty_ = false;
    }

    for (const auto& [distance, coord] : wanted_) {
        const Chunk* chunk = world.chunk(coord);
        if (chunk == nullptr) continue;

        const auto held = slot_of_.find(coord);
        if (held != slot_of_.end() && held->second.revision == chunk->revision()) continue;

        ++batch_.wanted;
        if (batch_.built >= budget_.max_builds_per_frame) continue;

        u32 slot = kNoThumb;
        if (held != slot_of_.end()) {
            slot = held->second.slot;   // rebuilding in place, so it never blinks out
        } else {
            slot = acquire_slot(distance, centre);
            if (slot == kNoThumb) break;   // full of nearer chunks; nothing further will fit
        }

        const Thumbnail thumb = build_thumbnail(*chunk, *types_);
        std::copy(std::begin(thumb.cells), std::end(thumb.cells),
                  cells_.begin() + static_cast<usize>(slot) * kThumbCells);

        records_[slot].x = static_cast<i32>(coord.x);
        records_[slot].y = static_cast<i32>(coord.y);
        records_[slot].z = static_cast<i32>(coord.z);
        records_[slot].used = 1;
        slot_of_[coord] = Held{slot, chunk->revision()};

        const u32 cell = grid_index(coord.x, coord.y, coord.z);
        if (grid_[cell] != slot) {
            // Another chunk wrapped onto this cell. It keeps its slot and its contents, and
            // simply stops being findable — the record check makes a stale cell read as "no
            // thumbnail". Nothing is corrupted and nothing is invented; the far chunk is
            // drawn one tier coarser until the grid comes back round to it.
            grid_[cell] = slot;
            batch_.grid_cells.push_back(cell);
        }
        batch_.slots.push_back(slot);
        ++batch_.built;
    }

    return batch_;
}

u32 ThumbnailCache::sample(const ChunkCoord& coord, u32 x, u32 y, u32 z) const {
    const u32 cell = grid_index(coord.x, coord.y, coord.z);
    const u32 slot = grid_[cell];
    if (slot == kNoThumb) return 0;

    const GpuThumbRecord& record = records_[slot];
    if (record.used == 0 || record.x != static_cast<i32>(coord.x) ||
        record.y != static_cast<i32>(coord.y) || record.z != static_cast<i32>(coord.z)) {
        return 0;
    }
    return cells_[static_cast<usize>(slot) * kThumbCells + thumb_index(x, y, z)];
}

bool ThumbnailCache::validate() const {
    if (slot_of_.size() > budget_.max_thumbs) return false;

    std::vector<u32> seen;
    seen.reserve(slot_of_.size());
    for (const auto& [coord, held] : slot_of_) {
        if (held.slot >= budget_.max_thumbs) return false;
        const GpuThumbRecord& record = records_[held.slot];
        if (record.used == 0) return false;
        if (record.x != static_cast<i32>(coord.x) || record.y != static_cast<i32>(coord.y) ||
            record.z != static_cast<i32>(coord.z)) {
            return false;
        }
        seen.push_back(held.slot);
    }

    std::sort(seen.begin(), seen.end());
    if (std::adjacent_find(seen.begin(), seen.end()) != seen.end()) return false;   // shared slot

    for (u32 slot : free_slots_) {
        if (slot >= budget_.max_thumbs) return false;
        if (records_[slot].used != 0) return false;   // free but still claimed
    }
    return true;
}

}  // namespace ws
