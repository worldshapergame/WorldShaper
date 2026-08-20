#include "world/emitter_store.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>

#include "core/hash.hpp"
#include "core/log.hpp"
#include "world/chunk.hpp"

namespace ws {

namespace {

// The record goes to disk and to the hash as bytes, so its size is part of the file format rather
// than an implementation detail. Nine i64, three f32 and a u32 is 88 bytes with no padding on any
// target this builds for; if that ever stops being true the format has changed and the version
// below has to change with it.
static_assert(sizeof(EmissiveCell) == 88, "EmissiveCell is written to the sidecar as-is");

constexpr u32 kSidecarMagic = 0x504D414Cu;   // "LAMP"
// 1 — the first. `src/world` is one of the directories the world cache stamps its key from, so any
// edit here already invalidates every cached world; the version is here so a sidecar that outlives
// its cache is refused rather than misread.
constexpr u32 kSidecarVersion = 1u;

constexpr usize kHeaderBytes = 4 + 4 + 8 + 4 + 4 + 8;   // magic, version, key, chunks, cells, hash

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
};

bool chunk_coord_less(const ChunkCoord& a, const ChunkCoord& b) {
    if (a.x != b.x) return a.x < b.x;
    if (a.y != b.y) return a.y < b.y;
    return a.z < b.z;
}

}  // namespace

const std::vector<EmissiveCell>* EmitterStore::cells(const ChunkCoord& coord) const {
    const auto found = by_chunk_.find(coord);
    return (found == by_chunk_.end()) ? nullptr : &found->second;
}

void EmitterStore::remember(const ChunkCoord& coord, std::vector<EmissiveCell> cells) {
    by_chunk_[coord] = std::move(cells);
}

bool EmitterStore::forget(const ChunkCoord& coord) { return by_chunk_.erase(coord) > 0; }

usize EmitterStore::forget_box(const i64 lo[3], const i64 hi[3]) {
    usize dropped = 0;
    for (i64 cz = chunk_of(lo[2]); cz <= chunk_of(hi[2]); ++cz) {
        for (i64 cy = chunk_of(lo[1]); cy <= chunk_of(hi[1]); ++cy) {
            for (i64 cx = chunk_of(lo[0]); cx <= chunk_of(hi[0]); ++cx) {
                dropped += by_chunk_.erase(ChunkCoord{cx, cy, cz});
            }
        }
    }
    return dropped;
}

usize EmitterStore::cell_count() const {
    usize total = 0;
    for (const auto& [coord, cells] : by_chunk_) total += cells.size();
    return total;
}

std::vector<ChunkCoord> EmitterStore::sorted_coords() const {
    std::vector<ChunkCoord> out;
    out.reserve(by_chunk_.size());
    for (const auto& [coord, cells] : by_chunk_) out.push_back(coord);
    std::sort(out.begin(), out.end(), chunk_coord_less);
    return out;
}

EmitterScan EmitterStore::refresh(const World& world, const VoxelTypeTable& types,
                                  EmitterResidency residency) {
    EmitterScan count;

    // What is in front of us, read once. A chunk already known is not read again — that is D587's
    // half of this stage and it is what took the rebuild from 13.99 ms to 2.54.
    world.for_each_chunk([&](const ChunkCoord& coord, const Chunk& chunk) {
        if (by_chunk_.find(coord) != by_chunk_.end()) {
            ++count.reused;
            return;
        }
        ++count.scanned;
        by_chunk_.emplace(coord, scan_chunk_emitters(chunk, coord.x * static_cast<i64>(kChunkEdge),
                                                     coord.y * static_cast<i64>(kChunkEdge),
                                                     coord.z * static_cast<i64>(kChunkEdge),
                                                     types));
    });

    // ...and what is not. This is the whole of R9g's second half and it is four lines: a chunk this
    // store remembers and the world does not hold is a chunk whose LAMPS are still there. Nothing
    // announced their removal — an announced removal comes through `forget_box` — so the fittings
    // stand and the list contains them.
    //
    // The control arm drops them instead, which is what the list did before this existed: light
    // that is a fact about residency.
    if (residency == EmitterResidency::kKeep) {
        for (const auto& [coord, cells] : by_chunk_) {
            if (!world.has_chunk(coord)) ++count.absent;
        }
    } else {
        std::vector<ChunkCoord> gone;
        for (const auto& [coord, cells] : by_chunk_) {
            if (!world.has_chunk(coord)) gone.push_back(coord);
        }
        for (const ChunkCoord& coord : gone) by_chunk_.erase(coord);
        count.dropped = static_cast<u32>(gone.size());
    }
    return count;
}

