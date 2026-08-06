#pragma once
// The world: a sparse map of 8-metre chunks keyed by 64-bit coordinates.
//
// documentation/03-voxel-data-model.md §1 — coordinates are signed 64-bit integers in
// voxel units, giving ±2.8×10¹¹ metres of world. Chunks exist only where something is,
// so an empty world costs nothing and a world nobody has visited costs nothing either
// (unexplored terrain is a seed, not data — answer F7).
//
// Above the chunk level there is a second, upward octree for distant rendering. It is
// built in Stage 4 alongside continuous detail, because it is the renderer's structure
// and nothing before Stage 4 reads it.

#include <unordered_map>
#include <vector>

#include "core/hash.hpp"
#include "core/types.hpp"
#include "world/chunk.hpp"

namespace ws {

struct ChunkCoord {
    i64 x = 0;
    i64 y = 0;
    i64 z = 0;
    bool operator==(const ChunkCoord&) const = default;
};

struct ChunkCoordHash {
    usize operator()(const ChunkCoord& c) const noexcept {
        return static_cast<usize>(hash_cell(c.x, c.y, c.z, 0, 0x43554E4Bull));
    }
};

// Arithmetic shift, not division: a voxel at -1 belongs to chunk -1, not chunk 0.
// Getting this wrong puts every negative coordinate in the world one chunk out.
constexpr i64 chunk_of(i64 voxel) { return voxel >> 8; }
constexpr u32 local_of(i64 voxel) { return static_cast<u32>(voxel & 255); }

constexpr ChunkCoord chunk_coord_of(i64 x, i64 y, i64 z) {
    return ChunkCoord{chunk_of(x), chunk_of(y), chunk_of(z)};
}

struct WorldStats {
    u64 chunks = 0;
    u64 bricks = 0;
    u64 solid_voxels = 0;
    u64 bytes = 0;
    // Cost per voxel of allocated space. This is the number documentation/03 §3 and the
    // memory budget are written against, because it is what scales with world size.
    f64 bytes_per_voxel() const {
        const u64 voxels = bricks * static_cast<u64>(kBrickVoxels);
        return (voxels > 0) ? static_cast<f64>(bytes) / static_cast<f64>(voxels) : 0.0;
    }

    // Cost per voxel that is actually matter. Higher, and it varies with how much of the
    // world is air, so it is reported but never budgeted against.
    f64 bytes_per_solid_voxel() const {
        return (solid_voxels > 0) ? static_cast<f64>(bytes) / static_cast<f64>(solid_voxels)
                                  : 0.0;
    }
};

class World {
public:
    VoxelTypeId get(i64 x, i64 y, i64 z) const;

    // Returns true when the voxel actually changed. Writing air where nothing exists
    // allocates nothing.
    bool set(i64 x, i64 y, i64 z, VoxelTypeId type);

    const Chunk* chunk(const ChunkCoord& coord) const;
    Chunk& chunk_for_write(const ChunkCoord& coord);

    bool has_chunk(const ChunkCoord& coord) const;
    u64 chunk_count() const { return chunks_.size(); }

    // Frees empty bricks and then any chunk left with nothing in it.
    void compact();

    u64 chunk_hash(const ChunkCoord& coord) const;   // 0 for a chunk that does not exist
    u64 content_hash() const;
    WorldStats stats() const;
    bool validate() const;

    // Iteration for the streamer, the saver and the tests. Order is unspecified, so
    // anything order-sensitive must sort first.
    template <typename Fn>
    void for_each_chunk(Fn&& fn) const {
        for (const auto& [coord, chunk] : chunks_) fn(coord, chunk);
    }

    std::vector<ChunkCoord> sorted_chunk_coords() const;

private:
    std::unordered_map<ChunkCoord, Chunk, ChunkCoordHash> chunks_;
};

}  // namespace ws
