#include "world/region.hpp"

#include <algorithm>

#include "world/greedy.hpp"
#include "world/world.hpp"

namespace ws {
namespace {

MatterReason reason_for(VoxelTypeId type) {
    return (type == kAir) ? MatterReason::PlayerBreak : MatterReason::PlayerPlace;
}

}  // namespace

u64 decompose_region(const World& world, i64 x0, i64 y0, i64 z0, i64 x1, i64 y1, i64 z1,
                     u64 tick, u32 player, std::vector<Op>& out) {
    if (x0 > x1) std::swap(x0, x1);
    if (y0 > y1) std::swap(y0, y1);
    if (z0 > z1) std::swap(z0, z1);

    const u64 before = out.size();

    // Level one: the brick grid. A brick the box covers completely and that holds a single
    // type collapses to one cell, so open air and solid rock cost nothing to remember.
    const i64 bx0 = x0 >> 3, bx1 = x1 >> 3;
    const i64 by0 = y0 >> 3, by1 = y1 >> 3;
    const i64 bz0 = z0 >> 3, bz1 = z1 >> 3;
    const i32 nx = static_cast<i32>(bx1 - bx0 + 1);
    const i32 ny = static_cast<i32>(by1 - by0 + 1);
    const i32 nz = static_cast<i32>(bz1 - bz0 + 1);

    const usize cell_count = static_cast<usize>(nx) * ny * nz;
    std::vector<VoxelTypeId> cells(cell_count, kAir);
    // A cell is "mixed" until proven otherwise: the box only clips part of it, or the brick
    // holds more than one type. Mixed cells go to the voxel-level pass instead.
    std::vector<u8> mixed(cell_count, u8{1});

    const Chunk* chunk = nullptr;
    ChunkCoord chunk_coord{};
    bool chunk_valid = false;

    for (i32 k = 0; k < nz; ++k) {
        for (i32 j = 0; j < ny; ++j) {
            for (i32 i = 0; i < nx; ++i) {
                const i64 bx = bx0 + i, by = by0 + j, bz = bz0 + k;
                const i64 base[3] = {bx << 3, by << 3, bz << 3};
                const bool full = base[0] >= x0 && base[0] + 7 <= x1 && base[1] >= y0 &&
                                  base[1] + 7 <= y1 && base[2] >= z0 && base[2] + 7 <= z1;
                if (!full) continue;   // stays mixed, handled at voxel level below

                const ChunkCoord cc = chunk_coord_of(base[0], base[1], base[2]);
                if (!chunk_valid || !(cc == chunk_coord)) {
                    chunk_coord = cc;
                    chunk = world.chunk(cc);
                    chunk_valid = true;
                }
                const Brick* brick =
                    (chunk == nullptr) ? nullptr
                                       : chunk->brick(local_of(base[0]) >> 3,
                                                      local_of(base[1]) >> 3,
                                                      local_of(base[2]) >> 3);
                const usize cell = static_cast<usize>(i) + static_cast<usize>(j) * nx +
                                   static_cast<usize>(k) * nx * ny;
                if (brick == nullptr) {
                    cells[cell] = kAir;
                    mixed[cell] = 0;
                } else if (brick->uniform()) {
                    cells[cell] = brick->uniform_value();
                    mixed[cell] = 0;
                }
            }
        }
    }

    std::vector<u8> claimed = mixed;   // mixed cells start claimed, so they are never grown into
    greedy_boxes(cells.data(), nx, ny, nz, claimed,
                 [&](VoxelTypeId value, i32 sx, i32 sy, i32 sz, i32 ex, i32 ey, i32 ez) {
                     out.push_back(Op::fill_box(tick, player, (bx0 + sx) << 3, (by0 + sy) << 3,
                                                (bz0 + sz) << 3, ((bx0 + ex) << 3) + 7,
                                                ((by0 + ey) << 3) + 7, ((bz0 + ez) << 3) + 7,
                                                value, reason_for(value)));
                 });

    // Level two: every brick the first pass left as mixed, decomposed over its own voxels.
    // Bounded to 512 cells at a time, so this stays cheap however large the edit was.
    VoxelTypeId voxel_cells[kBrickVoxels];
    std::vector<u8> voxel_claimed(kBrickVoxels, u8{0});
    chunk_valid = false;

    for (i32 k = 0; k < nz; ++k) {
        for (i32 j = 0; j < ny; ++j) {
            for (i32 i = 0; i < nx; ++i) {
                if (mixed[static_cast<usize>(i) + static_cast<usize>(j) * nx +
                          static_cast<usize>(k) * nx * ny] == 0) {
                    continue;
                }
                const i64 base[3] = {(bx0 + i) << 3, (by0 + j) << 3, (bz0 + k) << 3};
                const i64 lo[3] = {std::max(x0, base[0]), std::max(y0, base[1]),
                                   std::max(z0, base[2])};
                const i64 hi[3] = {std::min(x1, base[0] + 7), std::min(y1, base[1] + 7),
                                   std::min(z1, base[2] + 7)};

                const ChunkCoord cc = chunk_coord_of(base[0], base[1], base[2]);
                if (!chunk_valid || !(cc == chunk_coord)) {
                    chunk_coord = cc;
                    chunk = world.chunk(cc);
                    chunk_valid = true;
                }
                const Brick* brick =
                    (chunk == nullptr) ? nullptr
                                       : chunk->brick(local_of(base[0]) >> 3,
                                                      local_of(base[1]) >> 3,
                                                      local_of(base[2]) >> 3);

                const i32 sx = static_cast<i32>(hi[0] - lo[0] + 1);
                const i32 sy = static_cast<i32>(hi[1] - lo[1] + 1);
                const i32 sz = static_cast<i32>(hi[2] - lo[2] + 1);
                const usize count = static_cast<usize>(sx) * sy * sz;

                if (brick == nullptr) {
                    std::fill(voxel_cells, voxel_cells + count, kAir);
                } else if (count < kBrickVoxels / 2) {
                    // The box only clips a corner of this brick. Decoding all 512 to read a
                    // handful costs more than the per-voxel path it was meant to replace.
                    for (i32 z = 0; z < sz; ++z) {
                        for (i32 y = 0; y < sy; ++y) {
                            for (i32 x = 0; x < sx; ++x) {
                                voxel_cells[static_cast<usize>(x) + static_cast<usize>(y) * sx +
                                            static_cast<usize>(z) * sx * sy] =
                                    brick->get(static_cast<u32>((lo[0] + x) & 7),
                                               static_cast<u32>((lo[1] + y) & 7),
                                               static_cast<u32>((lo[2] + z) & 7));
                            }
                        }
                    }
                } else {
                    // Decoded once for the whole brick rather than looked up per voxel: the
                    // encoding test and the palette indirection are hoisted out of the loop.
                    VoxelTypeId decoded[kBrickVoxels];
                    brick->decode(decoded);
                    for (i32 z = 0; z < sz; ++z) {
                        for (i32 y = 0; y < sy; ++y) {
                            const usize row = static_cast<usize>(y) * sx +
                                              static_cast<usize>(z) * sx * sy;
                            for (i32 x = 0; x < sx; ++x) {
                                voxel_cells[row + static_cast<usize>(x)] =
                                    decoded[brick_index(static_cast<u32>((lo[0] + x) & 7),
                                                        static_cast<u32>((lo[1] + y) & 7),
                                                        static_cast<u32>((lo[2] + z) & 7))];
                            }
                        }
                    }
                }

                std::fill(voxel_claimed.begin(), voxel_claimed.begin() + count, u8{0});
                greedy_boxes(voxel_cells, sx, sy, sz, voxel_claimed,
                             [&](VoxelTypeId value, i32 ax, i32 ay, i32 az, i32 ex, i32 ey,
                                 i32 ez) {
                                 out.push_back(Op::fill_box(
                                     tick, player, lo[0] + ax, lo[1] + ay, lo[2] + az,
                                     lo[0] + ex, lo[1] + ey, lo[2] + ez, value,
                                     reason_for(value)));
                             });
            }
        }
    }

    return out.size() - before;
}

}  // namespace ws
