#include <doctest/doctest.h>

#include <cstring>

#include "core/arena.hpp"

using namespace ws;

TEST_CASE("arena hands out aligned memory and tracks usage") {
    Arena arena(4096);
    CHECK(arena.capacity() == 4096);
    CHECK(arena.used() == 0);

    void* a = arena.alloc(100, 16);
    REQUIRE(a != nullptr);
    CHECK(reinterpret_cast<usize>(a) % 16 == 0);
    CHECK(arena.used() == 100);

    void* b = arena.alloc(64, 64);
    REQUIRE(b != nullptr);
    CHECK(reinterpret_cast<usize>(b) % 64 == 0);
    CHECK(arena.used() >= 164);
    CHECK(a != b);
}

TEST_CASE("arena returns null instead of overflowing") {
    Arena arena(256);
    CHECK(arena.alloc(200) != nullptr);
    CHECK(arena.alloc(200) == nullptr);
    // A failed allocation must not have moved the cursor.
    CHECK(arena.used() == 200);
}

TEST_CASE("reset reuses memory without releasing it") {
    Arena arena(1024);
    void* first = arena.alloc(64);
    arena.reset();
    CHECK(arena.used() == 0);
    void* second = arena.alloc(64);
    CHECK(first == second);
    // The high-water mark survives a reset — it is what sizing decisions are based on.
    CHECK(arena.high_water() == 64);
}

TEST_CASE("scoped allocations roll back on exit") {
    Arena arena(1024);
    arena.alloc(32);
    const usize before = arena.used();
    {
        Arena::Scope scope(arena);
        arena.alloc(256);
        CHECK(arena.used() > before);
    }
    CHECK(arena.used() == before);
}

TEST_CASE("typed helpers") {
    Arena arena(4096);
    i32* values = arena.alloc_array<i32>(64);
    REQUIRE(values != nullptr);
    for (i32 i = 0; i < 64; ++i) values[i] = i * 3;
    CHECK(values[63] == 189);

    struct Thing {
        i32 a;
        i64 b;
    };
    Thing* t = arena.create<Thing>(7, 9);
    REQUIRE(t != nullptr);
    CHECK(t->a == 7);
    CHECK(t->b == 9);
    CHECK(reinterpret_cast<usize>(t) % alignof(Thing) == 0);
}

TEST_CASE("memory written through an arena stays intact") {
    Arena arena(1 << 16);
    u8* block = static_cast<u8*>(arena.alloc(1 << 15));
    REQUIRE(block != nullptr);
    std::memset(block, 0xAB, 1 << 15);
    u8* other = static_cast<u8*>(arena.alloc(1024));
    REQUIRE(other != nullptr);
    std::memset(other, 0x00, 1024);
    for (usize i = 0; i < (1u << 15); ++i) REQUIRE(block[i] == 0xAB);
}

TEST_CASE("arena is movable") {
    Arena a(1024);
    a.alloc(128);
    Arena b(std::move(a));
    CHECK(b.used() == 128);
    CHECK(b.capacity() == 1024);
    CHECK(a.capacity() == 0);
}