std::vector<EmissiveCell> EmitterStore::cells_for_merge() const {
    std::vector<EmissiveCell> out;
    out.reserve(cell_count());
    for (const ChunkCoord& coord : sorted_coords()) {
        const std::vector<EmissiveCell>& mine = by_chunk_.find(coord)->second;
        out.insert(out.end(), mine.begin(), mine.end());
    }
    return out;
}

std::vector<LightSource> build_light_list_from_store(const EmitterStore& store, i64 centre_x,
                                                     i64 centre_y, i64 centre_z) {
    return merge_light_list(store.cells_for_merge(), centre_x, centre_y, centre_z);
}

u64 emitter_store_hash(const EmitterStore& store) {
    u64 hash = hash_mix(static_cast<u64>(store.chunks()) + 0x9E3779B97F4A7C15ull);
    for (const ChunkCoord& coord : store.sorted_coords()) {
        hash = hash_combine(hash, static_cast<u64>(coord.x));
        hash = hash_combine(hash, static_cast<u64>(coord.y));
        hash = hash_combine(hash, static_cast<u64>(coord.z));
        const std::vector<EmissiveCell>& cells = *store.cells(coord);
        // The length as well as the bytes, so a chunk known to hold nothing cannot hash the same as
        // a chunk nobody has looked at. Those are the two answers this whole file keeps apart.
        hash = hash_combine(hash, static_cast<u64>(cells.size()) + 1ull);
        if (!cells.empty()) {
            hash = hash_bytes(reinterpret_cast<const u8*>(cells.data()),
                              cells.size() * sizeof(EmissiveCell), hash);
        }
    }
    return hash;
}

std::string emitter_sidecar_path(const std::string& world_cache_path) {
    return world_cache_path + ".lamps";
}

bool write_emitter_store(const std::string& path, u64 key, const EmitterStore& store) {
    std::vector<u8> payload;
    // 88 bytes a cell and a dozen cells a chunk: a whole world's lamps are tens of kilobytes, which
    // is the arithmetic this stage rests on.
    payload.reserve(store.cell_count() * sizeof(EmissiveCell) + store.chunks() * 32);

    const std::vector<ChunkCoord> order = store.sorted_coords();
    for (const ChunkCoord& coord : order) {
        const std::vector<EmissiveCell>& cells = *store.cells(coord);
        put_pod(payload, coord.x);
        put_pod(payload, coord.y);
        put_pod(payload, coord.z);
        put_pod(payload, static_cast<u32>(cells.size()));
        for (const EmissiveCell& cell : cells) put_pod(payload, cell);
    }

    std::vector<u8> head;
    head.reserve(kHeaderBytes);
    put_pod(head, kSidecarMagic);
    put_pod(head, kSidecarVersion);
    put_pod(head, key);
    put_pod(head, static_cast<u32>(order.size()));
    put_pod(head, static_cast<u32>(store.cell_count()));
    // Over the payload only, so the hash can be checked against what was actually read back.
    put_pod(head, payload.empty() ? 0ull
                                  : hash_bytes(payload.data(), payload.size(),
                                               0xCBF29CE484222325ull));

    // Written under a temporary name and renamed, the same way the world cache is: an interrupted
    // run leaves the previous sidecar intact rather than a half-file that passes its own header.
    const std::string temporary = path + ".part";
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file) {
            WS_LOG_WARN("light", "could not open '{}' for writing", temporary);
            return false;
        }
        file.write(reinterpret_cast<const char*>(head.data()),
                   static_cast<std::streamsize>(head.size()));
        if (!payload.empty()) {
            file.write(reinterpret_cast<const char*>(payload.data()),
                       static_cast<std::streamsize>(payload.size()));
        }
        if (!file) {
            WS_LOG_WARN("light", "could not write '{}'", temporary);
            return false;
        }
    }
    std::remove(path.c_str());
    if (std::rename(temporary.c_str(), path.c_str()) != 0) {
        WS_LOG_WARN("light", "could not rename '{}' into place", temporary);
        return false;
    }
    return true;
}

