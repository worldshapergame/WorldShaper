#include "world/test_scene.hpp"

#include "core/hash.hpp"
#include "world/op.hpp"
#include "world/tags.hpp"
#include "world/voxel_type.hpp"
#include "world/world.hpp"

namespace ws {
namespace {

VisualRecord make_visual(u8 r, u8 g, u8 b, u8 roughness, u8 opacity = 255) {
    VisualRecord v{};
    v.red = r;
    v.green = g;
    v.blue = b;
    v.opacity = opacity;
    v.roughness = roughness;
    return v;
}

BehaviourRecord make_behaviour(const TagRegistry& tags, u32 material, const char* tag,
                               u32 density, u32 hardness) {
    BehaviourRecord b{};
    b.material = material;
    b.tags.add(tags.find("solid"));
    b.tags.add(tags.find("buildable"));
    const TagId named = tags.find(tag);
    if (named != kInvalidTag) b.tags.add(named);
    b.properties.set(props::kDensity, PropertyValue::from_uint(density));
    b.properties.set(props::kHardness, PropertyValue::from_uint(hardness));
    return b;
}

void box(World& world, MatterLedger& ledger, u64 tick, i64 x0, i64 y0, i64 z0, i64 x1,
         i64 y1, i64 z1, VoxelTypeId type) {
    apply_op(world,
             Op::fill_box(tick, 0, x0, y0, z0, x1, y1, z1, type,
                          type == kAir ? MatterReason::Generation : MatterReason::Generation),
             ledger);
}

}  // namespace

TestScenePalette create_test_palette(VoxelTypeTable& types, const TagRegistry& tags) {
    TestScenePalette palette;

    const BehaviourRecord stone = make_behaviour(tags, 1, "stone", 2600, 200);
    const BehaviourRecord wood = make_behaviour(tags, 2, "wood", 700, 90);
    const BehaviourRecord metal = make_behaviour(tags, 3, "metal", 7800, 240);
    const BehaviourRecord glass = make_behaviour(tags, 4, "glass", 2500, 60);

    palette.stone = types.intern(make_visual(122, 120, 116, 210), stone);
    palette.stone_dark = types.intern(make_visual(96, 94, 92, 220), stone);
    palette.stone_light = types.intern(make_visual(158, 154, 146, 200), stone);
    palette.wood = types.intern(make_visual(120, 82, 44, 180), wood);
    palette.metal = types.intern(make_visual(168, 170, 176, 60), metal);

    VisualRecord glass_visual = make_visual(190, 210, 215, 10, 40);
    glass_visual.ior = 64;          // about 1.5
    glass_visual.translucency = 220;
    glass_visual.flags = static_cast<u8>(VisualFlags::CausticSource);
    palette.glass = types.intern(glass_visual, glass);

    VisualRecord lamp_visual = make_visual(255, 240, 200, 128);
    lamp_visual.emissive = 200;
    palette.lamp = types.intern(lamp_visual, metal);

    return palette;
}

void build_test_scene(World& world, const TestScenePalette& palette, i64 radius,
                      MatterLedger& ledger) {
    u64 tick = 0;

    // --- ground slab, with a stone-shade pattern so bricks hold several types ----------
    for (i64 z = -radius; z <= radius; z += 8) {
        for (i64 x = -radius; x <= radius; x += 8) {
            const u64 h = hash_cell(x, 0, z, 0, 0x5343454Eull);
            const VoxelTypeId shade = (hash_range(h, 3) == 0)   ? palette.stone_dark
                                      : (hash_range(h, 5) == 0) ? palette.stone_light
                                                                : palette.stone;
            box(world, ledger, tick++, x, -12, z, x + 7, -1, z + 7, shade);
        }
    }

    // --- a stepped pit: enclosed volume, concave, and a depth gradient ----------------
    for (i64 step = 0; step < 6; ++step) {
        const i64 inset = step * 6;
        box(world, ledger, tick++, -60 + inset, -12 + step * 2, -60 + inset, 60 - inset,
            -11 + step * 2, 60 - inset, kAir);
    }

    // --- four towers with hollow interiors and window slots ---------------------------
    const i64 corners[4][2] = {{-180, -180}, {180, -180}, {-180, 180}, {180, 180}};
    for (const auto& corner : corners) {
        const i64 cx = corner[0];
        const i64 cz = corner[1];
        box(world, ledger, tick++, cx - 20, 0, cz - 20, cx + 20, 120, cz + 20, palette.stone);
        box(world, ledger, tick++, cx - 16, 0, cz - 16, cx + 16, 116, cz + 16, kAir);

        for (i64 y = 16; y < 112; y += 24) {
            box(world, ledger, tick++, cx - 21, y, cz - 6, cx + 21, y + 10, cz + 6, kAir);
            box(world, ledger, tick++, cx - 6, y, cz - 21, cx + 6, y + 10, cz + 21, kAir);
            box(world, ledger, tick++, cx - 5, y + 1, cz - 21, cx + 5, y + 9, cz - 20,
                palette.glass);
        }
        box(world, ledger, tick++, cx - 3, 100, cz - 3, cx + 3, 103, cz + 3, palette.lamp);
    }

    // --- an arch: overhang, and geometry with nothing beneath it ----------------------
    box(world, ledger, tick++, -140, 0, -8, -120, 90, 8, palette.stone);
    box(world, ledger, tick++, 120, 0, -8, 140, 90, 8, palette.stone);
    box(world, ledger, tick++, -140, 90, -8, 140, 104, 8, palette.stone);
    for (i64 i = 0; i < 12; ++i) {
        const i64 inset = i * 10;
        box(world, ledger, tick++, -130 + inset, 90 - i, -8, 130 - inset, 90 - i, 8, kAir);
    }

    // --- a tunnel through the ground: interior surfaces seen from inside --------------
    box(world, ledger, tick++, -radius, -10, -6, radius, -2, 6, kAir);
    for (i64 x = -radius + 40; x < radius; x += 120) {
        box(world, ledger, tick++, x, -10, -6, x + 8, 0, 6, kAir);   // shafts to the surface
        box(world, ledger, tick++, x + 2, -4, -2, x + 5, -3, 1, palette.lamp);
    }

    // --- a lattice: thin geometry, the hardest case for both marching and meshing ------
    for (i64 x = -40; x <= 40; x += 8) {
        box(world, ledger, tick++, x, 0, -40, x + 1, 60, 40, palette.metal);
    }
    for (i64 y = 0; y <= 60; y += 8) {
        box(world, ledger, tick++, -40, y, -40, 40, y + 1, 40, palette.metal);
    }
    for (i64 z = -40; z <= 40; z += 8) {
        box(world, ledger, tick++, -40, 0, z, 40, 60, z + 1, palette.metal);
    }

    // --- scattered wooden crates: many small separate objects -------------------------
    for (i64 i = 0; i < 60; ++i) {
        const u64 h = hash_cell(i, 1, 2, 0, 0x43524154ull);
        const i64 x = static_cast<i64>(hash_range(h, static_cast<u32>(radius))) - radius / 2;
        const i64 z = static_cast<i64>(hash_range(hash_mix(h), static_cast<u32>(radius))) -
                      radius / 2;
        const i64 size = 4 + static_cast<i64>(hash_range(hash_mix(h + 1), 8));
        box(world, ledger, tick++, x, 0, z, x + size, size, z + size, palette.wood);
    }
}

}  // namespace ws
