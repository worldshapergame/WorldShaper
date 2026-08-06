#pragma once
// Fundamental integer aliases used everywhere.
//
// Everything that mutates shared world state uses these exact-width integer types.
// See documentation/05-simulation.md §12 for why floating point is banned from those
// paths: two machines must produce bit-identical results or multiplayer desyncs.

#include <cstddef>
#include <cstdint>

namespace ws {

using u8  = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

using i8  = std::int8_t;
using i16 = std::int16_t;
using i32 = std::int32_t;
using i64 = std::int64_t;

using usize = std::size_t;
using isize = std::ptrdiff_t;

// Floating point is fine for rendering and UI. It is never used for shared state.
using f32 = float;
using f64 = double;

// --- world constants -------------------------------------------------------------
// documentation/00-vision.md: 1 metre = 32 voxels, one voxel = 3.125 cm.
inline constexpr i32 kVoxelsPerMetre = 32;
inline constexpr i32 kBrickEdge      = 8;                       // voxels
inline constexpr i32 kBrickVoxels    = kBrickEdge * kBrickEdge * kBrickEdge;   // 512
inline constexpr i32 kChunkBricks    = 32;                      // bricks per chunk edge
inline constexpr i32 kChunkEdge      = kBrickEdge * kChunkBricks;              // 256 voxels = 8 m
inline constexpr i32 kSimTickHz      = 20;                      // documentation/05 §1

// Compile-time size checks: these are load-bearing for the memory budgets in
// documentation/09-performance-budgets.md.
static_assert(kBrickVoxels == 512);
static_assert(kChunkEdge == 256);

}  // namespace ws
