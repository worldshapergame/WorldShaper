#include "world/world_cache.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <unordered_map>
#include <vector>

#include "core/hash.hpp"
#include "core/jobs.hpp"
#include "core/log.hpp"
#include "core/time.hpp"
#include "world/brick.hpp"
#include "world/ledger.hpp"
#include "world/property.hpp"
#include "world/tags.hpp"
#include "world/world.hpp"

namespace ws {
namespace {

constexpr u32 kMagic = 0x57534357u;   // "WSCW"
// 2 — the sharpened-region list, so a world cached before it finished can be carried on rather
// than mistaken for a finished one. An older file is rejected by the header check and rebuilt.
// 3 — R9g: a chunk's emissive cells are written beside the world.
// 4 — the stipple verdict, without which a resumed world cannot despeckle anything it sharpens.
// Not read compatibly from a version 3 file: the missing verdict is exactly the fault being fixed,
// so a file that does not have one is rebuilt rather than loaded and quietly left speckled.
// 5 — the ladder's whole leaf set, each leaf with its octree key and the detail it was sampled at,
// so a resuming run rebuilds the tree rather than trying to recognise it by containment. Again not
// read compatibly: a version 4 file holds only the finest boxes and no keys, which is the fault
// being fixed, and a file that cannot say what its coarse leaves are is worth less than a rebuild.
// 6 — R11f: a mode byte in the header and an edit-only payload, so a world can be written as its
// clip plus what somebody changed rather than as sixty million voxels. This one could not have
// been read compatibly whatever anybody wanted: a version 5 file has no mode byte, so every field
// after the key sits at a different offset, and a reader that guessed at the mode would not fail —
// it would misread a whole world as a difference, or a difference as a whole world.
constexpr u32 kVersion = 6u;

// A brick, exactly as it is held. No canonical form, no re-encode: the whole reason this file
// exists is that it can be read faster than the world can be rebuilt, and a normalisation pass
// over sixty million voxels is most of what it is trying to avoid.
void write_brick_raw(std::vector<u8>& out, const Brick& brick) {
    const auto put_u32 = [&out](u32 value) {
        out.push_back(static_cast<u8>(value));
        out.push_back(static_cast<u8>(value >> 8));
        out.push_back(static_cast<u8>(value >> 16));
        out.push_back(static_cast<u8>(value >> 24));
    };

    if (brick.uniform()) {
        out.push_back(0u);
        put_u32(brick.uniform_value());
        return;
    }

    // A palette when the brick has one, because most bricks in most worlds hold a handful of
    // materials and writing five hundred and twelve full type ids for four distinct values is
    // four times the file for no gain. The clip-forge's own worlds are the opposite case — every
    // voxel its own record — and those fall through to the flat form below, which is the fastest
    // thing to read and, at 512 distinct types, also the smallest.
    if (brick.form() != Brick::Form::Direct) {
        const std::vector<VoxelTypeId>& palette = brick.palette_data();
        const std::vector<u8>& indices = brick.index_data();
        out.push_back(2u);
        out.push_back(static_cast<u8>(brick.index_bits()));
        put_u32(static_cast<u32>(palette.size()));
        for (VoxelTypeId type : palette) put_u32(type);
        out.insert(out.end(), indices.begin(), indices.end());
        return;
    }

    VoxelTypeId decoded[kBrickVoxels];
    brick.decode(decoded);
    out.push_back(1u);
    const usize at = out.size();
    out.resize(at + sizeof(decoded));
    std::memcpy(out.data() + at, decoded, sizeof(decoded));
}

// How many bytes the brick written at `data` occupies, without decoding it. Used to find where
// one chunk's payload ends so the chunks can then be filled in parallel.
usize brick_span(const u8* data, usize available) {
    if (available < 1) return 0;
    if (data[0] == 0u) return (available >= 5) ? 5 : 0;
    if (data[0] == 1u) {
        const usize span = 1 + kBrickVoxels * sizeof(VoxelTypeId);
        return (available >= span) ? span : 0;
    }
    if (data[0] != 2u || available < 6) return 0;
    const u32 bits = data[1];
    if (bits != 1 && bits != 2 && bits != 4 && bits != 8) return 0;
    u32 palette_size = 0;
    std::memcpy(&palette_size, data + 2, sizeof(palette_size));
    const usize span = 6 + palette_size * sizeof(VoxelTypeId) +
                       (static_cast<usize>(kBrickVoxels) * bits + 7) / 8;
    return (available >= span) ? span : 0;
}

usize read_brick_raw(const u8* data, usize available, Brick& brick) {
    const usize span = brick_span(data, available);
    if (span == 0) return 0;

    if (data[0] == 0u) {
        u32 value = 0;
        std::memcpy(&value, data + 1, sizeof(value));
        brick.fill(value);
        return span;
    }

    VoxelTypeId decoded[kBrickVoxels];
    if (data[0] == 1u) {
        std::memcpy(decoded, data + 1, sizeof(decoded));
        brick.assign(decoded);
        return span;
    }

    const u32 bits = data[1];
    u32 palette_size = 0;
    std::memcpy(&palette_size, data + 2, sizeof(palette_size));
    const u8* palette = data + 6;
    const u8* indices = palette + palette_size * sizeof(VoxelTypeId);
    const u32 per_byte = 8u / bits;
    const u32 mask = (1u << bits) - 1;
    for (u32 base = 0; base < static_cast<u32>(kBrickVoxels); base += per_byte) {
        const u8 packed = indices[base / per_byte];
        for (u32 i = 0; i < per_byte; ++i) {
            const u32 slot = (packed >> (i * bits)) & mask;
            if (slot >= palette_size) return 0;
            std::memcpy(&decoded[base + i], palette + slot * sizeof(VoxelTypeId),
                        sizeof(VoxelTypeId));
        }
    }
    brick.assign(decoded);
    return span;
}

template <typename T>
void put_pod(std::vector<u8>& out, const T& value) {
    const usize at = out.size();
    out.resize(at + sizeof(T));
    std::memcpy(out.data() + at, &value, sizeof(T));
}

struct Cursor {
    const u8* data = nullptr;
    usize size = 0;
    usize at = 0;
    bool ok = true;

