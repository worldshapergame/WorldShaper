#pragma once
// Thumbnails: what a chunk looks like once it is too far away to resolve a voxel.
//
// The problem they exist to solve is that a chunk currently has to be *fully* resident to
// draw at all — headers, occupancy and payload for up to 32,768 bricks. So however well
// streaming behaves, render distance is bounded by memory, and past that bound the world
// is not drawn far away, it is not drawn at all. You get sky where the world is.
//
// documentation/03-voxel-data-model.md has always called for a sparse octree above chunk
// level "for distant rendering". Stage 4 built its *occupancy* — enough to skip empty space
// in big jumps — and never its colour, which is what actually draws something.
//
// A thumbnail is the cheap half of a chunk: 8x8x8 cells, one per cubic metre, each holding
// a filtered colour and how full it is. Two kilobytes against a chunk's megabytes, which is
// what makes it affordable to hold thousands of them and to draw a world that does not end.
//
// Why one metre. A cell is 32 voxels, which falls below one pixel at about 515 m — and the
// full-detail streaming window is 256 m, so the two overlap rather than leaving a band where
// neither is good enough. Coarser cells would show as visible blocks just outside the window;
// finer ones cost more memory to describe detail no pixel can resolve.

#include "core/types.hpp"

namespace ws {

class Chunk;
class VoxelTypeTable;

inline constexpr u32 kThumbAxis = 8;                                   // cells per axis
inline constexpr u32 kThumbCells = kThumbAxis * kThumbAxis * kThumbAxis;   // 512
inline constexpr u32 kThumbCellVoxels = kChunkEdge / kThumbAxis;       // 32 voxels = 1 m
inline constexpr u32 kThumbCellBricks = kThumbCellVoxels / kBrickEdge;  // 4 bricks per axis

static_assert(kThumbCellVoxels == 32, "a thumbnail cell is one cubic metre");
static_assert(kThumbCells * sizeof(u32) == 2048, "a thumbnail is two kilobytes");

// Cell order, x fastest — the same order everything else in the engine uses, so a thumbnail
// can be walked by a shader without a translation table.
constexpr u32 thumb_index(u32 x, u32 y, u32 z) {
    return x | (y << 3) | (z << 6);
}

struct Thumbnail {
    // rgba8 per cell. Alpha is coverage: what fraction of the cell is matter. Zero means
    // nothing at all is here, and that is the only meaning zero has — see build_thumbnail.
    u32 cells[kThumbCells]{};

    bool empty() const;
    u32 at(u32 x, u32 y, u32 z) const { return cells[thumb_index(x, y, z)]; }
};

// Builds a chunk's thumbnail by filtering its bricks. Sixty-four bricks per cell, and the
// bricks already know their own filtered colour, so this never touches a voxel.
Thumbnail build_thumbnail(const Chunk& chunk, const VoxelTypeTable& types);

}  // namespace ws
