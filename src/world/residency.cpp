#include "world/residency.hpp"

#include <algorithm>
#include <bit>
#include <cstring>

#include "core/assert.hpp"
#include "core/hash.hpp"

namespace ws {
namespace {

constexpr u32 kAxis = static_cast<u32>(kChunkBricks);

// Slot runs are measured in entries, not bytes, and a chunk can hold anywhere from one
// brick to 32,768 — so the class table spans the whole range.
const std::vector<u32>& slot_run_classes() {
    // Powers of two alone waste up to half a run: a chunk with 5,090 bricks took an
    // 8,192-slot class and left 3,102 slots stranded. The halfway classes cut the worst case
    // from 2x to 1.5x, which on a dense world is the difference between holding six hundred
    // chunks and four hundred.
    static const std::vector<u32> classes{64,   128,  192,   256,   384,   512,   768,
                                          1024, 1536, 2048,  3072,  4096,  6144,  8192,
                                          12288, 16384, 24576, 32768};
    return classes;
}

constexpr i64 floor_mod(i64 value, i64 modulus) {
    const i64 result = value % modulus;
    return (result < 0) ? result + modulus : result;
}

}  // namespace

void UploadBatch::clear() {
    slots.clear();
    records.clear();
    grid_cells.clear();
    coarse_cells.clear();
    payload.clear();
    payload_bytes = 0;
    chunks_added = 0;
    chunks_refreshed = 0;
    chunks_evicted = 0;
    bricks_added = 0;
    chunks_deferred = 0;
    out_of_memory = false;
}

void ResidencyManager::create(const ResidencyBudget& budget, const VoxelTypeTable& types) {
    budget_ = budget;
    types_ = &types;
    payload_pool_.create(budget.payload_bytes);
    slot_pool_.create(budget.max_bricks, slot_run_classes());

    // Fixed-size mirrors, allocated once. The game never allocates GPU memory during play
    // (documentation/03 §8), and the mirror follows the same rule so that running out is
    // an eviction rather than a stall.
    headers_.assign(budget.max_bricks, GpuBrickHeader{});
    occupancy_.assign(static_cast<usize>(budget.max_bricks) * kBrickWords, 0);
    payload_.assign(static_cast<usize>(payload_pool_.capacity()), 0);
    records_.assign(budget.max_chunks, GpuChunkRecord{});
    masks_.assign(static_cast<usize>(budget.max_chunks) * kChunkMaskWords, 0);
    prefixes_.assign(static_cast<usize>(budget.max_chunks) * kChunkMaskWords, 0);
    grid_.assign(static_cast<usize>(budget.grid_width) * budget.grid_height *
                     budget.grid_depth,
                 kNoRecord);

    const u32 coarse_total = coarse_offset(kCoarseLevels);
    coarse_counts_.assign(coarse_total, 0);
    coarse_flags_.assign(static_cast<usize>(coarse_total) * 4, 0);

    free_records_.clear();
    record_cursor_ = 0;
    chunks_.clear();
    requested_.clear();
    dirty_.clear();
    scratch_payload_.reserve(2048);
}

u32 ResidencyManager::coarse_offset(u32 level) const {
    return level * kCoarseCellsPerLevel;
}

u32 ResidencyManager::coarse_index(u32 level, i64 chunk_x, i64 chunk_y, i64 chunk_z) const {
    const i64 factor = kCoarseFactor[level];

    // Floor-divide into blocks, then wrap. Every level has the same grid size, so the
    // coarser it is the further it reaches before two blocks can land in one cell.
    const i64 bx = (chunk_x >= 0) ? chunk_x / factor : -((-chunk_x + factor - 1) / factor);
    const i64 by = (chunk_y >= 0) ? chunk_y / factor : -((-chunk_y + factor - 1) / factor);
    const i64 bz = (chunk_z >= 0) ? chunk_z / factor : -((-chunk_z + factor - 1) / factor);

    const i64 gx = floor_mod(bx, kCoarseDimX);
    const i64 gy = floor_mod(by, kCoarseDimY);
    const i64 gz = floor_mod(bz, kCoarseDimZ);
    return coarse_offset(level) +
           static_cast<u32>((gz * kCoarseDimY + gy) * kCoarseDimX + gx);
}

void ResidencyManager::coarse_add(const ChunkCoord& coord, i32 delta) {
    for (u32 level = 0; level < kCoarseLevels; ++level) {
        const i64 factor = kCoarseFactor[level];
        const i32 bx = static_cast<i32>((coord.x >= 0) ? coord.x / factor
                                                       : -((-coord.x + factor - 1) / factor));
        const i32 by = static_cast<i32>((coord.y >= 0) ? coord.y / factor
                                                       : -((-coord.y + factor - 1) / factor));
        const i32 bz = static_cast<i32>((coord.z >= 0) ? coord.z / factor
                                                       : -((-coord.z + factor - 1) / factor));

        const u32 cell = coarse_index(level, coord.x, coord.y, coord.z);
        const u32 base = cell * 4;
        u32& count = coarse_counts_[cell];
        const u32 before_state = coarse_flags_[base + 3];

        if (before_state == kCoarseConflicted) continue;   // stays conservatively occupied

        const bool same_block = (before_state != kCoarseEmpty) &&
                                static_cast<i32>(coarse_flags_[base + 0]) == bx &&
                                static_cast<i32>(coarse_flags_[base + 1]) == by &&
                                static_cast<i32>(coarse_flags_[base + 2]) == bz;

        if (before_state != kCoarseEmpty && !same_block) {
            // Two different blocks of chunks wrapped onto the same cell. There is nowhere
            // to record both, so the cell becomes permanently "might contain something",
            // which costs a missed skip and never a missed hit. With any sane streaming
            // radius this cannot happen; being wrong in the safe direction is free.
            coarse_flags_[base + 3] = kCoarseConflicted;
            batch_.coarse_cells.push_back(cell);
            continue;
        }

        count = static_cast<u32>(static_cast<i64>(count) + delta);
        const u32 state = (count > 0) ? kCoarseOccupied : kCoarseEmpty;
        if (state != before_state || !same_block) {
            coarse_flags_[base + 0] = static_cast<u32>(bx);
            coarse_flags_[base + 1] = static_cast<u32>(by);
            coarse_flags_[base + 2] = static_cast<u32>(bz);
            coarse_flags_[base + 3] = state;
            batch_.coarse_cells.push_back(cell);
        }
    }
}

void ResidencyManager::rebuild_coarse(const World& world) {
    std::fill(coarse_counts_.begin(), coarse_counts_.end(), 0);
    std::fill(coarse_flags_.begin(), coarse_flags_.end(), 0);

    world.for_each_chunk([this](const ChunkCoord& coord, const Chunk& chunk) {
        if (chunk.empty()) return;
        coarse_add(coord, +1);
    });

    // Flag it rather than filling the batch here: update() clears the batch at the top of
    // every frame, so anything written outside update() is discarded before it is ever
    // uploaded. That cost an afternoon — the grids were correct on the CPU and all zero
    // on the GPU, so the marcher skipped the entire world and feedback never fired.
    coarse_dirty_ = true;
}

u32 ResidencyManager::grid_index(i64 chunk_x, i64 chunk_y, i64 chunk_z) const {
    const i64 gx = floor_mod(chunk_x, budget_.grid_width);
    const i64 gy = floor_mod(chunk_y, budget_.grid_height);
    const i64 gz = floor_mod(chunk_z, budget_.grid_depth);
    return static_cast<u32>((gz * budget_.grid_height + gy) * budget_.grid_width + gx);
}

void ResidencyManager::request(const ChunkCoord& coord) { requested_.push_back(coord); }

void ResidencyManager::invalidate(const ChunkCoord& coord) { dirty_.insert(coord); }

u32 ResidencyManager::allocate_record() {
    if (!free_records_.empty()) {
        const u32 record = free_records_.back();
        free_records_.pop_back();
        return record;
    }
    if (record_cursor_ >= budget_.max_chunks) return kNoRecord;
    return record_cursor_++;
}

void ResidencyManager::release_record(u32 record) {
    records_[record] = GpuChunkRecord{};
    std::memset(masks_.data() + static_cast<usize>(record) * kChunkMaskWords, 0,
                kChunkMaskWords * sizeof(u64));
    std::memset(prefixes_.data() + static_cast<usize>(record) * kChunkMaskWords, 0,
                kChunkMaskWords * sizeof(u32));
    free_records_.push_back(record);
}

void ResidencyManager::evict_chunk(const ChunkCoord& coord) {
    const auto found = chunks_.find(coord);
    if (found == chunks_.end()) return;
    ResidentChunk& chunk = found->second;

    for (const ResidentBrick& brick : chunk.bricks) {
        if (brick.payload_size > 0) {
            payload_pool_.release(brick.payload_offset, brick.payload_size);
        }
    }
    if (chunk.slot_run > 0) slot_pool_.release(chunk.slot_base, chunk.slot_run);

    if (chunk.record != kNoRecord) {
        const u32 cell = grid_index(coord.x, coord.y, coord.z);
        // Only clear the cell if it still points at us. Another chunk may have aliased
        // into it, and clearing that would make a live chunk invisible.
        if (grid_[cell] == chunk.record) {
            grid_[cell] = kNoRecord;
            batch_.grid_cells.push_back(cell);
        }
        release_record(chunk.record);
    }
    // Coarse occupancy deliberately does not change here: it describes the world, not
    // what happens to be resident. See rebuild_coarse().
    chunks_.erase(found);
    ++evictions_;
    ++batch_.chunks_evicted;
}

bool ResidencyManager::evict_one_lru(u64 frame) {
    // Never evict something wanted this frame — that would thrash on the very data the
    // renderer is about to read.
    ChunkCoord victim{};
    bool found_victim = false;
    u64 oldest = frame;

    for (const auto& [coord, chunk] : chunks_) {
        if (chunk.last_used >= frame) continue;
        if (!found_victim || chunk.last_used < oldest) {
            oldest = chunk.last_used;
            victim = coord;   // by value: evict_chunk erases the node this refers to
            found_victim = true;
        }
    }

    if (!found_victim) return false;
    evict_chunk(victim);
    return true;
}

ResidencyManager::UploadStep ResidencyManager::begin_upload(const World& world,
                                                            const ChunkCoord& coord,
                                                            u64 frame, u32 brick_budget) {
    const Chunk* chunk = world.chunk(coord);
    if (chunk == nullptr || chunk->empty()) {
        evict_chunk(coord);   // it used to exist and no longer does
        return UploadStep::Complete;
    }

    // A changed chunk is re-uploaded whole. Per-brick diffing would need the feedback
    // buffer to report at brick granularity, which it does not.
    evict_chunk(coord);

    const u32 record = allocate_record();
    if (record == kNoRecord) return UploadStep::Failed;

    const u32 brick_count = chunk->brick_count();
    if (slot_pool_.class_size_for(brick_count) == 0) {
        // More bricks than the largest run class. A chunk holds at most 32,768, and the
        // table goes that far, so this is a corrupt chunk rather than pool pressure.
        release_record(record);
        WS_LOG_ERROR("residency", "chunk holds {} bricks, beyond any run class", brick_count);
        return UploadStep::Failed;
    }
    const u32 slot_base = slot_pool_.allocate(brick_count);
    if (slot_base == kNoOffset) {
        release_record(record);
        return UploadStep::Failed;
    }

    pending_ = PendingUpload{};
    pending_.active = true;
    pending_.coord = coord;
    pending_.record = record;
    pending_.slot_base = slot_base;
    pending_.brick_count = brick_count;
    pending_.next_linear = 0;
    pending_.revision = chunk->revision();
    pending_.bricks.reserve(brick_count);

    u64* mask = masks_.data() + static_cast<usize>(record) * kChunkMaskWords;
    u32* prefix = prefixes_.data() + static_cast<usize>(record) * kChunkMaskWords;
    std::memset(mask, 0, kChunkMaskWords * sizeof(u64));
    std::memset(prefix, 0, kChunkMaskWords * sizeof(u32));

    return advance_upload(world, frame, brick_budget);
}

void ResidencyManager::abandon_pending() {
    if (!pending_.active) return;
    for (const ResidentBrick& done : pending_.bricks) {
        if (done.payload_size > 0) {
            payload_pool_.release(done.payload_offset, done.payload_size);
        }
    }
    slot_pool_.release(pending_.slot_base, pending_.brick_count);
    release_record(pending_.record);
    pending_ = PendingUpload{};
}

ResidencyManager::UploadStep ResidencyManager::advance_upload(const World& world, u64 frame,
                                                              u32 brick_budget) {
    const Chunk* chunk = world.chunk(pending_.coord);
    if (chunk == nullptr || chunk->revision() != pending_.revision) {
        // The chunk changed while we were part-way through it. Throw the partial work away
        // rather than publishing a mixture of two versions.
        abandon_pending();
        return UploadStep::Complete;
    }

    u64* mask = masks_.data() + static_cast<usize>(pending_.record) * kChunkMaskWords;
    GpuBrickHeader header;
    u64 brick_occupancy[kBrickWords];
    u32 encoded = 0;

    // z outer, then y, then x: brick_linear_index is x-fastest, so this visits bricks in
    // ascending linear order and rank == the order they are appended.
    //
    // Resuming starts the loops at the stored position rather than scanning up to it.
    // Scanning cost an octree descent per skipped position — 32,768 of them per frame
    // regardless of how few bricks were actually encoded, which showed up as a 1.04 ms
    // spike on the one frame that crossed into new chunks.
    const u32 resume_z = pending_.next_linear >> 10;
    const u32 resume_y = (pending_.next_linear >> 5) & 31u;
    const u32 resume_x = pending_.next_linear & 31u;

    for (u32 bz = resume_z; bz < kAxis; ++bz) {
        const u32 y_start = (bz == resume_z) ? resume_y : 0;
        for (u32 by = y_start; by < kAxis; ++by) {
            const u32 x_start = (bz == resume_z && by == resume_y) ? resume_x : 0;
            for (u32 bx = x_start; bx < kAxis; ++bx) {
                const u32 linear = brick_linear_index(bx, by, bz);

                if (encoded >= brick_budget) {
                    pending_.next_linear = linear;
                    return UploadStep::Partial;
                }

                const Brick* brick = chunk->brick(bx, by, bz);
                if (brick == nullptr || brick->empty()) continue;

                const u32 rank = static_cast<u32>(pending_.bricks.size());
                if (rank >= pending_.brick_count) {
                    WS_LOG_ERROR("residency", "chunk brick count {} is too low",
                                 pending_.brick_count);
                    pending_.next_linear = 0xFFFFFFFFu;
                    finish_pending(frame);
                    return UploadStep::Complete;
                }
                const u32 slot = pending_.slot_base + rank;

                encode_brick(*brick, *types_, header, scratch_payload_, brick_occupancy);
                ++encoded;

                ResidentBrick entry;
                entry.linear = linear;
                entry.payload_size = static_cast<u32>(scratch_payload_.size());

                if (entry.payload_size > 0) {
                    const u32 offset = payload_pool_.allocate(entry.payload_size);
                    if (offset == kNoOffset) {
                        abandon_pending();
                        return UploadStep::Failed;
                    }
                    entry.payload_offset = offset;
                    std::memcpy(payload_.data() + offset, scratch_payload_.data(),
                                entry.payload_size);
                    batch_.payload.push_back(UploadRange{offset, entry.payload_size});
                    batch_.payload_bytes += entry.payload_size;
                    header.payload_offset = offset;
                } else {
                    header.payload_offset = kNoOffset;
                }

                headers_[slot] = header;
                std::memcpy(occupancy_.data() + static_cast<usize>(slot) * kBrickWords,
                            brick_occupancy, kBrickWords * sizeof(u64));
                batch_.slots.push_back(slot);
                ++batch_.bricks_added;

                mask[linear / 64] |= (u64{1} << (linear % 64));
                pending_.bricks.push_back(entry);
            }
        }
    }

    finish_pending(frame);
    return UploadStep::Complete;
}

void ResidencyManager::finish_pending(u64 frame) {
    const u32 record = pending_.record;
    const ChunkCoord coord = pending_.coord;
    const u32 slot_base = pending_.slot_base;

    ResidentChunk resident;
    resident.record = record;
    resident.slot_base = slot_base;
    resident.slot_run = pending_.brick_count;
    resident.last_used = frame;
    resident.revision = pending_.revision;
    resident.bricks = std::move(pending_.bricks);

    u64* mask = masks_.data() + static_cast<usize>(record) * kChunkMaskWords;
    u32* prefix = prefixes_.data() + static_cast<usize>(record) * kChunkMaskWords;

    // Running popcount over level 0 only, so a set brick bit converts to a rank in O(1).
    // The mip levels above it are for skipping, not for addressing bricks.
    u32 running = 0;
    for (u32 word = 0; word < kChunkMipWords[0]; ++word) {
        prefix[word] = running;
        running += static_cast<u32>(std::popcount(mask[word]));
    }

    // Filter the brick grid upward. A cell at level k+1 is set when any of its eight
    // children at level k is set — the coarse test a distant ray uses to skip.
    for (u32 level = 1; level < kChunkMipLevels; ++level) {
        const u32 axis = chunk_mip_axis(level);
        const u32 child_axis = chunk_mip_axis(level - 1);
        const u64* child = mask + kChunkMipOffset[level - 1];
        u64* parent = mask + kChunkMipOffset[level];

        for (u32 z = 0; z < axis; ++z) {
            for (u32 y = 0; y < axis; ++y) {
                for (u32 x = 0; x < axis; ++x) {
                    bool any = false;
                    for (u32 c = 0; c < 8 && !any; ++c) {
                        const u32 cx = x * 2 + (c & 1u);
                        const u32 cy = y * 2 + ((c >> 1) & 1u);
                        const u32 cz = z * 2 + ((c >> 2) & 1u);
                        const u32 bit = cx | (cy * child_axis) | (cz * child_axis * child_axis);
                        any = ((child[bit / 64] >> (bit % 64)) & 1u) != 0;
                    }
                    if (!any) continue;
                    const u32 bit = x | (y * axis) | (z * axis * axis);
                    parent[bit / 64] |= (u64{1} << (bit % 64));
                }
            }
        }
    }

    // The chunk's own filtered colour, averaged over its bricks by coverage. Used when a
    // whole 8 m chunk is smaller than a pixel.
    {
        u64 red = 0;
        u64 green = 0;
        u64 blue = 0;
        u64 weight = 0;
        for (const ResidentBrick& entry : resident.bricks) {
            const u32 colour = headers_[slot_base + (&entry - resident.bricks.data())].average_colour;
            const u32 coverage = (colour >> 24) & 0xFFu;
            if (coverage == 0) continue;
            red += static_cast<u64>(colour & 0xFFu) * coverage;
            green += static_cast<u64>((colour >> 8) & 0xFFu) * coverage;
            blue += static_cast<u64>((colour >> 16) & 0xFFu) * coverage;
            weight += coverage;
        }
        u32 average = 0;
        if (weight > 0) {
            const u64 fill = (weight * 255) / (static_cast<u64>(resident.bricks.size()) * 255);
            average = static_cast<u32>(red / weight) |
                      (static_cast<u32>(green / weight) << 8) |
                      (static_cast<u32>(blue / weight) << 16) |
                      (static_cast<u32>(fill > 255 ? 255 : fill) << 24);
        }
        records_[record].average_colour = average;
    }

    GpuChunkRecord& gpu_record = records_[record];
    gpu_record.x = static_cast<i32>(coord.x);
    gpu_record.y = static_cast<i32>(coord.y);
    gpu_record.z = static_cast<i32>(coord.z);
    gpu_record.mask_offset = record * kChunkMaskWords;
    gpu_record.prefix_offset = record * kChunkMaskWords;
    gpu_record.slot_base = slot_base;
    gpu_record.brick_count = static_cast<u32>(resident.bricks.size());

    const u32 cell = grid_index(coord.x, coord.y, coord.z);
    grid_[cell] = record;
    batch_.grid_cells.push_back(cell);
    batch_.records.push_back(record);

    chunks_.emplace(coord, std::move(resident));
    pending_ = PendingUpload{};
    ++uploads_;
}

const UploadBatch& ResidencyManager::update(const World& world, u64 frame) {
    batch_.clear();

    if (coarse_dirty_) {
        for (u32 cell = 0; cell < static_cast<u32>(coarse_counts_.size()); ++cell) {
            batch_.coarse_cells.push_back(cell);
        }
        coarse_dirty_ = false;
    }

    // Carry over everything known to be stale. These are re-offered every frame until they
    // are actually served, because nothing else will ever ask for them (see invalidate()).
    //
    // A chunk that is not resident is dropped from the set rather than uploaded: it will be
    // fetched, correct and complete, the moment a ray wants it. Uploading it here instead
    // would spend the frame's budget on chunks nobody is looking at, and starve the ones
    // that are.
    for (auto it = dirty_.begin(); it != dirty_.end();) {
        if (chunks_.find(*it) == chunks_.end()) {
            it = dirty_.erase(it);
        } else {
            requested_.push_back(*it);
            ++it;
        }
    }

    // Deduplicate this frame's requests. The feedback buffer reports per pixel, so the
    // same chunk arrives thousands of times.
    std::sort(requested_.begin(), requested_.end(),
              [](const ChunkCoord& a, const ChunkCoord& b) {
                  if (a.z != b.z) return a.z < b.z;
                  if (a.y != b.y) return a.y < b.y;
                  return a.x < b.x;
              });
    requested_.erase(std::unique(requested_.begin(), requested_.end()), requested_.end());

    // Finish anything left half-encoded before starting something new. A chunk in
    // progress already holds its slot run and record, and nothing sees it until it is
    // done, so the only cost of taking several frames over it is that it appears later.
    if (pending_.active) {
        const u32 budget = budget_.max_bricks_per_frame;
        while (advance_upload(world, frame, budget) == UploadStep::Failed) {
            if (!evict_one_lru(frame)) {
                batch_.out_of_memory = true;
                break;
            }
        }
        if (pending_.active) {
            // Still not finished; the whole frame's budget went into it.
            batch_.chunks_deferred = static_cast<u32>(requested_.size());
            requested_.clear();
            return batch_;
        }
    }

    u32 uploads_this_frame = 0;

    for (const ChunkCoord& coord : requested_) {
        ++requests_;

        const auto found = chunks_.find(coord);
        if (found != chunks_.end()) {
            found->second.last_used = frame;
            const Chunk* chunk = world.chunk(coord);
            // O(1) staleness check. Hashing the chunk here instead measured 116 ms per
            // frame against a 0.8 ms budget, because it walks every voxel of every brick.
            if (chunk != nullptr && chunk->revision() == found->second.revision) {
                ++hits_;
                dirty_.erase(coord);   // what the GPU holds is what the world holds
                continue;
            }
        }

        // Work caps. Anything past them is simply wanted again next frame; the renderer
        // keeps asking until it gets served, which is what makes demand streaming
        // self-correcting rather than a queue that can grow without bound.
        if (uploads_this_frame >= budget_.max_chunk_uploads_per_frame ||
            batch_.bricks_added >= budget_.max_bricks_per_frame ||
            batch_.payload_bytes >= budget_.max_upload_bytes_per_frame) {
            ++batch_.chunks_deferred;
            continue;
        }

        if (found != chunks_.end()) {
            ++batch_.chunks_refreshed;
        } else {
            ++batch_.chunks_added;
        }

        // Retry around eviction: a full pool is normal, not an error.
        const u32 remaining = (batch_.bricks_added < budget_.max_bricks_per_frame)
                                  ? budget_.max_bricks_per_frame - batch_.bricks_added
                                  : 0;
        UploadStep step = begin_upload(world, coord, frame, remaining);
        while (step == UploadStep::Failed) {
            if (!evict_one_lru(frame)) {
                batch_.out_of_memory = true;
                break;
            }
            step = begin_upload(world, coord, frame, remaining);
        }
        ++uploads_this_frame;
        if (step == UploadStep::Partial) break;   // the rest of the frame belongs to it
    }

    requested_.clear();
    return batch_;
}

bool ResidencyManager::resident_bounds(ChunkCoord& min, ChunkCoord& max) const {
    if (chunks_.empty()) return false;
    bool first = true;
    for (const auto& [coord, chunk] : chunks_) {
        (void)chunk;
        if (first) {
            min = coord;
            max = coord;
            first = false;
            continue;
        }
        min.x = (coord.x < min.x) ? coord.x : min.x;
        min.y = (coord.y < min.y) ? coord.y : min.y;
        min.z = (coord.z < min.z) ? coord.z : min.z;
        max.x = (coord.x > max.x) ? coord.x : max.x;
        max.y = (coord.y > max.y) ? coord.y : max.y;
        max.z = (coord.z > max.z) ? coord.z : max.z;
    }
    return true;
}

bool ResidencyManager::resident(const ChunkCoord& coord) const {
    return chunks_.find(coord) != chunks_.end();
}

const ResidencyManager::ResidentBrick* ResidencyManager::find_brick(
    const ResidentChunk& chunk, u32 linear) const {
    const auto at = std::lower_bound(
        chunk.bricks.begin(), chunk.bricks.end(), linear,
        [](const ResidentBrick& brick, u32 want) { return brick.linear < want; });
    if (at == chunk.bricks.end() || at->linear != linear) return nullptr;
    return &*at;
}

u32 ResidencyManager::brick_rank(const ResidentChunk& chunk, u32 linear) const {
    // Exactly the walk the shader performs: test the mask bit, then convert it to a rank
    // with the stored prefix plus a partial popcount.
    const u64* mask = masks_.data() + static_cast<usize>(chunk.record) * kChunkMaskWords;
    const u32* prefix = prefixes_.data() + static_cast<usize>(chunk.record) * kChunkMaskWords;

    const u32 word = linear / 64;
    const u32 bit = linear % 64;
    if (((mask[word] >> bit) & 1u) == 0) return kNoSlot;

    const u64 below = (bit == 0) ? 0ull : (mask[word] << (64 - bit) >> (64 - bit));
    return prefix[word] + static_cast<u32>(std::popcount(below));
}

VoxelTypeId ResidencyManager::mirror_voxel(const ChunkCoord& coord, u32 x, u32 y,
                                           u32 z) const {
    const auto found = chunks_.find(coord);
    if (found == chunks_.end()) return kAir;
    const ResidentChunk& chunk = found->second;

    const u32 edge = static_cast<u32>(kBrickEdge);
    const u32 linear = brick_linear_index(x / edge, y / edge, z / edge);
    const u32 rank = brick_rank(chunk, linear);
    if (rank == kNoSlot) return kAir;

    const u32 slot = chunk.slot_base + rank;
    const GpuBrickHeader& header = headers_[slot];
    const u8* payload = (header.payload_offset == kNoOffset)
                            ? nullptr
                            : payload_.data() + header.payload_offset;
    return decode_voxel(header, payload, brick_index(x % edge, y % edge, z % edge));
}

VoxelTypeId ResidencyManager::mirror_voxel_world(i64 x, i64 y, i64 z) const {
    // The full shader walk: wrapped grid cell, record, coordinate check, then as above.
    const ChunkCoord coord = chunk_coord_of(x, y, z);
    const u32 record = grid_[grid_index(coord.x, coord.y, coord.z)];
    if (record == kNoRecord) return kAir;

    const GpuChunkRecord& gpu = records_[record];
    if (gpu.x != static_cast<i32>(coord.x) || gpu.y != static_cast<i32>(coord.y) ||
        gpu.z != static_cast<i32>(coord.z)) {
        return kAir;   // the cell aliased to a different chunk
    }

    const u32 lx = local_of(x);
    const u32 ly = local_of(y);
    const u32 lz = local_of(z);
    const u32 edge = static_cast<u32>(kBrickEdge);
    const u32 linear = brick_linear_index(lx / edge, ly / edge, lz / edge);

    const u64* mask = masks_.data() + gpu.mask_offset;
    const u32* prefix = prefixes_.data() + gpu.prefix_offset;
    const u32 word = linear / 64;
    const u32 bit = linear % 64;
    if (((mask[word] >> bit) & 1u) == 0) return kAir;

    const u64 below = (bit == 0) ? 0ull : (mask[word] << (64 - bit) >> (64 - bit));
    const u32 slot = gpu.slot_base + prefix[word] + static_cast<u32>(std::popcount(below));

    const GpuBrickHeader& header = headers_[slot];
    const u8* payload = (header.payload_offset == kNoOffset)
                            ? nullptr
                            : payload_.data() + header.payload_offset;
    return decode_voxel(header, payload, brick_index(lx % edge, ly % edge, lz % edge));
}

u64 ResidencyManager::mirror_hash(const ChunkCoord& coord) const {
    const auto found = chunks_.find(coord);
    if (found == chunks_.end()) return 0;
    const ResidentChunk& chunk = found->second;

    // Mirrors Chunk::content_hash exactly, including its iteration order and its key
    // formula, so the two numbers are directly comparable.
    u64 h = 0x243F6A88ull;
    for (u32 bz = 0; bz < kAxis; ++bz) {
        for (u32 by = 0; by < kAxis; ++by) {
            for (u32 bx = 0; bx < kAxis; ++bx) {
                const u32 rank = brick_rank(chunk, brick_linear_index(bx, by, bz));
                if (rank == kNoSlot) continue;

                const u32 slot = chunk.slot_base + rank;
                const u64* words =
                    occupancy_.data() + static_cast<usize>(slot) * kBrickWords;
                bool any_solid = false;
                for (u32 w = 0; w < kBrickWords; ++w) {
                    if (words[w] != 0) { any_solid = true; break; }
                }
                if (!any_solid) continue;

                const GpuBrickHeader& header = headers_[slot];
                const u8* payload = (header.payload_offset == kNoOffset)
                                        ? nullptr
                                        : payload_.data() + header.payload_offset;
                h = hash_combine(h, (bx << 10) | (by << 5) | bz);
                h = hash_combine(h, decode_brick_hash(header, payload));
            }
        }
    }
    return h;
}

ResidencyStats ResidencyManager::stats() const {
    ResidencyStats out;
    out.resident_chunks = static_cast<u32>(chunks_.size());
    for (const auto& [coord, chunk] : chunks_) {
        (void)coord;
        out.resident_bricks += static_cast<u32>(chunk.bricks.size());
    }

    const BlockPoolStats payload = payload_pool_.stats();
    const BlockPoolStats slots = slot_pool_.stats();
    out.payload_in_use = payload.in_use;
    out.payload_capacity = payload.capacity;
    out.slot_run_in_use = slots.in_use;
    out.header_bytes = slots.in_use * sizeof(GpuBrickHeader);
    out.occupancy_bytes = slots.in_use * kBrickWords * sizeof(u64);
    out.index_bytes = static_cast<u64>(out.resident_chunks) *
                          (sizeof(GpuChunkRecord) + kChunkMaskWords * (8 + 4)) +
                      grid_.size() * sizeof(u32);
    out.total_bytes =
        out.payload_in_use + out.header_bytes + out.occupancy_bytes + out.index_bytes;
    out.uploads = uploads_;
    out.evictions = evictions_;
    out.requests = requests_;
    out.hits = hits_;
    return out;
}

bool ResidencyManager::validate() const {
    if (!payload_pool_.validate()) return false;
    if (!slot_pool_.validate()) return false;

    std::vector<u8> record_seen(budget_.max_chunks, 0);
    for (u32 record : free_records_) {
        if (record >= budget_.max_chunks) return false;
        if (record_seen[record] != 0) return false;
        record_seen[record] = 2;
    }

    for (const auto& [coord, chunk] : chunks_) {
        if (chunk.record >= budget_.max_chunks) return false;
        if (record_seen[chunk.record] != 0) return false;   // aliased or freed
        record_seen[chunk.record] = 1;

        const GpuChunkRecord& gpu = records_[chunk.record];
        if (gpu.x != static_cast<i32>(coord.x) || gpu.y != static_cast<i32>(coord.y) ||
            gpu.z != static_cast<i32>(coord.z)) {
            return false;
        }
        if (gpu.slot_base != chunk.slot_base) return false;
        if (gpu.brick_count != chunk.bricks.size()) return false;
        if (chunk.slot_base + chunk.slot_run > budget_.max_bricks) return false;

        // Bricks must be sorted by linear index, because rank depends on it.
        for (usize i = 1; i < chunk.bricks.size(); ++i) {
            if (chunk.bricks[i - 1].linear >= chunk.bricks[i].linear) return false;
        }

        // The mask, the prefix and the brick list must all agree.
        for (u32 rank = 0; rank < chunk.bricks.size(); ++rank) {
            if (brick_rank(chunk, chunk.bricks[rank].linear) != rank) return false;
        }

        for (const ResidentBrick& brick : chunk.bricks) {
            if (brick.payload_size == 0) continue;
            if (brick.payload_offset + brick.payload_size > payload_.size()) return false;
            if (brick.payload_offset % kBlockGranularity != 0) return false;
        }
    }
    return true;
}

}  // namespace ws
