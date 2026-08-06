#include <doctest/doctest.h>

#include "world/chunk.hpp"
#include "world/thumbnail.hpp"
#include "world/voxel_type.hpp"

using namespace ws;

namespace {

// Two loud, unmistakable colours, so an average can be reasoned about by hand.
struct Palette {
    VoxelTypeTable table;
    VoxelTypeId red = kAir;
    VoxelTypeId blue = kAir;
};

Palette make_types() {
    Palette palette;
    VisualRecord red{};
    red.red = 200;
    VisualRecord blue{};
    blue.blue = 200;
    palette.red = palette.table.intern(red, BehaviourRecord{});
    palette.blue = palette.table.intern(blue, BehaviourRecord{});
    return palette;
}

}  // namespace

TEST_CASE("an empty chunk has an empty thumbnail") {
    const Palette palette = make_types();
    Chunk chunk;
    const Thumbnail thumb = build_thumbnail(chunk, palette.table);
    CHECK(thumb.empty());
}

TEST_CASE("a solid chunk thumbnails to full coverage in its own colour") {
    const Palette palette = make_types();
    const VoxelTypeId red = palette.red;

    Chunk chunk;
    for (u32 z = 0; z < static_cast<u32>(kChunkEdge); ++z) {
        for (u32 y = 0; y < static_cast<u32>(kChunkEdge); ++y) {
            for (u32 x = 0; x < static_cast<u32>(kChunkEdge); ++x) chunk.set(x, y, z, red);
        }
    }

    const Thumbnail thumb = build_thumbnail(chunk, palette.table);
    REQUIRE_FALSE(thumb.empty());
    for (u32 cell : thumb.cells) {
        CHECK((cell & 0xFFu) == 200u);           // red
        CHECK(((cell >> 8) & 0xFFu) == 0u);
        CHECK(((cell >> 16) & 0xFFu) == 0u);
        CHECK((cell >> 24) == 255u);             // completely full
    }
}

TEST_CASE("a thumbnail cell covers exactly one cubic metre, in the right place") {
    // Cells are addressed the same way as everything else, x fastest. Getting this wrong
    // mirrors the world along an axis, which is the kind of thing that looks almost right.
    const Palette palette = make_types();
    const VoxelTypeId red = palette.red;

    Chunk chunk;
    // One solid metre at cell (5, 1, 2), and nothing else.
    const u32 base[3] = {5 * kThumbCellVoxels, 1 * kThumbCellVoxels, 2 * kThumbCellVoxels};
    for (u32 z = 0; z < kThumbCellVoxels; ++z) {
        for (u32 y = 0; y < kThumbCellVoxels; ++y) {
            for (u32 x = 0; x < kThumbCellVoxels; ++x) {
                chunk.set(base[0] + x, base[1] + y, base[2] + z, red);
            }
        }
    }

    const Thumbnail thumb = build_thumbnail(chunk, palette.table);
    CHECK((thumb.at(5, 1, 2) >> 24) == 255u);
    CHECK(thumb.at(1, 5, 2) == 0u);   // not transposed
    CHECK(thumb.at(2, 1, 5) == 0u);

    u32 occupied = 0;
    for (u32 cell : thumb.cells) {
        if (cell != 0) ++occupied;
    }
    CHECK(occupied == 1);
}

TEST_CASE("a single voxel survives being thumbnailed") {
    // The one that matters. A cubic metre holds 32,768 voxels, so one voxel is a coverage of
    // 0.003% — which rounds to zero, and zero means "empty". A railing, a wire or a window
    // frame would not merely look thin at a distance, it would not be drawn at all, and it
    // would vanish at exactly the range where you can no longer see well enough to notice.
    const Palette palette = make_types();
    const VoxelTypeId red = palette.red;

    Chunk chunk;
    chunk.set(40, 8, 70, red);

    const Thumbnail thumb = build_thumbnail(chunk, palette.table);
    REQUIRE_FALSE(thumb.empty());
    const u32 cell = thumb.at(40 / kThumbCellVoxels, 8 / kThumbCellVoxels, 70 / kThumbCellVoxels);
    CHECK(cell != 0);
    CHECK((cell >> 24) >= 1u);          // present, however faintly
    CHECK((cell & 0xFFu) == 200u);      // and it kept its colour
}

TEST_CASE("a half-and-half cell averages both materials") {
    const Palette palette = make_types();
    const VoxelTypeId red = palette.red;
    const VoxelTypeId blue = palette.blue;

    Chunk chunk;
    for (u32 z = 0; z < kThumbCellVoxels; ++z) {
        for (u32 y = 0; y < kThumbCellVoxels; ++y) {
            for (u32 x = 0; x < kThumbCellVoxels; ++x) {
                chunk.set(x, y, z, (x < kThumbCellVoxels / 2) ? red : blue);
            }
        }
    }

    const u32 cell = build_thumbnail(chunk, palette.table).at(0, 0, 0);
    CHECK((cell >> 24) == 255u);
    // Both contribute, neither wins outright.
    CHECK((cell & 0xFFu) > 60u);
    CHECK((cell & 0xFFu) < 140u);
    CHECK(((cell >> 16) & 0xFFu) > 60u);
    CHECK(((cell >> 16) & 0xFFu) < 140u);
}

TEST_CASE("coverage reports how full a cell actually is") {
    const Palette palette = make_types();
    const VoxelTypeId red = palette.red;

    Chunk chunk;
    // Half the metre solid: 16 of its 32 voxel layers.
    for (u32 z = 0; z < kThumbCellVoxels; ++z) {
        for (u32 y = 0; y < kThumbCellVoxels / 2; ++y) {
            for (u32 x = 0; x < kThumbCellVoxels; ++x) chunk.set(x, y, z, red);
        }
    }

    const u32 alpha = build_thumbnail(chunk, palette.table).at(0, 0, 0) >> 24;
    CHECK(alpha > 100u);
    CHECK(alpha < 155u);
}
