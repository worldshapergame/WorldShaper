#pragma once
// Fixed-point arithmetic.
//
// Why this exists (documentation/11-reality-check.md §6):
//   GPUs and CPUs do not agree bit-for-bit on floating point across vendors — different
//   fused-multiply-add usage, different transcendental implementations. WorldShaper's
//   whole networking model depends on every machine computing identical simulation
//   results, so every value that touches shared world state is an integer with an
//   implied fractional scale instead.
//
// Layout: Fx32 is 16.16 (±32768, resolution 1/65536 ≈ 0.0000153).
//         Fx64 is 48.16 (±1.4e14, same resolution) for world-space positions, which at
//         3.125 cm per voxel covers far more than the 64-bit voxel coordinate range.
//
// Scale conversions round toward negative infinity via arithmetic shift, so floor_int()
// behaves correctly for negative coordinates (C's division would truncate toward zero and
// put -0.5 in the wrong voxel). Both are deterministic; only the shift is *correct* for
// spatial indexing.

#include <compare>
#include <limits>

#include "core/assert.hpp"
#include "core/types.hpp"

namespace ws {

inline constexpr i32 kFxFracBits = 16;
inline constexpr i32 kFxOne      = 1 << kFxFracBits;

// Arithmetic right shift that is well defined for negative values on every compiler we
// support (C++20 guarantees this for signed types, but we keep it explicit and tested).
template <typename T>
constexpr T fx_shr(T value, i32 bits) noexcept {
    return static_cast<T>(value >> bits);
}

// ---------------------------------------------------------------------------------
// Fx32 — 16.16
// ---------------------------------------------------------------------------------
struct Fx32 {
    i32 raw = 0;

    constexpr Fx32() noexcept = default;
    explicit constexpr Fx32(i32 raw_value) noexcept : raw(raw_value) {}

    static constexpr Fx32 from_raw(i32 v) noexcept { return Fx32{v}; }
    static constexpr Fx32 from_int(i32 v) noexcept { return Fx32{v * kFxOne}; }
    static constexpr Fx32 from_fraction(i32 numerator, i32 denominator) noexcept {
        return Fx32{static_cast<i32>((static_cast<i64>(numerator) * kFxOne) / denominator)};
    }

    // Float conversion is for rendering, UI and debugging only. Never call it from
    // simulation or physics; the CI lint in documentation/05 §12 rule 1 depends on that.
    static Fx32 from_f32_unsafe(f32 v) noexcept {
        return Fx32{static_cast<i32>(v * static_cast<f32>(kFxOne))};
    }
    constexpr f32 to_f32() const noexcept {
        return static_cast<f32>(raw) / static_cast<f32>(kFxOne);
    }

    // Truncates toward negative infinity, unlike C integer division.
    constexpr i32 floor_int() const noexcept { return fx_shr(raw, kFxFracBits); }
    constexpr i32 frac_raw() const noexcept { return raw & (kFxOne - 1); }

    constexpr auto operator<=>(const Fx32&) const noexcept = default;
    constexpr bool operator==(const Fx32&) const noexcept = default;
};

constexpr Fx32 operator+(Fx32 a, Fx32 b) noexcept { return Fx32{a.raw + b.raw}; }
constexpr Fx32 operator-(Fx32 a, Fx32 b) noexcept { return Fx32{a.raw - b.raw}; }
constexpr Fx32 operator-(Fx32 a) noexcept { return Fx32{-a.raw}; }

constexpr Fx32 operator*(Fx32 a, Fx32 b) noexcept {
    const i64 wide = static_cast<i64>(a.raw) * static_cast<i64>(b.raw);
    return Fx32{static_cast<i32>(fx_shr(wide, kFxFracBits))};
}

constexpr Fx32 operator*(Fx32 a, i32 b) noexcept { return Fx32{a.raw * b}; }
constexpr Fx32 operator*(i32 a, Fx32 b) noexcept { return Fx32{a * b.raw}; }

constexpr Fx32 operator/(Fx32 a, Fx32 b) noexcept {
    const i64 wide = (static_cast<i64>(a.raw) << kFxFracBits);
    return Fx32{static_cast<i32>(wide / b.raw)};
}

constexpr Fx32 operator/(Fx32 a, i32 b) noexcept { return Fx32{a.raw / b}; }

constexpr Fx32& operator+=(Fx32& a, Fx32 b) noexcept { a.raw += b.raw; return a; }
constexpr Fx32& operator-=(Fx32& a, Fx32 b) noexcept { a.raw -= b.raw; return a; }

constexpr Fx32 fx_abs(Fx32 a) noexcept { return a.raw < 0 ? Fx32{-a.raw} : a; }
constexpr Fx32 fx_min(Fx32 a, Fx32 b) noexcept { return a.raw < b.raw ? a : b; }
constexpr Fx32 fx_max(Fx32 a, Fx32 b) noexcept { return a.raw > b.raw ? a : b; }
constexpr Fx32 fx_clamp(Fx32 v, Fx32 lo, Fx32 hi) noexcept {
    return fx_min(fx_max(v, lo), hi);
}

// Linear interpolation with t in [0,1]. Exact at both endpoints.
constexpr Fx32 fx_lerp(Fx32 a, Fx32 b, Fx32 t) noexcept { return a + (b - a) * t; }

// Integer square root of a fixed-point value, deterministic on every platform.
// Uses the classic restoring bit-by-bit algorithm on the widened operand.
constexpr Fx32 fx_sqrt(Fx32 v) noexcept {
    if (v.raw <= 0) return Fx32{0};
    u64 value = static_cast<u64>(v.raw) << kFxFracBits;  // shift to keep the scale
    u64 result = 0;
    u64 bit = u64{1} << 62;
    while (bit > value) bit >>= 2;
    while (bit != 0) {
        if (value >= result + bit) {
            value -= result + bit;
            result = (result >> 1) + bit;
        } else {
            result >>= 1;
        }
        bit >>= 2;
    }
    return Fx32{static_cast<i32>(result)};
}

// ---------------------------------------------------------------------------------
// Fx64 — 48.16, for world-space positions
// ---------------------------------------------------------------------------------
struct Fx64 {
    i64 raw = 0;

