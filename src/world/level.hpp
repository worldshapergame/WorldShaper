#pragma once
// R8a: a level is SIGNED, and what that costs.
//
// # What a negative level is
//
// Level 0 is one voxel, 3.125 cm. Every level up doubles the cell and every level DOWN halves one:
// level -1 is a sixteenth of a metre, -2 a thirty-second, and -k is 3.125/2^k centimetres. There is
// no floor in the rule, which is what "infinite" means in R8's own sentence — the depth is a
// property of the construction rather than a number somebody wrote down and hoped for.
//
// Until this file existed a level was a `u32` clamped at nought everywhere it was read, and the
// clamp is the whole of what R8a takes off. `NodePool::request` clamps at the brick, `node_march`
// clamps its descent target with `max(level, kLeafLevel)`, `subpixel_finest_level` floors at the
// leaf, and `VariationSample::depth` counts levels below a voxel as a POSITIVE number precisely
// because there was no signed level to put it in — its own comment says so, and says that when R8a
// lands it becomes `-level`. This is that.
//
// # The field stays a `u32`, and its bits are two's complement
//
// This is the one decision in R8a worth arguing about, so the argument is here rather than in a
// commit message.
//
// `NodeKey::level` and `FaceKey::level` are built from `u32` expressions in files this stage does
// not own. `src/app/main.cpp` writes `NodeKey{leaf.x >> up, leaf.y >> up, leaf.z >> up,
// leaf.level + up}` and `FaceKey{entry.x, entry.y, entry.z, level, face}`, and both are BRACED
// initialisers — which refuse a narrowing conversion outright rather than warning about one. So
// declaring the field `i32` is not a change to `node_pool.hpp`; it is a change to `main.cpp`, and
// the tree does not compile without it.
//
// The field therefore keeps its type and gains a meaning: **its bits are a two's-complement signed
// level.** For every level nought and above — which is every level any build before this one ever
// produced — the bits are identical, so the packing, the entry hash, the face hash and the GPU
// record are byte for byte what they were. Only the code that COMPARES two levels, takes a maximum
// of two, or SHIFTS by one has to change, and it changes to ask through `level_signed` instead of
// reading the field.
//
// That is not a free trade and the cost is stated rather than hidden: an unsigned comparison
// against a negative level silently reads it as four billion. Twice that is a rescue —
// `if (key.level < 32) ++churn_per_level_[key.level]` cannot index out of bounds, and
// `if (level < 16) ++by_level[level]` in main.cpp's face histogram cannot either — and everywhere
// else it is a fault waiting. The rule is: **inside `node_pool.*` and `face_store.*`, a level is
// never compared or shifted without going through `level_signed` first.**
//
// # Why the packed byte needs no bias
//
// `pack_node` and `pack_face` put a level in the bottom byte and mask it with 0xFF, so
// `level_word(-1)` lands as 0xFF and `level_word(-2)` as 0xFE — an ordinary two's-complement byte,
// which `level_from_byte` reads back. Nothing had to move, and the byte for every level 3 to 24 is
// what it always was.
//
// The one value that byte cannot carry twice is NOUGHT, because `GpuNode{}` is all zeroes and the
// node pool spells "the world has this and I have not built it" as a packed word of nought. That
// costs nothing here for a reason worth writing down rather than discovering: **the pool stores no
// node finer than a brick, in either arm.** Levels 2, 1 and 0 live inside a leaf's occupancy — a
// brick is 8³ voxels and the marcher walks them — and everything below nought is DERIVED by R8b's
// child source and never written to a record. So no `GpuNode` ever carries a level below three, and
// the sentinel keeps the only value it ever needed.
//
// A `GpuFace` is the other way round and that is why `kFaceLive` exists: a face at level 0 facing
// +x with no flags packs to nought, which was read as a free slot until a marker bit was added. A
// face at level -1 packs to 0xFF and is a live record like any other. **The face store is where a
// signed level is actually STORED**, which is what R8's plan means by "signed levels through
// `NodeKey`, the descent and the face key".

#include <cmath>

#include "core/types.hpp"

namespace ws {

// A brick is the leaf. Eight voxels a side, which is level 3.
//
// It lived in `node_pool.hpp` until R8a and moved here for one reason: it is the FLOOR of the signed
// level line, so the face store needs it as much as the node pool does — a face at level 3 is a
// brick face, and everything below it is a face on something finer than the pool has a record for.
// A constant two files both need and neither owns belongs to the thing it is about.
inline constexpr u32 kLeafLevel = 3;

// A level, read out of the `u32` the key carries it in.
constexpr i32 level_signed(u32 bits) { return static_cast<i32>(bits); }

// ...and written back into one. The inverse, and the only way a negative level is ever stored.
constexpr u32 level_word(i32 level) { return static_cast<u32>(level); }

// The same, out of the bottom byte of a packed word. Sign-extended, so a byte of 0xFF is level -1
// and not level 255.
constexpr i32 level_from_byte(u32 byte) {
    const u32 low = byte & 0xFFu;
    return (low >= 128u) ? (static_cast<i32>(low) - 256) : static_cast<i32>(low);
}

// How far below a voxel the ADDRESSING can go, which is the only cap there is and is not a policy.
//
// A cell at level -k is named by `voxel << k`, and a voxel coordinate is a signed 64-bit integer.
// So the honest bound is how many bits are left over: thirty-two here leaves thirty-one for the
// voxel, which is ±2^31 voxels — ±67,108 km of world — and a cell 2^-32 of a voxel is 7.3
// picometres. That is smaller than an atom and about fourteen levels finer than anything a screen
// could ask for standing a nanometre from a wall.
//
// It bounds the ADDRESS and not the detail, and the difference is the whole of R8b's design: the
// child source has no sub-voxel coordinate in it at all — a cell is identified by *(its parent,
// which of the eight children it is)* and the chain is one hash mix per step — so the SOURCE really
// is uncapped, and this is only how deep a `NodeKey` can still name what the source derived.
inline constexpr i32 kFinestLevel = -32;

// A node's size in voxels at any level: 8 at the brick, 1 at level 0, a half at level -1.
//
// f64 rather than an integer shift, because below nought there is no integer to shift. Above it the
// value is exact — every power of two from 2^-32 to 2^24 is exact in a double — so a caller that
// used to write `i64{1} << level` gets the identical number.
inline f64 level_span_voxels(i32 level) {
    if (level >= 0) return static_cast<f64>(u64{1} << (static_cast<u32>(level) & 63u));
    return 1.0 / static_cast<f64>(u64{1} << (static_cast<u32>(-level) & 63u));
}

// The same as a float, and constexpr, for the face store's coverage arithmetic.
//
// Written as a loop rather than as `ldexp` so it can be a constant expression: `face_coverage_pixels`
// is constexpr, its shader twin is a compile-time constant, and the two are compared by eye.
constexpr f32 level_extent_f32(i32 level) {
    if (level >= 0) return static_cast<f32>(u32{1} << (static_cast<u32>(level) & 31u));
    f32 extent = 1.0f;
    for (i32 step = level; step < 0; ++step) extent *= 0.5f;
    return extent;
}

}  // namespace ws
