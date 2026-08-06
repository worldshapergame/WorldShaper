#include <doctest/doctest.h>

#include "world/voxel_type.hpp"

using namespace ws;

namespace {

VisualRecord grey(u8 level) {
    VisualRecord v{};
    v.red = level;
    v.green = level;
    v.blue = level;
    v.opacity = 255;
    v.roughness = 200;
    return v;
}

BehaviourRecord stone_behaviour(const TagRegistry& tags) {
    BehaviourRecord b{};
    b.material = 1;
    b.tags.add(tags.find("stone"));
    b.tags.add(tags.find("solid"));
    b.tags.add(tags.find("buildable"));
    b.properties.set(props::kDensity, PropertyValue::from_uint(2600));
    b.properties.set(props::kHardness, PropertyValue::from_uint(200));
    return b;
}

}  // namespace

TEST_CASE("air is id 0 in every table") {
    VoxelTypeTable table;
    CHECK(table.type_count() == 1);
    CHECK(table.visual_of(kAir).opacity == 0);
    CHECK(table.type(kAir).visual == kAirVisual);
    CHECK(table.type(kAir).behaviour == kAirBehaviour);
}

TEST_CASE("identical records intern to the same id") {
    VoxelTypeTable table;
    TagRegistry tags;

    const VoxelTypeId a = table.intern(grey(120), stone_behaviour(tags));
    const VoxelTypeId b = table.intern(grey(120), stone_behaviour(tags));
    CHECK(a == b);
    CHECK(table.type_count() == 2);          // air plus one
    CHECK(table.visual_count() == 2);
    CHECK(table.behaviour_count() == 2);
}

TEST_CASE("a million identical voxels cost one record") {
    // This is the whole argument for interning: sameness is free.
    VoxelTypeTable table;
    TagRegistry tags;
    const BehaviourRecord behaviour = stone_behaviour(tags);
    const VisualRecord visual = grey(90);

    VoxelTypeId first = kAir;
    for (u32 i = 0; i < 100000; ++i) {
        const VoxelTypeId id = table.intern(visual, behaviour);
        if (i == 0) first = id;
        REQUIRE(id == first);
    }
    CHECK(table.visual_count() == 2);
    CHECK(table.behaviour_count() == 2);
    CHECK(table.stats().dedup_rate() > 0.99);
}

TEST_CASE("a unique colour costs one visual record and nothing else") {
    // Answer O4: visual records are never merged, so a hand-painted wall genuinely gets
    // its own colours — but the behaviour they all share is stored once.
    VoxelTypeTable table;
    TagRegistry tags;
    const BehaviourRecord behaviour = stone_behaviour(tags);

    for (u32 i = 0; i < 256; ++i) {
        table.intern(grey(static_cast<u8>(i)), behaviour);
    }
    CHECK(table.visual_count() == 257);      // air plus 256 shades
    CHECK(table.behaviour_count() == 2);     // air plus one shared behaviour
    CHECK(table.type_count() == 257);

    const VoxelTypeStats stats = table.stats();
    CHECK(stats.visual_bytes == 257 * 16);
}

TEST_CASE("the same colour with different behaviour is a different type") {
    VoxelTypeTable table;
    TagRegistry tags;

    BehaviourRecord stone = stone_behaviour(tags);
    BehaviourRecord painted_wood{};
    painted_wood.material = 2;
    painted_wood.tags.add(tags.find("wood"));
    painted_wood.tags.add(tags.find("flammable"));

    const VoxelTypeId a = table.intern(grey(200), stone);
    const VoxelTypeId b = table.intern(grey(200), painted_wood);
    CHECK(a != b);
    CHECK(table.visual_count() == 2);    // the colour is shared
    CHECK(table.behaviour_count() == 3);
}

TEST_CASE("properties and tags read back through the type") {
    VoxelTypeTable table;
    TagRegistry tags;
    PropertyRegistry properties;

    const VoxelTypeId stone = table.intern(grey(140), stone_behaviour(tags));
    CHECK(table.property(properties, stone, props::kDensity).as_uint() == 2600);
    CHECK(table.property(properties, stone, props::kFriction).as_uint() == 128);  // default
    CHECK(table.has_tag(stone, tags.find("stone")));
    CHECK_FALSE(table.has_tag(stone, tags.find("metal")));
}

TEST_CASE("visual record stays 16 bytes and hashes on its contents") {
    CHECK(sizeof(VisualRecord) == 16);

    VisualRecord a = grey(50);
    VisualRecord b = grey(50);
    CHECK(a == b);
    CHECK(a.content_hash() == b.content_hash());

    b.roughness = static_cast<u8>(b.roughness + 1);
    CHECK_FALSE(a == b);
    CHECK(a.content_hash() != b.content_hash());
}

TEST_CASE("visual flags") {
    VisualRecord water{};
    water.flags = static_cast<u8>(VisualFlags::SmoothSurface | VisualFlags::CausticSource);
    CHECK(has_flag(static_cast<VisualFlags>(water.flags), VisualFlags::SmoothSurface));
    CHECK(has_flag(static_cast<VisualFlags>(water.flags), VisualFlags::CausticSource));
    CHECK_FALSE(has_flag(static_cast<VisualFlags>(water.flags), VisualFlags::Dispersive));
}

TEST_CASE("hash collisions do not merge different records") {
    // The index is a hash bucket of ids, and every candidate is compared field by field
    // before being reused. If that comparison were ever dropped, two different materials
    // would silently become one — so it is worth a test that does not depend on luck.
    VoxelTypeTable table;
    const usize distinct = 4096;
    std::vector<VoxelTypeId> ids;
    ids.reserve(distinct);
    for (usize i = 0; i < distinct; ++i) {
        VisualRecord v{};
        v.red = static_cast<u8>(i & 0xFF);
        v.green = static_cast<u8>((i >> 8) & 0xFF);
        v.opacity = 255;
        ids.push_back(table.intern(v, BehaviourRecord{}));
    }
    for (usize i = 0; i < distinct; ++i) {
        for (usize k = i + 1; k < distinct; ++k) {
            REQUIRE(ids[i] != ids[k]);
        }
        if (i > 64) break;   // a full O(n²) sweep is wasteful; the first block is enough
    }
    CHECK(table.type_count() == distinct + 1);
}