    template <typename T>
    T pod() {
        T value{};
        if (at + sizeof(T) > size) {
            ok = false;
            return value;
        }
        std::memcpy(&value, data + at, sizeof(T));
        at += sizeof(T);
        return value;
    }

    const u8* take(usize bytes) {
        if (at + bytes > size) {
            ok = false;
            return nullptr;
        }
        const u8* p = data + at;
        at += bytes;
        return p;
    }
};

// --------------------------------------------------------------------------------------
// R11f: what "the same as what the clip builds" means, brick by brick.
// --------------------------------------------------------------------------------------

// Do these two bricks hold the same voxels?
//
// `Brick::operator==` already answers this and answers it correctly — contents, never encoding —
// but it answers it by reading all five hundred and twelve voxels through `get()`, and a diff over
// the facility asks the question a quarter of a million times about bricks that are nearly always
// equal, which is the worst case that comparison has: no early exit and a form test per voxel.
//
// So: identical encodings are compared as bytes, which they are for every brick either world got
// from the same sampler, and anything else falls through to the honest comparison. The fast path
// can only ever say "equal" about bricks that really are, because two bricks with the same form,
// the same palette in the same order and the same indices decode to the same voxels by
// construction. A permuted palette simply misses the fast path and is compared properly.
bool bricks_identical(const Brick& a, const Brick& b) {
    if (a.uniform() && b.uniform()) return a.uniform_value() == b.uniform_value();
    if (a.form() == b.form() && a.palette_data() == b.palette_data() &&
        a.index_data() == b.index_data() && a.direct_data() == b.direct_data()) {
        return true;
    }
    return a == b;
}

// Do a named edit box and a box of voxels overlap? Both inclusive of their corners.
bool box_touches(const CachedEditBox& box, const i64 low[3], const i64 high[3]) {
    for (u32 axis = 0; axis < 3; ++axis) {
        if (box.high[axis] < low[axis] || box.low[axis] > high[axis]) return false;
    }
    return true;
}

// The order `World::sorted_chunk_coords` puts them in, so two sorted lists can be merged.
bool chunk_coord_less(const ChunkCoord& a, const ChunkCoord& b) {
    if (a.z != b.z) return a.z < b.z;
    if (a.y != b.y) return a.y < b.y;
    return a.x < b.x;
}

// A chunk holding something, as against one that exists and does not. The distinction is the
// whole of D620: an allocated chunk with no bricks in it claims matter the world does not have.
bool chunk_is_live(const Chunk* chunk) { return chunk != nullptr && !chunk->empty(); }
bool brick_is_live(const Brick* brick) { return brick != nullptr && !brick->empty(); }

// Every chunk of a world that holds something, in the canonical order.
std::vector<ChunkCoord> live_chunk_coords(const World& world) {
    std::vector<ChunkCoord> out = world.sorted_chunk_coords();
    out.erase(std::remove_if(out.begin(), out.end(),
                             [&world](const ChunkCoord& c) {
                                 return !chunk_is_live(world.chunk(c));
                             }),
              out.end());
    return out;
}

constexpr u16 brick_slot(u32 bx, u32 by, u32 bz) {
    return static_cast<u16>((bx << 10) | (by << 5) | bz);
}

}  // namespace

u64 world_cache_key(const std::string& source_text, i32 voxels_per_metre, u64 build_stamp) {
    u64 h = hash_mix(kVersion);
    h = hash_combine(h, build_stamp);
    h = hash_combine(h, static_cast<u64>(voxels_per_metre));
    h = hash_combine(h, static_cast<u64>(source_text.size()));
    for (char c : source_text) h = hash_combine(h, static_cast<u64>(static_cast<u8>(c)));
    return h;
}

bool write_world_cache(const std::string& path, u64 key, const WorldCache& cache) {
    if (cache.world == nullptr || cache.types == nullptr) return false;

    // A world differenced against itself is empty by construction, so this would write a file
    // saying "the world is exactly what the clip builds" over a world somebody has spent an
    // evening carving, and it would say it without a single warning. Refused rather than
    // documented: there is no caller for whom this is what they meant.
    if (cache.baseline == cache.world) {
        WS_LOG_WARN("cache", "refusing to write '{}' as a difference from itself", path);
        return false;
    }
    const bool edit_only = cache.baseline != nullptr;

    // Timed, and reported, because this now runs while somebody is playing rather than only at
    // the end of a build. A cost nobody can see is a cost nobody can weigh against the stall it
    // is meant to be saving.
    const u64 began = now_ns();

    std::vector<u8> out;
    out.reserve(1u << 24);
    put_pod(out, kMagic);
    put_pod(out, kVersion);
    put_pod(out, key);
    // Whole world or difference, said in the header rather than worked out from what follows. See
    // WorldCacheMode: an empty payload means opposite things in the two modes.
    put_pod(out, static_cast<u8>(edit_only ? WorldCacheMode::EditOnly : WorldCacheMode::Whole));

    // Tags and properties, by name, so a build that registers them in a different order is
    // rejected by the reader rather than silently mismatched.
    put_pod(out, static_cast<u32>(cache.tags->count()));
    for (u32 i = 0; i < cache.tags->count(); ++i) {
        const std::string name(cache.tags->name(i));
        put_pod(out, static_cast<u32>(name.size()));
        out.insert(out.end(), name.begin(), name.end());
    }
    put_pod(out, static_cast<u32>(cache.properties->count()));
    for (u32 i = 0; i < cache.properties->count(); ++i) {
        const PropertyInfo& info = cache.properties->info(i);
        put_pod(out, static_cast<u32>(info.name.size()));
        out.insert(out.end(), info.name.begin(), info.name.end());
        put_pod(out, static_cast<u8>(info.type));
        put_pod(out, static_cast<u8>(info.domain));
        put_pod(out, info.default_value.bits);
    }

    // The type table, as three arrays. Visual records are fixed-size and go out in one block;
    // behaviour records carry variable-length tag overflow and properties, and there are a
    // handful of them, so they are written one at a time.
    const std::vector<VisualRecord>& visuals = cache.types->visuals();
    put_pod(out, static_cast<u32>(visuals.size()));
    {
        const usize at = out.size();
        out.resize(at + visuals.size() * sizeof(VisualRecord));
        std::memcpy(out.data() + at, visuals.data(), visuals.size() * sizeof(VisualRecord));
    }

    const std::vector<BehaviourRecord>& behaviours = cache.types->behaviours();
    put_pod(out, static_cast<u32>(behaviours.size()));
    for (const BehaviourRecord& record : behaviours) {
        put_pod(out, record.material);
        put_pod(out, record.script);
        const u64* words = record.tags.fast_words();
        for (u32 w = 0; w < kFastTagWords; ++w) put_pod(out, words[w]);
        put_pod(out, static_cast<u32>(record.tags.overflow().size()));
        for (TagId tag : record.tags.overflow()) put_pod(out, tag);
        put_pod(out, static_cast<u32>(record.properties.size()));
        for (const PropertyMap::Entry& entry : record.properties.entries()) {
            put_pod(out, entry.id);
            put_pod(out, entry.value.bits);
        }
    }

    const std::vector<VoxelType>& types = cache.types->types();
    put_pod(out, static_cast<u32>(types.size()));
    {
        const usize at = out.size();
        out.resize(at + types.size() * sizeof(VoxelType));
        std::memcpy(out.data() + at, types.data(), types.size() * sizeof(VoxelType));
    }

    put_pod(out, static_cast<u32>(cache.materials.size()));
    for (VoxelTypeId id : cache.materials) put_pod(out, id);

    // Which boxes of the ladder have been sharpened. See CachedRegion: this is what makes a
    // half-built world worth keeping instead of a trap.
    put_pod(out, static_cast<u32>(cache.regions.size()));
    for (const CachedRegion& region : cache.regions) {
        for (i64 v : region.key) put_pod(out, v);
        put_pod(out, region.level);
        for (f64 v : region.low) put_pod(out, v);
        for (f64 v : region.high) put_pod(out, v);
        put_pod(out, region.applied_per_metre);
        put_pod(out, static_cast<u8>(region.done ? 1u : 0u));
    }

    // Which materials the despeckler may touch. Taken once over the whole clip and unobtainable
    // from anything a resuming run has to hand -- see CachedStipple. The flag goes out separately
    // from the list because "asked, and nothing had specks" and "never asked" are different
    // answers and the reader has to be able to tell them apart.
    put_pod(out, static_cast<u8>(cache.stipple_taken ? 1u : 0u));
    put_pod(out, static_cast<u32>(cache.stipple.size()));
    for (const CachedStipple& entry : cache.stipple) {
        put_pod(out, entry.type);
        put_pod(out, static_cast<u8>(entry.may_despeckle ? 1u : 0u));
    }

    // Where the lamps are, so a loaded world does not have to be read again to find them. R9g,
    // and the same argument as the ledger below: rediscovering them is the order of work this file
    // exists to skip.
    put_pod(out, static_cast<u32>(cache.emitters.size()));
    for (const CachedEmitters& chunk : cache.emitters) {
        put_pod(out, chunk.chunk_x);
        put_pod(out, chunk.chunk_y);
        put_pod(out, chunk.chunk_z);
        put_pod(out, static_cast<u32>(chunk.cells.size()));
        for (const EmissiveCell& cell : chunk.cells) put_pod(out, cell);
    }

    // The ledger's running totals. Recomputing them means counting every voxel in the world,
    // which is the same order of work the cache exists to skip.
    if (cache.ledger != nullptr) {
        put_pod(out, static_cast<u32>(cache.ledger->totals().size()));
        for (const auto& entry : cache.ledger->totals()) {
            put_pod(out, entry.first);
            put_pod(out, entry.second);
        }
    } else {
        put_pod(out, 0u);
    }

    // Chunks: every voxel, or only what the clip would not put there.
    const u32 axis = static_cast<u32>(kChunkBricks);
    u32 written_bricks = 0;
    u32 cleared_bricks = 0;
    u32 kept_bricks = 0;
    if (!edit_only) {
        const std::vector<ChunkCoord> coords = cache.world->sorted_chunk_coords();
        u32 live = 0;
        for (const ChunkCoord& coord : coords) {
            if (chunk_is_live(cache.world->chunk(coord))) ++live;
        }
        put_pod(out, live);
        for (const ChunkCoord& coord : coords) {
            const Chunk* chunk = cache.world->chunk(coord);
            if (!chunk_is_live(chunk)) continue;
            put_pod(out, coord.x);
            put_pod(out, coord.y);
            put_pod(out, coord.z);

            const usize count_at = out.size();
            put_pod(out, 0u);
            u32 bricks = 0;
            for (u32 bz = 0; bz < axis; ++bz) {
                for (u32 by = 0; by < axis; ++by) {
                    for (u32 bx = 0; bx < axis; ++bx) {
                        const Brick* brick = chunk->brick(bx, by, bz);
                        if (!brick_is_live(brick)) continue;
                        put_pod(out, brick_slot(bx, by, bz));
                        write_brick_raw(out, *brick);
                        ++bricks;
                    }
                }
            }
            std::memcpy(out.data() + count_at, &bricks, sizeof(bricks));
            written_bricks += bricks;
        }
    } else {
        // ---- R11f: the difference from what the clip builds ---------------------------------
        //
        // What was edited, if the writer was told. Separate flag, empty list: see
        // WorldCache::edits_named. Written before the difference because it is what decides how
        // much of the difference there is.
        put_pod(out, static_cast<u8>(cache.edits_named ? 1u : 0u));
        put_pod(out, static_cast<u32>(cache.edited.size()));
        for (const CachedEditBox& box : cache.edited) {
            for (i64 v : box.low) put_pod(out, v);
            for (i64 v : box.high) put_pod(out, v);
        }

        // The fingerprint of the baseline this difference was taken against, chunk by chunk.
        //
        // Without it the reader has no way to know that the world it is laying edits over is the
        // world they were cut out of, and the failure is silent by construction: a difference
        // applies cleanly to any world at all, and produces one that is right where somebody
        // carved and quietly wrong everywhere else. Per chunk rather than one number for the
        // world, because a mismatch that can be NAMED is a mismatch somebody can diagnose, and
        // because the reader can then say how much of the world it disagrees about.
        const std::vector<ChunkCoord> base_live = live_chunk_coords(*cache.baseline);
        put_pod(out, static_cast<u32>(base_live.size()));
        for (const ChunkCoord& coord : base_live) {
            put_pod(out, coord.x);
            put_pod(out, coord.y);
            put_pod(out, coord.z);
            put_pod(out, cache.baseline->chunk_hash(coord));
        }

        // Every chunk either world has anything in, in one order.
        const std::vector<ChunkCoord> world_live = live_chunk_coords(*cache.world);
        std::vector<ChunkCoord> all = world_live;
        all.insert(all.end(), base_live.begin(), base_live.end());
        std::sort(all.begin(), all.end(), chunk_coord_less);
        all.erase(std::unique(all.begin(), all.end()), all.end());

        // Chunks the baseline fills and the world does not. An emptied chunk has to be said out
        // loud: "the file does not mention it" already means "leave the clip's answer alone", so
        // a room somebody demolished would come back built.
        std::vector<ChunkCoord> gone;
        for (const ChunkCoord& coord : all) {
            if (!chunk_is_live(cache.world->chunk(coord)) &&
                chunk_is_live(cache.baseline->chunk(coord))) {
                gone.push_back(coord);
            }
        }
        put_pod(out, static_cast<u32>(gone.size()));
        for (const ChunkCoord& coord : gone) {
            put_pod(out, coord.x);
            put_pod(out, coord.y);
            put_pod(out, coord.z);
        }

        u32 changed_chunks = 0;
        const usize changed_at = out.size();
        put_pod(out, 0u);

        std::vector<const CachedEditBox*> reaching;
        std::vector<u16> clears;
        std::vector<u8> writes;
        for (const ChunkCoord& coord : all) {
            const Chunk* chunk = cache.world->chunk(coord);
            if (!chunk_is_live(chunk)) continue;   // an emptied chunk is already in `gone`
            const Chunk* base = cache.baseline->chunk(coord);

            // Which named edit boxes reach into this chunk at all, so the per-brick test below is
            // against a handful of boxes rather than the whole log.
            const i64 chunk_low[3] = {coord.x * kChunkEdge, coord.y * kChunkEdge,
                                      coord.z * kChunkEdge};
            const i64 chunk_high[3] = {chunk_low[0] + kChunkEdge - 1, chunk_low[1] + kChunkEdge - 1,
                                       chunk_low[2] + kChunkEdge - 1};
            reaching.clear();
            for (const CachedEditBox& box : cache.edited) {
                if (box_touches(box, chunk_low, chunk_high)) reaching.push_back(&box);
            }

            clears.clear();
            writes.clear();
            u32 write_count = 0;
            for (u32 bz = 0; bz < axis; ++bz) {
                for (u32 by = 0; by < axis; ++by) {
                    for (u32 bx = 0; bx < axis; ++bx) {
                        const Brick* mine = chunk->brick(bx, by, bz);
                        const Brick* theirs = (base != nullptr) ? base->brick(bx, by, bz) : nullptr;
                        const bool mine_live = brick_is_live(mine);
                        const bool theirs_live = brick_is_live(theirs);
                        if (!mine_live && !theirs_live && reaching.empty()) continue;

                        bool named = false;
                        if (!reaching.empty()) {
                            const i64 low[3] = {chunk_low[0] + static_cast<i64>(bx) * kBrickEdge,
                                                chunk_low[1] + static_cast<i64>(by) * kBrickEdge,
                                                chunk_low[2] + static_cast<i64>(bz) * kBrickEdge};
                            const i64 high[3] = {low[0] + kBrickEdge - 1, low[1] + kBrickEdge - 1,
                                                 low[2] + kBrickEdge - 1};
                            for (const CachedEditBox* box : reaching) {
                                if (box_touches(*box, low, high)) {
                                    named = true;
                                    break;
                                }
                            }
                        }

                        if (mine_live) {
                            if (!named && theirs_live && bricks_identical(*mine, *theirs)) {
                                ++kept_bricks;   // the clip's own answer; not written at all
                                continue;
                            }
                            const usize at = writes.size();
                            writes.resize(at + sizeof(u16));
                            const u16 slot = brick_slot(bx, by, bz);
                            std::memcpy(writes.data() + at, &slot, sizeof(slot));
                            write_brick_raw(writes, *mine);
                            ++write_count;
                            ++written_bricks;
                        } else if (theirs_live || named) {
                            // Air, said rather than left out. `theirs_live` is the carve that took
                            // away matter the clip puts back, and `named` is the same carve
                            // through air the clip happens to agree about today -- which costs two
                            // bytes and is the difference between a hole that survives a change to
                            // the clip and one that fills itself in.
                            clears.push_back(brick_slot(bx, by, bz));
                            ++cleared_bricks;
                        }
                    }
                }
            }
            if (clears.empty() && writes.empty()) continue;

            put_pod(out, coord.x);
            put_pod(out, coord.y);
            put_pod(out, coord.z);
            put_pod(out, static_cast<u32>(clears.size()));
            for (u16 slot : clears) put_pod(out, slot);
            put_pod(out, write_count);
            out.insert(out.end(), writes.begin(), writes.end());
            ++changed_chunks;
        }
        std::memcpy(out.data() + changed_at, &changed_chunks, sizeof(changed_chunks));
        WS_LOG_INFO("cache",
                    "'{}' as the clip plus its edits: {} bricks written, {} cleared, {} left to "
                    "the clip; {} chunks fingerprinted, {} edit boxes named{}",
                    path, written_bricks, cleared_bricks, kept_bricks, base_live.size(),
                    cache.edited.size(),
                    cache.edits_named ? "" : " (nobody said what was edited)");
    }

    // Written under a temporary name and renamed, so an interrupted run leaves the previous
    // cache intact rather than a half-file that passes its own header check.
    const std::string temporary = path + ".part";
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file) {
            WS_LOG_WARN("cache", "could not open '{}' for writing", temporary);
            return false;
        }
        file.write(reinterpret_cast<const char*>(out.data()),
                   static_cast<std::streamsize>(out.size()));
        if (!file) {
            WS_LOG_WARN("cache", "could not write '{}'", temporary);
            return false;
        }
    }
    std::remove(path.c_str());
    if (std::rename(temporary.c_str(), path.c_str()) != 0) {
        WS_LOG_WARN("cache", "could not rename '{}' into place", temporary);
        return false;
    }
    WS_LOG_INFO("cache", "wrote '{}' ({} MB in {:.0f} ms)", path, out.size() / (1024 * 1024),
                ns_to_ms(now_ns() - began));
    return true;
}

