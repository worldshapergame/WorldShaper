#include <doctest/doctest.h>

#include <cstdlib>

#include "core/fixed.hpp"

using namespace ws;

TEST_CASE("fixed-point construction and round trip") {
    CHECK(Fx32::from_int(0).raw == 0);
    CHECK(Fx32::from_int(1).raw == kFxOne);
    CHECK(Fx32::from_int(-1).raw == -kFxOne);
    CHECK(Fx32::from_int(7).floor_int() == 7);
    CHECK(Fx32::from_int(-7).floor_int() == -7);
}

TEST_CASE("floor rounds toward negative infinity, not toward zero") {
    // This is the whole reason we shift instead of divide: a position of -0.5 must land
    // in voxel -1, not voxel 0, or every negative coordinate is off by one.
    const Fx32 minus_half = Fx32::from_fraction(-1, 2);
    CHECK(minus_half.floor_int() == -1);
    CHECK(Fx32::from_fraction(1, 2).floor_int() == 0);
    CHECK(Fx32::from_fraction(-3, 2).floor_int() == -2);

    const Fx64 big_negative = Fx64::from_int(-1000) + Fx64::from_fx32(Fx32::from_fraction(-1, 4));
    CHECK(big_negative.floor_int() == -1001);
}

TEST_CASE("fractional part is always non-negative") {
    CHECK(Fx32::from_fraction(-1, 2).frac_raw() == kFxOne / 2);
    CHECK(Fx32::from_fraction(1, 4).frac_raw() == kFxOne / 4);
}

TEST_CASE("addition and subtraction are exact") {
    const Fx32 a = Fx32::from_fraction(1, 3);
    const Fx32 b = Fx32::from_fraction(2, 3);
    CHECK((a + b - a) == b);
    CHECK((a - a).raw == 0);
}

TEST_CASE("multiplication keeps scale") {
    CHECK((Fx32::from_int(3) * Fx32::from_int(4)).floor_int() == 12);
    CHECK((Fx32::from_fraction(1, 2) * Fx32::from_fraction(1, 2)).raw == kFxOne / 4);
    CHECK((Fx32::from_int(-3) * Fx32::from_int(4)).floor_int() == -12);
    CHECK((Fx32::from_int(5) * 3).floor_int() == 15);
}

TEST_CASE("division keeps scale") {
    CHECK((Fx32::from_int(12) / Fx32::from_int(4)).floor_int() == 3);
    CHECK((Fx32::from_int(1) / Fx32::from_int(2)).raw == kFxOne / 2);
    CHECK((Fx32::from_int(10) / 4).raw == Fx32::from_fraction(5, 2).raw);
}

TEST_CASE("square root") {
    CHECK(fx_sqrt(Fx32::from_int(0)).raw == 0);
    CHECK(fx_sqrt(Fx32::from_int(1)).raw == kFxOne);
    CHECK(fx_sqrt(Fx32::from_int(4)).floor_int() == 2);
    CHECK(fx_sqrt(Fx32::from_int(144)).floor_int() == 12);

    // sqrt(2) ≈ 1.41421356; allow one unit in the last place.
    const i32 expected = static_cast<i32>(1.41421356 * kFxOne);
    CHECK(std::abs(fx_sqrt(Fx32::from_int(2)).raw - expected) <= 2);

    // Negative input is clamped rather than undefined.
    CHECK(fx_sqrt(Fx32::from_int(-5)).raw == 0);
}

TEST_CASE("square root of every perfect square in range is exact") {
    for (i32 n = 0; n < 180; ++n) {
        CHECK(fx_sqrt(Fx32::from_int(n * n)).raw == Fx32::from_int(n).raw);
    }
}

TEST_CASE("clamp, min, max, abs") {
    const Fx32 lo = Fx32::from_int(-2);
    const Fx32 hi = Fx32::from_int(5);
    CHECK(fx_clamp(Fx32::from_int(9), lo, hi) == hi);
    CHECK(fx_clamp(Fx32::from_int(-9), lo, hi) == lo);
    CHECK(fx_clamp(Fx32::from_int(1), lo, hi) == Fx32::from_int(1));
    CHECK(fx_abs(Fx32::from_int(-4)) == Fx32::from_int(4));
    CHECK(fx_min(lo, hi) == lo);
    CHECK(fx_max(lo, hi) == hi);
}

TEST_CASE("lerp is exact at the endpoints") {
    const Fx32 a = Fx32::from_int(10);
    const Fx32 b = Fx32::from_int(20);
    CHECK(fx_lerp(a, b, Fx32::from_int(0)) == a);
    CHECK(fx_lerp(a, b, Fx32::from_int(1)) == b);
    CHECK(fx_lerp(a, b, Fx32::from_fraction(1, 2)) == Fx32::from_int(15));
}

TEST_CASE("64-bit world coordinates survive the full world range") {
    // documentation/03 §1: 64-bit voxel coordinates. Check a position far from the origin
    // still resolves to the exact voxel it should.
    const i64 far_voxel = 100'000'000'000LL;
    const Fx64 p = Fx64::from_int(far_voxel) + Fx64::from_fx32(Fx32::from_fraction(3, 4));
    CHECK(p.floor_int() == far_voxel);
    CHECK(p.frac_raw() == (kFxOne * 3) / 4);
}

TEST_CASE("floating origin rebasing keeps sub-millimetre precision") {
    const Vec3Fx64 origin{Fx64::from_int(1'000'000), Fx64::from_int(-2'000'000),
                          Fx64::from_int(500'000)};
    const Vec3Fx64 world{origin.x + Fx64::from_fx32(Fx32::from_fraction(1, 8)),
                         origin.y - Fx64::from_fx32(Fx32::from_int(3)),
                         origin.z + Fx64::from_fx32(Fx32::from_int(64))};
    const Vec3Fx local = to_local(world, origin);
    CHECK(local.x.raw == kFxOne / 8);
    CHECK(local.y == Fx32::from_int(-3));
    CHECK(local.z == Fx32::from_int(64));
}

TEST_CASE("vector maths") {
    const Vec3Fx a{Fx32::from_int(3), Fx32::from_int(4), Fx32::from_int(0)};
    CHECK(length(a) == Fx32::from_int(5));
    CHECK(dot(a, a) == Fx32::from_int(25));

    const Vec3Fx b = a + a;
    CHECK(b.x == Fx32::from_int(6));
    CHECK((b - a).y == Fx32::from_int(4));
    CHECK((a * Fx32::from_int(2)).x == Fx32::from_int(6));
}

TEST_CASE("results are identical regardless of evaluation order") {
    // Determinism smoke test: the same sequence computed two ways must agree bit for bit.
    Fx32 forward = Fx32::from_int(0);
    for (i32 i = 1; i <= 100; ++i) forward += Fx32::from_fraction(1, i);

    Fx32 backward = Fx32::from_int(0);
    for (i32 i = 100; i >= 1; --i) backward += Fx32::from_fraction(1, i);

    // Addition of fixed-point values is associative and commutative because it is plain
    // integer addition — unlike floating point, where this test would fail.
    CHECK(forward.raw == backward.raw);
}
