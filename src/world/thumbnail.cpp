#include "world/thumbnail.hpp"

#include <algorithm>

#include "world/chunk.hpp"
#include "world/gpu_brick.hpp"

namespace ws {

bool Thumbnail::empty() const {
    for (u32 cell : cells) {
        if (cell != 0) return false;
    }
    return true;
}

Thumbnail build_thumbnail(const Chunk& chunk, const VoxelTypeTable& types) {
    Thumbnail thumb;
    constexpr u32 kBricksPerCell = kThumbCellBricks * kThumbCellBricks * kThumbCellBricks;

    for (u32 cz = 0; cz < kThumbAxis; ++cz) {
        for (u32 cy = 0; cy < kThumbAxis; ++cy) {
            for (u32 cx = 0; cx < kThumbAxis; ++cx) {
                u32 red = 0;
                u32 green = 0;
                u32 blue = 0;
                u32 weight = 0;
                u32 coverage = 0;
                u32 occupied_bricks = 0;

                for (u32 bz = 0; bz < kThumbCellBricks; ++bz) {
                    for (u32 by = 0; by < kThumbCellBricks; ++by) {
                        for (u32 bx = 0; bx < kThumbCellBricks; ++bx) {
                            const Brick* brick =
                                chunk.brick(cx * kThumbCellBricks + bx, cy * kThumbCellBricks + by,
                                            cz * kThumbCellBricks + bz);
                            if (brick == nullptr) continue;

                            const u32 filtered = filtered_colour(*brick, types);
                            if (filtered == 0) continue;   // allocated but entirely air

                            const u32 alpha = filtered >> 24;
                            coverage += alpha;
                            ++occupied_bricks;

                            // Colour is weighted by how full each brick is, so a brick that
                            // is a tenth matter does not colour the metre as strongly as one
                            // that is solid. The +1 keeps a barely-covered brick counting for
                            // something rather than being silently ignored.
                            const u32 w = alpha + 1;
                            red += (filtered & 0xFFu) * w;
                            green += ((filtered >> 8) & 0xFFu) * w;
                            blue += ((filtered >> 16) & 0xFFu) * w;
                            weight += w;
                        }
                    }
                }

                if (weight == 0) continue;   // stays zero: nothing here

                // Coverage is the mean over all sixty-four brick positions, empty ones
                // included — a metre with one brick of stone in it is one sixty-fourth full,
                // not solid.
                u32 alpha = coverage / kBricksPerCell;

                // But it is never *nothing*. A single railing or wire rounds to zero here,
                // and zero is how "empty" is spelled, so it would not merely look thin at a
                // distance — it would vanish. Thin structures disappearing at exactly the
                // range where you can no longer check is the failure this guards.
                if (alpha == 0 && occupied_bricks > 0) alpha = 1;

                thumb.cells[thumb_index(cx, cy, cz)] =
                    (red / weight) | ((green / weight) << 8) | ((blue / weight) << 16) |
                    (alpha << 24);
            }
        }
    }

    return thumb;
}

}  // namespace ws