// Does the file on disk belong to this key, without reading the file.
//
// Sixteen bytes rather than six hundred megabytes. The caller uses it to decide whether a cached
// world is dead and should be deleted, and reading a third of a gigabyte off a disk to answer that
// would cost more than the rebuild it is trying to avoid announcing.
bool world_cache_matches(const std::string& path, u64 key) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    u32 magic = 0, version = 0;
    u64 stored = 0;
    file.read(reinterpret_cast<char*>(&magic), sizeof(magic));
    file.read(reinterpret_cast<char*>(&version), sizeof(version));
    file.read(reinterpret_cast<char*>(&stored), sizeof(stored));
    if (!file) return false;
    return magic == kMagic && version == kVersion && stored == key;
}

// The seventeenth byte, and a caller has to have it before it commits to anything.
//
// An edit-only file cannot be opened without the world its clip builds, and building that world is
// the expensive thing the caller is deciding about. Discovering the requirement inside the read —
// after the type table, after the region list — leaves the caller holding a failure it could have
// been told about for seventeen bytes.
bool world_cache_mode_of(const std::string& path, WorldCacheMode& out) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    u32 magic = 0, version = 0;
    u64 stored = 0;
    u8 mode = 0;
    file.read(reinterpret_cast<char*>(&magic), sizeof(magic));
    file.read(reinterpret_cast<char*>(&version), sizeof(version));
    file.read(reinterpret_cast<char*>(&stored), sizeof(stored));
    file.read(reinterpret_cast<char*>(&mode), sizeof(mode));
    if (!file) return false;
    if (magic != kMagic || version != kVersion) return false;
    if (mode != static_cast<u8>(WorldCacheMode::Whole) &&
        mode != static_cast<u8>(WorldCacheMode::EditOnly)) {
        return false;
    }
    out = static_cast<WorldCacheMode>(mode);
    return true;
}