bool read_emitter_store(const std::string& path, u64 key, EmitterStore& store) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return false;
    const std::streamoff size = file.tellg();
    if (size < static_cast<std::streamoff>(kHeaderBytes)) return false;
    file.seekg(0, std::ios::beg);

    std::vector<u8> raw(static_cast<usize>(size));
    file.read(reinterpret_cast<char*>(raw.data()), size);
    if (!file) return false;

    Cursor at{raw.data(), raw.size(), 0, true};
    const u32 magic = at.pod<u32>();
    const u32 version = at.pod<u32>();
    const u64 written_for = at.pod<u64>();
    const u32 chunks = at.pod<u32>();
    const u32 cells = at.pod<u32>();
    const u64 want = at.pod<u64>();
    if (!at.ok || magic != kSidecarMagic || version != kSidecarVersion) return false;
    if (written_for != key) {
        // Not a corruption and worth saying so: a sidecar left behind by another world is the one
        // failure here that would otherwise light a building with somebody else's lamps.
        WS_LOG_INFO("light", "'{}' was written for another world; scanning for the lamps instead",
                    path);
        return false;
    }

    const usize payload_at = at.at;
    const u64 have = (raw.size() > payload_at)
                         ? hash_bytes(raw.data() + payload_at, raw.size() - payload_at,
                                      0xCBF29CE484222325ull)
                         : 0ull;
    if (have != want) {
        WS_LOG_WARN("light", "'{}' does not hash to its own header; scanning for the lamps instead",
                    path);
        return false;
    }

    // Decoded whole before anything is put anywhere, so a file that runs out halfway leaves the
    // store exactly as it was. A half-loaded set of lamps is a building lit in patches, and it
    // would look like a rendering fault rather than like a bad file.
    std::vector<std::pair<ChunkCoord, std::vector<EmissiveCell>>> decoded;
    decoded.reserve(chunks);
    usize seen_cells = 0;
    for (u32 i = 0; i < chunks; ++i) {
        ChunkCoord coord;
        coord.x = at.pod<i64>();
        coord.y = at.pod<i64>();
        coord.z = at.pod<i64>();
        const u32 count = at.pod<u32>();
        if (!at.ok) return false;
        // The count is read out of the file, so it is bounded by what the file can actually hold
        // before it is used to reserve anything.
        if (static_cast<usize>(count) * sizeof(EmissiveCell) > raw.size() - at.at) return false;
        std::vector<EmissiveCell> mine(count);
        for (u32 c = 0; c < count; ++c) mine[c] = at.pod<EmissiveCell>();
        if (!at.ok) return false;
        seen_cells += count;
        decoded.emplace_back(coord, std::move(mine));
    }
    if (seen_cells != cells) return false;

    // What is already known was read from the world in front of us and is never overwritten by a
    // memory of one.
    for (auto& [coord, mine] : decoded) {
        if (store.known(coord)) continue;
        store.remember(coord, std::move(mine));
    }
    return true;
}

}  // namespace ws
