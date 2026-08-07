#include "world/world_cache.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

#include "core/hash.hpp"
#include "core/jobs.hpp"
#include "core/log.hpp"
#include "world/brick.hpp"
#include "world/ledger.hpp"
#include "world/property.hpp"
#include "world/tags.hpp"
#include "world/world.hpp"

namespace ws {
namespace {

constexpr u32 kMagic = 0x57534357u;   // "WSCW"
constexpr u32 kVersion = 1u;

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

}  // namespace

u64 world_cache_key(const std::string& source_text, i32 voxels_per_metre) {
    u64 h = hash_mix(kVersion);
    h = hash_combine(h, static_cast<u64>(voxels_per_metre));
    h = hash_combine(h, static_cast<u64>(source_text.size()));
    for (char c : source_text) h = hash_combine(h, static_cast<u64>(static_cast<u8>(c)));
    return h;
}

bool write_world_cache(const std::string& path, u64 key, const WorldCache& cache) {
    if (cache.world == nullptr || cache.types == nullptr) return false;

    std::vector<u8> out;
    out.reserve(1u << 24);
    put_pod(out, kMagic);
    put_pod(out, kVersion);
    put_pod(out, key);

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

    // Chunks.
    const std::vector<ChunkCoord> coords = cache.world->sorted_chunk_coords();
    u32 live = 0;
    for (const ChunkCoord& coord : coords) {
        const Chunk* chunk = cache.world->chunk(coord);
        if (chunk != nullptr && !chunk->empty()) ++live;
    }
    put_pod(out, live);
    for (const ChunkCoord& coord : coords) {
        const Chunk* chunk = cache.world->chunk(coord);
        if (chunk == nullptr || chunk->empty()) continue;
        put_pod(out, coord.x);
        put_pod(out, coord.y);
        put_pod(out, coord.z);

        const usize count_at = out.size();
        put_pod(out, 0u);
        u32 bricks = 0;
        const u32 axis = static_cast<u32>(kChunkBricks);
        for (u32 bz = 0; bz < axis; ++bz) {
            for (u32 by = 0; by < axis; ++by) {
                for (u32 bx = 0; bx < axis; ++bx) {
                    const Brick* brick = chunk->brick(bx, by, bz);
                    if (brick == nullptr || brick->empty()) continue;
                    const u16 slot = static_cast<u16>((bx << 10) | (by << 5) | bz);
                    put_pod(out, slot);
                    write_brick_raw(out, *brick);
                    ++bricks;
                }
            }
        }
        std::memcpy(out.data() + count_at, &bricks, sizeof(bricks));
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
    WS_LOG_INFO("cache", "wrote '{}' ({} MB)", path, out.size() / (1024 * 1024));
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

    const u32 ledger_count = in.pod<u32>();
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
        const u8* data = nullptr;
        usize size = 0;
        u32 bricks = 0;
    };
    std::vector<Pending> pending;
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
        pending.push_back({&cache.world->chunk_for_write(coord), blob.data() + begin,
                           in.at - begin, bricks});
    }
    if (!in.ok) return false;

    const auto fill = [&](usize from, usize to) {
        for (usize i = from; i < to; ++i) {
            Pending& job = pending[i];
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
    return true;
}

}  // namespace ws