bool read_world_cache(const std::string& path, u64 key, WorldCache& cache, JobSystem* jobs) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return false;
    const std::streamsize size = file.tellg();
    if (size <= 0) return false;
    file.seekg(0);

    std::vector<u8> blob(static_cast<usize>(size));
    if (!file.read(reinterpret_cast<char*>(blob.data()), size)) return false;

    Cursor in{blob.data(), blob.size(), 0, true};
    if (in.pod<u32>() != kMagic) return false;
    if (in.pod<u32>() != kVersion) return false;
    if (in.pod<u64>() != key) return false;   // built from different source; not an error

    const u8 stored_mode = in.pod<u8>();
    if (!in.ok) return false;
    if (stored_mode != static_cast<u8>(WorldCacheMode::Whole) &&
        stored_mode != static_cast<u8>(WorldCacheMode::EditOnly)) {
        return false;
    }
    cache.mode = static_cast<WorldCacheMode>(stored_mode);
    cache.baseline_agreed = true;
    cache.baseline_chunks_differing = 0;
    // Refused rather than muddled through. A difference applied over nothing is a building
    // reduced to the holes cut in it -- which looks so much like a world that is still loading
    // that it would be argued about for an hour before anybody suspected the file.
    if (cache.mode == WorldCacheMode::EditOnly && cache.baseline == nullptr) {
        WS_LOG_WARN("cache",
                    "'{}' is a world's difference from its clip and no clip-built world was "
                    "given to lay it over; not loading it",
                    path);
        return false;
    }

    const u32 tag_count = in.pod<u32>();
    for (u32 i = 0; i < tag_count && in.ok; ++i) {
        const u32 length = in.pod<u32>();
        const u8* text = in.take(length);
        if (!in.ok) return false;
        if (cache.tags->intern(std::string(reinterpret_cast<const char*>(text), length)) != i) {
            return false;
        }
    }
    const u32 property_count = in.pod<u32>();
    for (u32 i = 0; i < property_count && in.ok; ++i) {
        const u32 length = in.pod<u32>();
        const u8* text = in.take(length);
        if (!in.ok) return false;
        const auto type = static_cast<PropertyType>(in.pod<u8>());
        const auto domain = static_cast<PropertyDomain>(in.pod<u8>());
        const PropertyValue value{in.pod<u64>()};
        if (cache.properties->define(
                std::string(reinterpret_cast<const char*>(text), length), type, domain, value) !=
            i) {
            return false;
        }
    }
    if (!in.ok) return false;

    const u32 visual_count = in.pod<u32>();
    std::vector<VisualRecord> visuals(visual_count);
    {
        const u8* p = in.take(visual_count * sizeof(VisualRecord));
        if (!in.ok) return false;
        std::memcpy(visuals.data(), p, visual_count * sizeof(VisualRecord));
    }

    const u32 behaviour_count = in.pod<u32>();
    std::vector<BehaviourRecord> behaviours(behaviour_count);
    for (BehaviourRecord& record : behaviours) {
        record.material = in.pod<u32>();
        record.script = in.pod<u32>();
        for (u32 w = 0; w < kFastTagWords; ++w) {
            const u64 word = in.pod<u64>();
            for (u32 bit = 0; bit < 64; ++bit) {
                if ((word >> bit) & 1u) record.tags.add(w * 64 + bit);
            }
        }
        const u32 overflow = in.pod<u32>();
        for (u32 i = 0; i < overflow && in.ok; ++i) record.tags.add(in.pod<u32>());
        const u32 entries = in.pod<u32>();
        for (u32 i = 0; i < entries && in.ok; ++i) {
            const PropertyId id = in.pod<u32>();
            record.properties.set(id, PropertyValue{in.pod<u64>()});
        }
        if (!in.ok) return false;
    }

    const u32 type_count = in.pod<u32>();
    std::vector<VoxelType> types(type_count);
    {
        const u8* p = in.take(type_count * sizeof(VoxelType));
        if (!in.ok) return false;
        std::memcpy(types.data(), p, type_count * sizeof(VoxelType));
    }
    cache.types->adopt(std::move(visuals), std::move(behaviours), std::move(types));

    const u32 material_count = in.pod<u32>();
    cache.materials.clear();
    for (u32 i = 0; i < material_count && in.ok; ++i) cache.materials.push_back(in.pod<u32>());

    const u32 region_count = in.pod<u32>();
    cache.regions.clear();
    if (!in.ok) return false;
    cache.regions.reserve(region_count);
    for (u32 i = 0; i < region_count && in.ok; ++i) {
        CachedRegion region;
        for (i64& v : region.key) v = in.pod<i64>();
        region.level = in.pod<u32>();
        for (f64& v : region.low) v = in.pod<f64>();
        for (f64& v : region.high) v = in.pod<f64>();
        region.applied_per_metre = in.pod<i32>();
        region.done = in.pod<u8>() != 0u;
        cache.regions.push_back(region);
    }
    if (!in.ok) return false;

    cache.stipple_taken = in.pod<u8>() != 0u;
    const u32 stipple_count = in.pod<u32>();
    cache.stipple.clear();
    if (!in.ok) return false;
    cache.stipple.reserve(stipple_count);
    for (u32 i = 0; i < stipple_count && in.ok; ++i) {
        CachedStipple entry;
        entry.type = in.pod<u32>();
        entry.may_despeckle = in.pod<u8>() != 0u;
        cache.stipple.push_back(entry);
    }
    if (!in.ok) return false;

    const u32 emitter_chunks = in.pod<u32>();
    cache.emitters.clear();
    if (!in.ok) return false;
    cache.emitters.reserve(emitter_chunks);
    for (u32 i = 0; i < emitter_chunks && in.ok; ++i) {
        CachedEmitters chunk;
        chunk.chunk_x = in.pod<i64>();
        chunk.chunk_y = in.pod<i64>();
        chunk.chunk_z = in.pod<i64>();
        const u32 cell_count = in.pod<u32>();
        if (!in.ok) return false;
        chunk.cells.reserve(cell_count);
        for (u32 c = 0; c < cell_count && in.ok; ++c) chunk.cells.push_back(in.pod<EmissiveCell>());
        cache.emitters.push_back(std::move(chunk));
    }
    if (!in.ok) return false;

    const u32 ledger_count = in.pod<u32>();
    // The totals in the file are the WHOLE world's, taken after the edits, in both modes. On the
    // edit-only path the caller has already built the clip's world into the baseline and charged
    // its ledger for every voxel of it, so adding these on top would count the building twice and
    // the audit would report matter nobody created. Reset and restate. This clears the per-player
    // accounting with it, which is correct here and only here: this is a load, and a load is
    // exactly the moment a ledger has no history worth keeping.
    if (cache.mode == WorldCacheMode::EditOnly && cache.ledger != nullptr) cache.ledger->clear();
    for (u32 i = 0; i < ledger_count && in.ok; ++i) {
        const VoxelTypeId type = in.pod<u32>();
        const i64 total = in.pod<i64>();
        if (cache.ledger != nullptr && total > 0) {
            cache.ledger->record_bulk(kAir, type, static_cast<u64>(total), MatterReason::Load);
        }
    }
    if (!in.ok) return false;

    // Chunks. Created on this thread — the world's map is not safe to insert into from several —
    // and then filled in parallel, because once a chunk exists it is an independent object.
    struct Pending {
        Chunk* chunk = nullptr;
        ChunkCoord coord;
        const u8* clears = nullptr;   // brick slots to empty; edit-only, null in the whole form
        u32 clear_count = 0;
        const u8* data = nullptr;
        usize size = 0;
        u32 bricks = 0;
    };
    std::vector<Pending> pending;
    std::vector<ChunkCoord> emptied;

    // Every brick of a chunk taken back to air, through the route that unlinks it. Not a
    // convenience: a brick left allocated with nothing in it is `world_has` claiming matter the
    // world does not have, which the marcher draws as a filled cube it can never build (D620).
    const auto strip_chunk = [](Chunk& chunk) {
        const u32 axis = static_cast<u32>(kChunkBricks);
        for (u32 bz = 0; bz < axis; ++bz) {
            for (u32 by = 0; by < axis; ++by) {
                for (u32 bx = 0; bx < axis; ++bx) {
                    if (chunk.brick(bx, by, bz) == nullptr) continue;
                    chunk.brick_for_write(bx, by, bz).fill(kAir);
                    chunk.drop_brick_if_empty(bx, by, bz);
                }
            }
        }
        chunk.mark_modified();
    };

    if (cache.mode == WorldCacheMode::EditOnly) {
        // ---- R11f: the difference, and the world it is a difference from --------------------
        cache.edits_named = in.pod<u8>() != 0u;
        const u32 edit_boxes = in.pod<u32>();
        cache.edited.clear();
        if (!in.ok) return false;
        cache.edited.reserve(edit_boxes);
        for (u32 i = 0; i < edit_boxes && in.ok; ++i) {
            CachedEditBox box;
            for (i64& v : box.low) v = in.pod<i64>();
            for (i64& v : box.high) v = in.pod<i64>();
            cache.edited.push_back(box);
        }
        if (!in.ok) return false;

        // Is the world about to be written over the world this difference was cut out of? Asked
        // before anything is applied, and asked chunk by chunk so the answer can be a NUMBER --
        // "the baseline disagrees" is not diagnosable and "9 of 74 chunks disagree" is.
        const u32 fingerprint = in.pod<u32>();
        if (!in.ok) return false;
        std::vector<ChunkCoord> expect(fingerprint);
        std::vector<u64> expect_hash(fingerprint);
        for (u32 i = 0; i < fingerprint && in.ok; ++i) {
            expect[i].x = in.pod<i64>();
            expect[i].y = in.pod<i64>();
            expect[i].z = in.pod<i64>();
            expect_hash[i] = in.pod<u64>();
        }
        if (!in.ok) return false;

        // A hash per chunk is every voxel of the baseline read once, so it goes wide when there
        // is a pool to go wide on. One byte of answer per chunk rather than an atomic: the ranges
        // are disjoint and the sum is trivial afterwards.
        const u64 verify_began = now_ns();
        std::vector<u8> disagrees(fingerprint, 0u);
        const auto verify = [&](usize from, usize to) {
            for (usize i = from; i < to; ++i) {
                disagrees[i] = (cache.baseline->chunk_hash(expect[i]) != expect_hash[i]) ? 1u : 0u;
            }
        };
        if (jobs != nullptr && fingerprint > 1) {
            jobs->parallel_for(fingerprint, 1, verify);
        } else {
            verify(0, fingerprint);
        }
        u32 differing = 0;
        for (u8 flag : disagrees) differing += flag;

        // And the other direction, which the hashes above cannot see: matter in the baseline that
        // the file never knew about. It would survive into the loaded world as if somebody had
        // built it, so it counts as a disagreement even though every named chunk agreed.
        std::unordered_map<ChunkCoord, u8, ChunkCoordHash> named;
        named.reserve(static_cast<usize>(fingerprint) * 2 + 1);
        for (const ChunkCoord& coord : expect) named.emplace(coord, static_cast<u8>(0));
        u32 unexpected = 0;
        cache.baseline->for_each_chunk([&](const ChunkCoord& coord, const Chunk& chunk) {
            if (chunk.empty()) return;
            if (named.find(coord) == named.end()) ++unexpected;
        });
        cache.baseline_chunks_differing = differing + unexpected;
        cache.baseline_agreed = cache.baseline_chunks_differing == 0;
        if (!cache.baseline_agreed) {
            // Said and then gone ahead with. Refusing here hands the caller nothing, and a caller
            // with nothing rebuilds from the clip -- which loses every carving outright, where
            // going ahead loses at most the bricks the file judged identical to a clip that has
            // since moved. See WorldCache::baseline_agreed.
            WS_LOG_WARN("cache",
                        "'{}' was cut from a different world than the one it is being laid over: "
                        "{} of {} chunks disagree ({} the file never saw). The edits it names are "
                        "applied; everything it left to the clip is whatever the clip builds now",
                        path, cache.baseline_chunks_differing, fingerprint, unexpected);
        }
        WS_LOG_INFO("cache", "checked the clip's world against '{}' in {:.0f} ms ({} chunks)", path,
                    ns_to_ms(now_ns() - verify_began), fingerprint);

        // The baseline becomes the world, unless it already is it. Pointing `baseline` at `world`
        // is the ordinary path and the difference lands in place; a separate baseline is copied,
        // which costs a world and is the caller's choice to make.
        if (cache.baseline != cache.world) {
            cache.baseline->for_each_chunk([&](const ChunkCoord& coord, const Chunk& chunk) {
                if (chunk.empty()) return;
                cache.world->chunk_for_write(coord) = chunk;
            });
        }

        // Chunks the clip fills and the saved world does not.
        const u32 gone_count = in.pod<u32>();
        if (!in.ok) return false;
        for (u32 i = 0; i < gone_count && in.ok; ++i) {
            ChunkCoord coord;
            coord.x = in.pod<i64>();
            coord.y = in.pod<i64>();
            coord.z = in.pod<i64>();
            if (!in.ok) return false;
            if (!cache.world->has_chunk(coord)) continue;
            strip_chunk(cache.world->chunk_for_write(coord));
            emptied.push_back(coord);
        }
        if (!in.ok) return false;

        const u32 changed = in.pod<u32>();
        if (!in.ok) return false;
        pending.reserve(changed);
        for (u32 i = 0; i < changed && in.ok; ++i) {
            ChunkCoord coord;
            coord.x = in.pod<i64>();
            coord.y = in.pod<i64>();
            coord.z = in.pod<i64>();
            const u32 clear_count = in.pod<u32>();
            if (!in.ok) return false;
            const u8* clears = in.take(clear_count * sizeof(u16));
            if (!in.ok) return false;
            const u32 bricks = in.pod<u32>();
            if (!in.ok) return false;

            const usize begin = in.at;
            for (u32 b = 0; b < bricks; ++b) {
                in.take(sizeof(u16));
                if (!in.ok) return false;
                const usize span = brick_span(blob.data() + in.at, in.size - in.at);
                if (span == 0) return false;
                in.take(span);
                if (!in.ok) return false;
            }
            pending.push_back({&cache.world->chunk_for_write(coord), coord, clears, clear_count,
                               blob.data() + begin, in.at - begin, bricks});
        }
        if (!in.ok) return false;
    } else {
        // A whole-world file says nothing at all about what was edited, which is not the same as
        // saying nothing was. Trap 7, one more time: leave the claim unmade.
        cache.edits_named = false;
        cache.edited.clear();

        const u32 chunk_count = in.pod<u32>();
        pending.reserve(chunk_count);
        for (u32 i = 0; i < chunk_count && in.ok; ++i) {
            ChunkCoord coord;
            coord.x = in.pod<i64>();
            coord.y = in.pod<i64>();
            coord.z = in.pod<i64>();
            const u32 bricks = in.pod<u32>();
            if (!in.ok) return false;

            // Walk the bricks once to find where this chunk's payload ends, without decoding them.
            const usize begin = in.at;
            for (u32 b = 0; b < bricks; ++b) {
                in.take(sizeof(u16));
                if (!in.ok) return false;
                const usize span = brick_span(blob.data() + in.at, in.size - in.at);
                if (span == 0) return false;
                in.take(span);
                if (!in.ok) return false;
            }
            pending.push_back({&cache.world->chunk_for_write(coord), coord, nullptr, 0,
                               blob.data() + begin, in.at - begin, bricks});
        }
        if (!in.ok) return false;
    }

    const auto fill = [&](usize from, usize to) {
        for (usize i = from; i < to; ++i) {
            Pending& job = pending[i];
            // Clearings first. A brick is named here because it is air in the saved world and the
            // clip puts something there -- the carve -- so it has to go before anything else in
            // this chunk is judged, and it has to go through drop_brick_if_empty (D620).
            for (u32 c = 0; c < job.clear_count; ++c) {
                u16 slot = 0;
                std::memcpy(&slot, job.clears + c * sizeof(u16), sizeof(slot));
                const u32 bx = (slot >> 10) & 31u;
                const u32 by = (slot >> 5) & 31u;
                const u32 bz = slot & 31u;
                if (job.chunk->brick(bx, by, bz) == nullptr) continue;
                job.chunk->brick_for_write(bx, by, bz).fill(kAir);
                job.chunk->drop_brick_if_empty(bx, by, bz);
            }
            usize at = 0;
            for (u32 b = 0; b < job.bricks; ++b) {
                u16 slot = 0;
                std::memcpy(&slot, job.data + at, sizeof(slot));
                at += sizeof(slot);
                const u32 bx = (slot >> 10) & 31u;
                const u32 by = (slot >> 5) & 31u;
                const u32 bz = slot & 31u;
                at += read_brick_raw(job.data + at, job.size - at,
                                     job.chunk->brick_for_write(bx, by, bz));
            }
            job.chunk->mark_modified();
        }
    };
    if (jobs != nullptr && pending.size() > 1) {
        jobs->parallel_for(pending.size(), 1, fill);
    } else {
        fill(0, pending.size());
    }

    // A chunk whose last brick went with an edit has to leave the map, for the reason above one
    // level up: `NodePool::world_has` answers above level 8 out of which chunks EXIST, so a chunk
    // emptied by a carve goes on claiming occupancy for the rest of the run (D344's phantom). On
    // this thread, because it erases from the world's map.
    for (const Pending& job : pending) cache.world->drop_chunk_if_empty(job.coord);
    for (const ChunkCoord& coord : emptied) cache.world->drop_chunk_if_empty(coord);
    return true;
}

}  // namespace ws