    constexpr Fx64() noexcept = default;
    explicit constexpr Fx64(i64 raw_value) noexcept : raw(raw_value) {}

    static constexpr Fx64 from_raw(i64 v) noexcept { return Fx64{v}; }
    static constexpr Fx64 from_int(i64 v) noexcept { return Fx64{v * kFxOne}; }
    static constexpr Fx64 from_fx32(Fx32 v) noexcept { return Fx64{v.raw}; }

    constexpr i64 floor_int() const noexcept { return fx_shr(raw, kFxFracBits); }
    constexpr i32 frac_raw() const noexcept { return static_cast<i32>(raw & (kFxOne - 1)); }

    // Only valid once the value has been rebased near the floating origin.
    constexpr Fx32 to_fx32_unchecked() const noexcept {
        return Fx32{static_cast<i32>(raw)};
    }

    constexpr auto operator<=>(const Fx64&) const noexcept = default;
    constexpr bool operator==(const Fx64&) const noexcept = default;
};

constexpr Fx64 operator+(Fx64 a, Fx64 b) noexcept { return Fx64{a.raw + b.raw}; }
constexpr Fx64 operator-(Fx64 a, Fx64 b) noexcept { return Fx64{a.raw - b.raw}; }
constexpr Fx64 operator-(Fx64 a) noexcept { return Fx64{-a.raw}; }
constexpr Fx64 operator*(Fx64 a, i64 b) noexcept { return Fx64{a.raw * b}; }

// Deliberately no Fx64 * Fx64: it needs a 128-bit intermediate, which MSVC does not
// offer portably. World-space maths only ever needs "position scaled by a small factor",
// which this covers. If a use case appears, add an explicit 128-bit helper and test it —
// do not silently truncate.
constexpr Fx64 operator*(Fx64 a, Fx32 b) noexcept {
    // Split a into integer and fractional parts so the product fits in 64 bits for any
    // sane world coordinate; the fractional cross term is computed exactly.
    const i64 int_part  = a.floor_int();
    const i64 frac_part = a.raw & (kFxOne - 1);
    const i64 hi = int_part * static_cast<i64>(b.raw);
    const i64 lo = fx_shr(frac_part * static_cast<i64>(b.raw), kFxFracBits);
    return Fx64{hi + lo};
}

constexpr Fx64& operator+=(Fx64& a, Fx64 b) noexcept { a.raw += b.raw; return a; }
constexpr Fx64& operator-=(Fx64& a, Fx64 b) noexcept { a.raw -= b.raw; return a; }

constexpr Fx64 fx_abs(Fx64 a) noexcept { return a.raw < 0 ? Fx64{-a.raw} : a; }
constexpr Fx64 fx_min(Fx64 a, Fx64 b) noexcept { return a.raw < b.raw ? a : b; }
constexpr Fx64 fx_max(Fx64 a, Fx64 b) noexcept { return a.raw > b.raw ? a : b; }

// ---------------------------------------------------------------------------------
// Vectors. Deliberately minimal — grown only as the engine actually needs them.
// ---------------------------------------------------------------------------------
struct Vec3Fx {
    Fx32 x, y, z;
};

constexpr Vec3Fx operator+(Vec3Fx a, Vec3Fx b) noexcept { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
constexpr Vec3Fx operator-(Vec3Fx a, Vec3Fx b) noexcept { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
constexpr Vec3Fx operator*(Vec3Fx a, Fx32 s) noexcept { return {a.x * s, a.y * s, a.z * s}; }

constexpr Fx32 dot(Vec3Fx a, Vec3Fx b) noexcept { return a.x * b.x + a.y * b.y + a.z * b.z; }
constexpr Fx32 length(Vec3Fx a) noexcept { return fx_sqrt(dot(a, a)); }

// World-space position: 64-bit so the world is effectively unbounded
// (documentation/03-voxel-data-model.md §1).
struct Vec3Fx64 {
    Fx64 x, y, z;
};

constexpr Vec3Fx64 operator+(Vec3Fx64 a, Vec3Fx64 b) noexcept { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
constexpr Vec3Fx64 operator-(Vec3Fx64 a, Vec3Fx64 b) noexcept { return {a.x - b.x, a.y - b.y, a.z - b.z}; }

// Rebase a world position against a floating origin so it can be handed to the renderer
// as ordinary 32-bit values without losing precision.
constexpr Vec3Fx to_local(Vec3Fx64 world, Vec3Fx64 origin) noexcept {
    const Vec3Fx64 d = world - origin;
    return {d.x.to_fx32_unchecked(), d.y.to_fx32_unchecked(), d.z.to_fx32_unchecked()};
}

}  // namespace ws
