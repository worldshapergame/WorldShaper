#pragma once
// Turning a grid of voxel values into as few boxes as possible.
//
// Shared because two things need exactly this: undo, which has to describe a region it is
// about to destroy, and the clipboard, which has to describe a clip it is about to stamp.
// Both answers are lists of box-fill ops, and both want the list short.
//
// The shape it grows is the standard greedy one — along x, then whole rows along y, then
// whole plates along z. Not provably minimal, but linear, allocation-free and close enough
// that the difference never showed up in a measurement.

#include <vector>

#include "core/types.hpp"
#include "world/voxel_type.hpp"

namespace ws {

// `claimed` is both an input and scratch: cells already marked are skipped and never grown
// into, which is how cells with no single value — or no value at all — are excluded without
// reserving a sentinel type id. It must be the same length as `cells`.
//
// emit(value, x0, y0, z0, x1, y1, z1) is called once per box, with inclusive corners in
// grid coordinates.
template <typename Emit>
void greedy_boxes(const VoxelTypeId* cells, i32 nx, i32 ny, i32 nz, std::vector<u8>& claimed,
                  Emit&& emit) {
    const auto at = [&](i32 x, i32 y, i32 z) {
        return static_cast<usize>(x) + static_cast<usize>(y) * nx +
               static_cast<usize>(z) * nx * ny;
    };

    for (i32 z = 0; z < nz; ++z) {
        for (i32 y = 0; y < ny; ++y) {
            for (i32 x = 0; x < nx; ++x) {
                const usize start = at(x, y, z);
                if (claimed[start] != 0) continue;
                const VoxelTypeId value = cells[start];

                i32 ex = x;
                while (ex + 1 < nx && claimed[at(ex + 1, y, z)] == 0 &&
                       cells[at(ex + 1, y, z)] == value) {
                    ++ex;
                }

                i32 ey = y;
                while (ey + 1 < ny) {
                    bool row_matches = true;
                    for (i32 i = x; i <= ex && row_matches; ++i) {
                        row_matches =
                            claimed[at(i, ey + 1, z)] == 0 && cells[at(i, ey + 1, z)] == value;
                    }
                    if (!row_matches) break;
                    ++ey;
                }

                i32 ez = z;
                while (ez + 1 < nz) {
                    bool plate_matches = true;
                    for (i32 j = y; j <= ey && plate_matches; ++j) {
                        for (i32 i = x; i <= ex && plate_matches; ++i) {
                            plate_matches = claimed[at(i, j, ez + 1)] == 0 &&
                                            cells[at(i, j, ez + 1)] == value;
                        }
                    }
                    if (!plate_matches) break;
                    ++ez;
                }

                for (i32 k = z; k <= ez; ++k) {
                    for (i32 j = y; j <= ey; ++j) {
                        for (i32 i = x; i <= ex; ++i) claimed[at(i, j, k)] = u8{1};
                    }
                }
                emit(value, x, y, z, ex, ey, ez);
            }
        }
    }
}

}  // namespace ws
