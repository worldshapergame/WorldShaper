#include "world/world.hpp"

#include <algorithm>

namespace ws {

VoxelTypeId World::get(i64 x, i64 y, i64 z) const {
    const auto found = chunks_.find(chunk_coord_of(x, y, z));
    if (found == chunks_.end()) return kAir;
    return found->second.get(local_of(x), local_of(y), local_of(z));
}

bool World::set(i64 x, i64 y, i64 z, VoxelTypeId type) {
    const ChunkCoord coord = chunk_coord_of(x, y, z);

    if (type == kAir) {
        const auto found = chunks_.find(coord);
        if (found == chunks_.end()) return false;
        return found->second.set(local_of(x), local_of(y), local_of(z), kAir);
    }

    return chunk_for_write(coord).set(local_of(x), local_of(y), local_of(z), type);
}

const Chunk* World::chunk(const ChunkCoord& coord) const {
    const auto found = chunks_.find(coord);
    return (found == chunks_.end()) ? nullptr : &found->second;
}

Chunk& World::chunk_for_write(const ChunkCoord& coord) { return chunks_[coord]; }

bool World::has_chunk(const ChunkCoord& coord) const {
    return chunks_.find(coord) != chunks_.end();
}

bool World::drop_chunk_if_empty(const ChunkCoord& coord) {
    const auto found = chunks_.find(coord);
    if (found == chunks_.end() || !found->second.empty()) return false;
    chunks_.erase(found);
    return true;
}

void World::compact() {
    for (auto it = chunks_.begin(); it != chunks_.end();) {
        it->second.compact();
        it = it->second.empty() ? chunks_.erase(it) : std::next(it);
    }
}

u64 World::chunk_hash(const ChunkCoord& coord) const {
    const Chunk* c = chunk(coord);
    return (c == nullptr) ? 0 : c->content_hash();
}

u64 World::content_hash() const {
    // Order-independent: the map iterates in whatever order it likes, and two peers with
    // the same world must agree. Summing the per-chunk hashes (each already keyed by its
    // coordinate) gives that for free.
    u64 total = 0;
    for (const auto& [coord, chunk] : chunks_) {
        if (chunk.empty()) continue;
        const u64 keyed = hash_combine(hash_cell(coord.x, coord.y, coord.z, 0, 0x57484C44ull),
                                       chunk.content_hash());
        total += keyed;
    }
    return total;
}

WorldStats World::stats() const {
    WorldStats out;
    for (const auto& [coord, chunk] : chunks_) {
        (void)coord;
        out.chunks += 1;
        out.bricks += chunk.brick_count();
        out.solid_voxels += chunk.solid_voxels();
        out.bytes += chunk.bytes();
    }
    return out;
}

bool World::validate() const {
    for (const auto& [coord, chunk] : chunks_) {
        (void)coord;
        if (!chunk.validate()) return false;
    }
    return true;
}

std::vector<ChunkCoord> World::sorted_chunk_coords() const {
    std::vector<ChunkCoord> out;
    out.reserve(chunks_.size());
    for (const auto& [coord, chunk] : chunks_) {
        (void)chunk;
        out.push_back(coord);
    }
    std::sort(out.begin(), out.end(), [](const ChunkCoord& a, const ChunkCoord& b) {
        if (a.z != b.z) return a.z < b.z;
        if (a.y != b.y) return a.y < b.y;
        return a.x < b.x;
    });
    return out;
}

bool any_matter_in(const World& world, const i64 low[3], const i64 high[3]) {
    for (u32 axis = 0; axis < 3; ++axis) {
        if (low[axis] > high[axis]) return false;
    }
    // Walked chunk by chunk rather than brick by brick over the whole box, because a chunk that
    // does not exist answers for 32,768 bricks in one lookup -- which is the ordinary case for a
    // node in the air above a building.
    const ChunkCoord first = chunk_coord_of(low[0], low[1], low[2]);
    const ChunkCoord last = chunk_coord_of(high[0], high[1], high[2]);
    for (i64 cz = first.z; cz <= last.z; ++cz) {
        for (i64 cy = first.y; cy <= last.y; ++cy) {
            for (i64 cx = first.x; cx <= last.x; ++cx) {
                const Chunk* chunk = world.chunk(ChunkCoord{cx, cy, cz});
                if (chunk == nullptr) continue;
                // The bricks this chunk holds that the box actually reaches.
                const i64 base[3] = {cx * kChunkEdge, cy * kChunkEdge, cz * kChunkEdge};
                u32 from[3];
                u32 to[3];
                for (u32 axis = 0; axis < 3; ++axis) {
                    const i64 lo = std::max(low[axis], base[axis]) - base[axis];
                    const i64 hi = std::min(high[axis], base[axis] + kChunkEdge - 1) - base[axis];
                    if (lo > hi) return false;   // cannot happen; the loops bound it
                    from[axis] = static_cast<u32>(lo / kBrickEdge);
                    to[axis] = static_cast<u32>(hi / kBrickEdge);
                }
                for (u32 bz = from[2]; bz <= to[2]; ++bz) {
                    for (u32 by = from[1]; by <= to[1]; ++by) {
                        for (u32 bx = from[0]; bx <= to[0]; ++bx) {
                            if (chunk->brick(bx, by, bz) != nullptr) return true;
                        }
                    }
                }
            }
        }
    }
    return false;
}

}  // namespace ws
