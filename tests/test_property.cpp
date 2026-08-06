#include <doctest/doctest.h>

#include "world/property.hpp"

using namespace ws;

TEST_CASE("built-in properties have the ids the engine hard-codes") {
    PropertyRegistry registry;
    CHECK(registry.find("density") == props::kDensity);
    CHECK(registry.find("friction") == props::kFriction);
    CHECK(registry.find("hardness") == props::kHardness);
    CHECK(registry.find("phase") == props::kPhase);
    CHECK(registry.find("gravity_warp") == props::kGravityWarp);
    CHECK(registry.find("not_a_property") == kInvalidProperty);
}

TEST_CASE("a mod can add properties without touching engine code") {
    PropertyRegistry registry;
    const u32 before = registry.count();
    const PropertyId mana = registry.define("mana_conductivity", PropertyType::Uint,
                                            PropertyDomain::Simulation,
                                            PropertyValue::from_uint(0));
    REQUIRE(mana != kInvalidProperty);
    CHECK(registry.count() == before + 1);
    CHECK(registry.find("mana_conductivity") == mana);
    // Registering the same thing again returns the same id rather than a duplicate.
    CHECK(registry.define("mana_conductivity", PropertyType::Uint, PropertyDomain::Simulation,
                          PropertyValue::from_uint(0)) == mana);
}

TEST_CASE("a float simulation property is rejected, which is what protects multiplayer") {
    // documentation/05-simulation.md §12 rule 1: no floating point may touch shared state,
    // because two GPUs will not agree on it bit for bit.
    PropertyRegistry registry;
    const PropertyId rejected = registry.define("bounciness", PropertyType::Float,
                                                PropertyDomain::Simulation,
                                                PropertyValue::from_float(0.5f));
    CHECK(rejected == kInvalidProperty);
    CHECK(registry.find("bounciness") == kInvalidProperty);

    // The same property is fine in the visual domain, where nothing downstream is shared.
    const PropertyId allowed = registry.define("sheen", PropertyType::Float,
                                               PropertyDomain::Visual,
                                               PropertyValue::from_float(0.25f));
    CHECK(allowed != kInvalidProperty);
}

TEST_CASE("redefining with a different type is refused") {
    PropertyRegistry registry;
    CHECK(registry.define("density", PropertyType::Int, PropertyDomain::Simulation,
                          PropertyValue::from_int(0)) == kInvalidProperty);
}

TEST_CASE("values round-trip through their raw bits") {
    CHECK(PropertyValue::from_uint(1234).as_uint() == 1234);
    CHECK(PropertyValue::from_int(-77).as_int() == -77);
    CHECK(PropertyValue::from_bool(true).as_bool());
    CHECK_FALSE(PropertyValue::from_bool(false).as_bool());
    CHECK(PropertyValue::from_float(0.375f).as_float() == doctest::Approx(0.375f));
}

TEST_CASE("a map stores only overrides and falls back to the default") {
    PropertyRegistry registry;
    PropertyMap map;
    CHECK(map.size() == 0);
    CHECK(map.get(registry, props::kDensity).as_uint() == 1000);

    map.set(props::kDensity, PropertyValue::from_uint(7800));   // steel
    CHECK(map.size() == 1);
    CHECK(map.has(props::kDensity));
    CHECK(map.get(registry, props::kDensity).as_uint() == 7800);
    CHECK(map.get(registry, props::kFriction).as_uint() == 128);   // still the default

    map.clear(props::kDensity);
    CHECK(map.size() == 0);
    CHECK(map.get(registry, props::kDensity).as_uint() == 1000);
}

TEST_CASE("setting the same property twice replaces rather than appends") {
    PropertyMap map;
    map.set(props::kHardness, PropertyValue::from_uint(10));
    map.set(props::kHardness, PropertyValue::from_uint(200));
    CHECK(map.size() == 1);
    PropertyRegistry registry;
    CHECK(map.get(registry, props::kHardness).as_uint() == 200);
}

TEST_CASE("insertion order does not change identity, so records deduplicate") {
    // This is what lets two behaviour records written by different code paths collapse
    // into one entry in the voxel type table.
    PropertyMap a;
    a.set(props::kDensity, PropertyValue::from_uint(2600));
    a.set(props::kHardness, PropertyValue::from_uint(180));
    a.set(props::kFriction, PropertyValue::from_uint(90));

    PropertyMap b;
    b.set(props::kFriction, PropertyValue::from_uint(90));
    b.set(props::kHardness, PropertyValue::from_uint(180));
    b.set(props::kDensity, PropertyValue::from_uint(2600));

    CHECK(a == b);
    CHECK(a.content_hash() == b.content_hash());

    b.set(props::kFriction, PropertyValue::from_uint(91));
    CHECK_FALSE(a == b);
    CHECK(a.content_hash() != b.content_hash());
}
