// R8b: hashed variation — where a node's children come from when nothing else can answer.
//
// The other copy of `src/world/node_pool.hpp`'s variation block, and the rule that governs the two
// is the same one `field_leaf.glsl` states about the field: **when this and node_pool.hpp
// disagree, node_pool.hpp is right.** Every function here is transliterated line for line from
// there, in the same order, and where an expression looks redundant it has been LEFT redundant.
// A line tidied on the way across is a disagreement nobody wrote down and nobody can bisect.
//
// # What this is for
//
// `21-renderer-rewrite.md` §7 lists three child sources in the order they are tried: the material's
// own field (R11c), then this, then whatever the player has carved. This one is the middle, and it
// is the one that is ALWAYS available — a hand-carved world has no field by construction, and
// neither has a world loaded from a file whose clip is gone. Those are not edge cases. They are
// what somebody who has been building for a week is standing in.
//
// # The tree is the coordinate
//
// A sub-voxel cell is identified by *(its parent, which of the eight children it is)* and by
// nothing else. The chain starts as a hash of the VOXEL it is inside and takes one mix per octant
// on the way down, so there is no sub-voxel coordinate anywhere to overflow and the depth is
// unbounded by construction rather than by a number somebody wrote down and hoped for.
//
// # Every step is integer, and that is the promise
//
// Same key, same children, for ever and on every machine. Two floating-point pipelines that agree
// to within an ulp are two pipelines that disagree about a rounded byte a few times in a thousand,
// and a few times in a thousand over a wall is a wall that shimmers on one computer and not on the
// next. So the hash is `uint` from end to end and the colour nudge is `int` from end to end; the
// only float in the whole source is the knob a human wrote, quantised once on the CPU into
// `amount_q` and passed in already whole.
//
// # It varies the MATERIAL and not the SHAPE
//
// All eight children of a solid voxel are solid; the eight children of air are air. Hashing the
// shape as well would be one line and it is wrong twice: it changes the silhouette of a wall as
// you walk towards it, which is exactly the swimming this source exists to avoid, and it does not
// conserve matter — §7 is explicit that simulation, physics and the matter ledger stay at level 0,
// and a child source that erodes a voxel into seven eighths of one has quietly handed the renderer
// a different world from the one the chisel reads.

#ifndef WS_VARIATION_GLSL
#define WS_VARIATION_GLSL

// The defaults, so a call site needs nothing bound to try this. Must match `kVariationSeed` and
// `variation_amount_q(kVariationColour)` in src/world/node_pool.hpp — 0.05 of full scale is
// `20-clip-forge.md` section 7's own worked example, and 0.05 * 256 rounds to thirteen.
//
// They are constants rather than push-constant fields because nothing pushes them yet. `lens.w` is
// unused and is where a runtime knob would go; that is a change to `params.glsl`, and it is worth
// making only once somebody wants to turn the grain up while the game is running.
const uint kVariationSeed = 0x57538B01u;
const uint kVariationAmountQ = 13u;

