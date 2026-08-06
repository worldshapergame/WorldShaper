#include <doctest/doctest.h>

#include <thread>
#include <vector>

#include "core/ring.hpp"

using namespace ws;

TEST_CASE("ring reports empty, fills, and refuses to overflow") {
    SpscRing<i32, 8> ring;
    CHECK(ring.empty());
    CHECK(ring.capacity() == 7);

    for (i32 i = 0; i < 7; ++i) CHECK(ring.push(i));
    CHECK_FALSE(ring.push(99));  // full
    CHECK(ring.size() == 7);
    CHECK_FALSE(ring.empty());
}

TEST_CASE("values come out in the order they went in") {
    SpscRing<i32, 16> ring;
    for (i32 i = 0; i < 10; ++i) REQUIRE(ring.push(i * 5));
    for (i32 i = 0; i < 10; ++i) {
        i32 out = -1;
        REQUIRE(ring.pop(out));
        CHECK(out == i * 5);
    }
    i32 unused = 0;
    CHECK_FALSE(ring.pop(unused));
}

TEST_CASE("indices wrap without losing data") {
    SpscRing<i32, 4> ring;
    for (i32 round = 0; round < 100; ++round) {
        REQUIRE(ring.push(round));
        i32 out = -1;
        REQUIRE(ring.pop(out));
        CHECK(out == round);
    }
    CHECK(ring.empty());
}

TEST_CASE("one producer and one consumer on separate threads lose nothing") {
    // This is the pattern every cross-thread queue in the engine uses, so it is worth
    // testing under real contention rather than assuming.
    constexpr i32 kCount = 200000;
    SpscRing<i32, 1024> ring;

    std::thread producer([&ring] {
        for (i32 i = 0; i < kCount; ++i) {
            while (!ring.push(i)) std::this_thread::yield();
        }
    });

    i64 sum = 0;
    i32 received = 0;
    while (received < kCount) {
        i32 value = 0;
        if (ring.pop(value)) {
            CHECK(value == received);  // ordering must hold exactly
            sum += value;
            ++received;
        } else {
            std::this_thread::yield();
        }
    }
    producer.join();

    const i64 expected = (static_cast<i64>(kCount) - 1) * kCount / 2;
    CHECK(sum == expected);
    CHECK(ring.empty());
}

TEST_CASE("ring carries structs as well as scalars") {
    struct Op {
        u64 tick;
        i32 x, y, z;
    };
    SpscRing<Op, 8> ring;
    REQUIRE(ring.push(Op{42, 1, 2, 3}));
    Op out{};
    REQUIRE(ring.pop(out));
    CHECK(out.tick == 42);
    CHECK(out.z == 3);
}
