#pragma once
// Deterministic hashing.
//
// documentation/05-simulation.md §12 rule 5: all randomness in the simulation comes from
// hash(coord, tick, salt). There is no RNG state to synchronise between machines, no
// order dependence, and no way for two peers to diverge — the same cell at the same tick
// always draws the same number everywhere.
//
// These must never change once a world exists: changing the mixing function changes
// every past simulation result. If a better hash is ever wanted, version it.

#include "core/types.hpp"

namespace ws {

// SplitMix64 finalizer. Passes SMHasher, one multiply-xorshift chain, no state.
constexpr u64 hash_mix(u64 x) noexcept {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

constexpr u64 hash_combine(u64 a, u64 b) noexcept { return hash_mix(a ^ (b * 0x9E3779B97F4A7C15ull)); }

// The canonical simulation entry point: a voxel coordinate, the tick, and a per-system
// salt so that two systems asking at the same place and time get independent streams.
constexpr u64 hash_cell(i64 x, i64 y, i64 z, u64 tick, u64 salt) noexcept {
    u64 h = hash_mix(static_cast<u64>(x) * 0x8CB92BA72F3D8DD7ull);
    h = hash_combine(h, static_cast<u64>(y) * 0xD6E8FEB86659FD93ull);
    h = hash_combine(h, static_cast<u64>(z) * 0xA24BAED4963EE407ull);
    h = hash_combine(h, tick);
    return hash_combine(h, salt);
}

// Uniform value in [0, bound). Uses the high bits, which mix best.
constexpr u32 hash_range(u64 h, u32 bound) noexcept {
    return static_cast<u32>((static_cast<u64>(static_cast<u32>(h >> 32)) * bound) >> 32);
}

// Uniform 16.16 fixed-point value in [0, 1).
constexpr i32 hash_unit_fx(u64 h) noexcept {
    return static_cast<i32>((h >> 48) & 0xFFFFu);
}

// FNV-1a over bytes — used for content hashes (mods, materials, reaction rules) where we
// need a stable identifier rather than a random stream.
constexpr u64 hash_bytes(const u8* data, usize size, u64 seed = 0xCBF29CE484222325ull) noexcept {
    u64 h = seed;
    for (usize i = 0; i < size; ++i) {
        h ^= data[i];
        h *= 0x100000001B3ull;
    }
    return h;
}

}  // namespace ws
