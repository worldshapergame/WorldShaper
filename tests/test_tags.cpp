#include <doctest/doctest.h>

// doctest only forward-declares the stream types; comparing a string_view in a CHECK
// instantiates operator<< and needs the full definition.
#include <ostream>
#include <string>

#include "world/tags.hpp"

using namespace ws;

TEST_CASE("registry interns names once and keeps ids stable") {
    TagRegistry registry;
    const TagId stone = registry.find("stone");
    REQUIRE(stone != kInvalidTag);
    CHECK(registry.intern("stone") == stone);
    CHECK(registry.name(stone) == "stone");

    const TagId custom = registry.intern("mod_crystalline");
    CHECK(custom != kInvalidTag);
    CHECK(custom != stone);
    CHECK(registry.intern("mod_crystalline") == custom);
    CHECK(registry.find("never_registered") == kInvalidTag);
}

TEST_CASE("air is tag 0, because a zeroed record must mean empty space") {
    TagRegistry registry;
    CHECK(registry.find("air") == 0);
}

TEST_CASE("set membership") {
    TagRegistry registry;
    const TagId wood = registry.find("wood");
    const TagId flammable = registry.find("flammable");
    const TagId metal = registry.find("metal");

    TagSet set;
    CHECK(set.empty());
    set.add(wood);
    set.add(flammable);
    CHECK_FALSE(set.empty());
    CHECK(set.has(wood));
    CHECK(set.has(flammable));
    CHECK_FALSE(set.has(metal));

    set.remove(flammable);
    CHECK_FALSE(set.has(flammable));
    CHECK(set.has(wood));
}

TEST_CASE("has_all is the operation reaction rules run") {
    TagRegistry registry;
    const TagId wood = registry.find("wood");
    const TagId flammable = registry.find("flammable");
    const TagId buildable = registry.find("buildable");

    TagSet plank;
    plank.add(wood);
    plank.add(flammable);
    plank.add(buildable);

    TagSet burns;
    burns.add(flammable);
    CHECK(plank.has_all(burns));

    TagSet burns_and_metal;
    burns_and_metal.add(flammable);
    burns_and_metal.add(registry.find("metal"));
    CHECK_FALSE(plank.has_all(burns_and_metal));

    CHECK(plank.has_any(burns_and_metal));   // flammable matches

    TagSet nothing;
    CHECK(plank.has_all(nothing));           // an empty requirement matches everything
    CHECK_FALSE(plank.has_any(nothing));
}

TEST_CASE("tags past the fast range still work") {
    TagRegistry registry;
    // Push well past the 256-bit fast set.
    TagId high = kInvalidTag;
    for (u32 i = 0; i < 400; ++i) {
        high = registry.intern("filler_" + std::to_string(i));
    }
    REQUIRE(high >= kFastTagBits);

    TagSet set;
    set.add(high);
    CHECK(set.has(high));
    CHECK_FALSE(set.empty());
    CHECK(set.overflow().size() == 1);

    TagSet required;
    required.add(high);
    CHECK(set.has_all(required));

    set.remove(high);
    CHECK_FALSE(set.has(high));
    CHECK(set.empty());
}

TEST_CASE("overflow tags stay sorted and deduplicated") {
    TagSet set;
    set.add(500);
    set.add(300);
    set.add(400);
    set.add(300);
    REQUIRE(set.overflow().size() == 3);
    CHECK(set.overflow()[0] == 300);
    CHECK(set.overflow()[1] == 400);
    CHECK(set.overflow()[2] == 500);
}

TEST_CASE("equal sets hash equal, different sets do not") {
    TagSet a;
    a.add(3);
    a.add(9);
    a.add(300);

    TagSet b;
    b.add(300);   // inserted in a different order on purpose
    b.add(9);
    b.add(3);

    CHECK(a == b);
    CHECK(a.content_hash() == b.content_hash());

    b.add(4);
    CHECK_FALSE(a == b);
    CHECK(a.content_hash() != b.content_hash());
}

TEST_CASE("adding a tag twice is idempotent") {
    TagSet set;
    set.add(7);
    const u64 once = set.content_hash();
    set.add(7);
    CHECK(set.content_hash() == once);
}