// Must match `node_hash_mix` in src/world/node_pool.hpp. Repeated under its own name rather than
// depending on `node.glsl` having been included first, so that this file compiles alone and the
// two bodies can be compared by eye.
uint variation_mix(uint x) {
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

// The seed of the chain at a voxel. Must match `variation_root_hash` in node_pool.hpp.
//
// `coord` is the VOXEL, not the sub-voxel cell — 32-bit, truncated the same way `node_entry_hash`
// truncates, because two voxels 2^32 apart sharing a grain is not something anybody will ever be
// in a position to see.
uint variation_root_hash(ivec3 coord, uint seed) {
    uint h = variation_mix(0x9E3779B9u ^ seed);
    h = variation_mix(h ^ uint(coord.x));
    h = variation_mix(h ^ uint(coord.y));
    h = variation_mix(h ^ uint(coord.z));
    return h;
}

// One step down. Must match `variation_child_hash` in node_pool.hpp.
//
// The octant is folded in through an odd multiplier rather than XORed raw, so the eight children of
// one parent do not share their low bits — and the low byte is what the colour is read out of.
uint variation_child_hash(uint parent_hash, uint octant) {
    return variation_mix(parent_hash ^ (0x85EBCA6Bu + octant * 0x9E3779B9u));
}

// Must match `variation_byte` in node_pool.hpp.
uint variation_byte(uint h, uint which) { return (h >> (which * 8u)) & 0xFFu; }

// One channel nudged and clamped. Must match `variation_nudge` in node_pool.hpp.
//
// `delta = (bits - 128) * amount_q * 255 / (128 * 256)`, which is one multiply and a shift of 15.
// The MAGNITUDE is shifted and the sign re-applied, never the signed value: a right shift of a
// negative integer is implementation-dependent in GLSL and defined in C++20, so the one form that
// is the same on both sides is the one that never shifts a negative.
uint variation_nudge(uint base, uint bits, uint amount_q) {
    int offset = int(bits) - 128;
    uint magnitude = uint(abs(offset));
    int scaled = int((magnitude * amount_q * 255u) >> 15u);
    int moved = int(base) + ((offset < 0) ? -scaled : scaled);
    return uint(clamp(moved, 0, 255));
}

// A child's colour from its parent's. Must match `variation_child_colour` in node_pool.hpp.
//
// Alpha is carried through untouched. Alpha is COVERAGE (D139), not opacity, and a solid child of a
// solid parent covers exactly what its parent covered.
uint variation_child_colour(uint parent_colour, uint child_hash, uint amount_q) {
    uint r = variation_nudge(parent_colour & 0xFFu, variation_byte(child_hash, 0u), amount_q);
    uint g = variation_nudge((parent_colour >> 8) & 0xFFu, variation_byte(child_hash, 1u), amount_q);
    uint b = variation_nudge((parent_colour >> 16) & 0xFFu, variation_byte(child_hash, 2u), amount_q);
    return (parent_colour & 0xFF000000u) | (b << 16) | (g << 8) | r;
}

// Which of its parent's eight a cell is, at `step` levels above the finest. Must match
// `octant_of` in node_pool.hpp: x fastest, matching brick and cell order everywhere else in the
// engine, so this needs no translation table.
uint variation_octant(ivec3 cell, uint step) {
    return uint((cell.x >> step) & 1) | (uint((cell.y >> step) & 1) << 1) |
           (uint((cell.z >> step) & 1) << 2);
}

// The whole walk, and the only entry point a caller needs.
//
// `voxel` is the voxel the cell is inside. `cell` is the cell's coordinate at `depth` levels below
// a voxel — so `cell >> depth == voxel`, which is the caller's own arithmetic and is asserted on
// the CPU side rather than trusted here. `colour` in is the voxel's own rgba8; out is the cell's.
//
// **Only the low `depth` bits of `cell` are read.** `variation_octant` takes bit `step` of each
// axis and `step` never reaches `depth`, so a caller holding the cell's LOCAL offset within its
// voxel — which is what a marcher has, and what stays inside an `int` at any depth — may pass that
// instead of an absolute coordinate and get the identical answer. The CPU's `mirror_variation`
// takes the absolute one because it also has to find the voxel; both read the same bits.
//
// Must match `NodePool::mirror_variation` in src/world/node_pool.cpp, which is what the headless
// test walks the whole facility with. That test exists because a structure the renderer walks and
// nobody compares against the world is a renderer debugging a mirage.
uint variation_descend(ivec3 voxel, ivec3 cell, uint depth, uint colour, uint seed, uint amount_q) {
    uint h = variation_root_hash(voxel, seed);
    for (uint step = depth; step > 0u; --step) {
        h = variation_child_hash(h, variation_octant(cell, step - 1u));
        colour = variation_child_colour(colour, h, amount_q);
    }
    return colour;
}

#endif  // WS_VARIATION_GLSL
