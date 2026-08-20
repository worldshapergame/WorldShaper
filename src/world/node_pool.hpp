#pragma once
// The node pool: one sparse octree, at every scale, replacing the chunk system entirely.
//
// # What it replaces, and why all of it at once
//
// Geometry is currently addressed by four separate schemes glued end to end — a wrapped chunk
// grid, a per-chunk brick mask with popcount prefixes, a per-chunk brick-mask pyramid, and a
// separate summary-octree tier with its own wrapped grids and its own residency policy. Every
// seam between them has produced a bug with its own decision-log entry (D133, D137, D146, D147,
// D148, D151, D155), and they are all the same bug: two structures that answer the same question
// and are allowed to disagree.
//
// They exist because a chunk is a fixed 8 m box in a renderer whose entire premise is that
// nothing has a fixed size.
//
// So: one structure, at every scale, with one answer.
//
// # What a node is
//
//   level 3   a brick: 8 voxels, 25 cm. The finest node, and the leaf.
//   level L   2^L voxels. Level 8 is what used to be a chunk; nothing treats it specially.
//
// A node's coordinate is its voxel coordinate shifted right by its level, so a node at level L
// covers voxels [c << L, (c+1) << L).
//
// Below level 3 nothing changes: a brick keeps its palette encoding, its 64-byte occupancy and
// its two occupancy mips, and a ray marches inside it at single-voxel resolution exactly as it
// does now (D132 — the detail level chooses the colour, never the shape). Bricks are the
// storage and compression unit and they work; chunks are the fixed-size box and they do not.
//
// # Why the children are contiguous
//
// A node holds ONE index — the base of its eight children — rather than eight. Descending is
// then `children + octant`, a direct index with no hash and no search, and the eight siblings a
// descent chooses between share a cache line. Only the *entry* into the tree costs a hash
// lookup; everything below it is pointer arithmetic.
//
// Today a ray pays two dependent loads per chunk it enters (the wrapped grid cell, then that
// record's own coordinate, because the grid wraps and a cell may be held by a chunk from
// somewhere else) and does it up to thirty-two times along each axis it crosses, plus a separate
// coarse-grid fetch per skip. On a machine whose binding constraint is memory bandwidth
// (documentation/09 §1) that difference is the point of the exercise.
//
// # Why streaming cannot deadlock any more
//
// A ray reaches a node only through its parent, and a parent's `child_mask` says whether the
// child exists in the *world* — not whether it is resident. So a ray can never fail to report
// something that exists, and can never report something that does not. That is the whole of
// D133 and D147, and it is now unrepresentable rather than fixed: there is no second structure
// to disagree with the first.
//
// # What is deliberately NOT here
//
// Chunks still exist on disk and on the wire. `06-multiplayer.md` reconciles per chunk,
// `05-simulation.md` sleeps and wakes bricks, authority regions are 64 m cells, and `.wsworld`
// writes chunk-sized records. None of that is rendering and none of it benefits from being torn
// up. Chunks leave the *renderer*; they remain a storage grouping the renderer never sees.

#include <bit>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/block_pool.hpp"
#include "core/dirty_set.hpp"
#include "core/types.hpp"
#include "world/gpu_brick.hpp"
#include "world/level.hpp"
#include "world/world.hpp"

namespace ws {

class VoxelTypeTable;

inline constexpr u32 kNoNode = 0xFFFFFFFFu;

// A brick is the leaf. Eight voxels a side, which is level 3. **Moved to `world/level.hpp` by R8a**,
// because the face store needs the same floor and neither file owns it; it is still spelt
// `kLeafLevel` and still means what it meant.

// The coarsest node the pool will build. Level 24 is 16.7 million voxels a side — 524 km — which
// is past anything a 64-bit voxel world is asked to draw at once, and the tree is sparse so the
// levels nobody reaches cost nothing.
inline constexpr u32 kMaxNodeLevel = 24;

// Where a ray enters the tree. Above this, lookup is a hash; below it, pointer arithmetic.
//
// Level 14 is 16,384 voxels — 512 m. Coarse enough that a ray crossing a scene enters through a
// handful of them and pays a handful of hashes; fine enough that the table stays small and a
// probe stays in cache. This is a performance knob and nothing depends on its value being right,
// which is the property to preserve when tuning it.
inline constexpr u32 kEntryLevel = 14;

// ---- R2b's second half: a node finer than the pixel is never STORED ---------------------------
//
// *Never requested* has been done since D190. *Never stored* has been blocked since D259 on one
// sentence -- "eviction can only drop what it can afford to rebuild" -- and R12 was named as what
// unblocks it, because a node the card can derive in a dispatch costs nothing to throw away.
//
// **That is not the only way out, and the other one needs no card at all.** The pool does not have
// to be able to rebuild a node it never has to serve again. A node the MARCHER's own descent will
// never target is a node no ray will ever report missing, so giving it up is not a bet on being
// able to rebuild it -- it is the observation that nothing will ask.
//
// So the rule is the marcher's rule, read off `node_march` rather than invented beside it:
//
//     footprint = t * pixel_angle          (voxels, in shaders/node.glsl)
//     level     = floor(log2(footprint)) + dither,   dither in [0,1)
//     target    = max(level, kLeafLevel)
//
// `dither` is never negative, so the FINEST level any ray at distance t will ever descend to is
// `max(floor(log2(t * pixel_angle)), kLeafLevel)`. Anything finer than that is detail the walk
// cannot reach: it is not "probably invisible", it is unaddressable.
//
// # Why this agrees with the ladder rather than competing with it
//
// `main.cpp`'s `kRefineSplitAt` is `8.0 * 0.002` and D674 reads it as eight pixels at 0.002 of
// their own distance, a ladder node being eight CELLS a side. Take the eight back out and what is
// left is the angle one pixel subtends -- which is the marcher's `pixel_angle` and this constant.
// The two rules then land on the same place by arithmetic rather than by agreement: a ladder node
// settles at level L when `2^L >= 0.512 * d_metres`, so its cells are `2^(L-3)` voxels across,
// which is `0.064 * d_metres` -- exactly `t * 0.002` in voxels. **The finest content the world is
// ever given at a distance is the finest node the marcher will ever ask for there.** A pool holding
// anything finer is holding something neither the world nor the screen has an opinion about.
//
// # What it costs to give one up, and why the picture does not move
//
// A node's slot is cleared and its parent's run of eight is KEPT. The descent then stops at the
// parent, which still carries the colour and the per-direction coverage it was folded from, and
// `node_march` reports `kFoundHere` on it -- an ordinary coarse hit at exactly the level the pixel
// asked for. No stand-in, no `kFoundWanted`, no report, no rebuild. That is the whole safety
// argument and it is an argument rather than a counter: trap 20 says a pass that gets cheaper by
// building less is a regression in improvement's clothes, and the defence has to be a reason the
// picture cannot change, checked against a content hash.
//
// One thing had to be repaired for it to hold. `fold_children` sets a node's colour to nought when
// none of its children are built, which is right for a node that has never had any and wrong for
// one whose children have been given up -- it turns the parent black at the moment it becomes the
// thing being drawn. The fold of a node the rule stopped at is therefore skipped; its ancestors
// still fold, and they read its colour, which is intact.

// The angle one pixel subtends, in radians, at 1280x800 through a 90-degree lens.
//
// Written as `kRefineSplitAt / 8` rather than as `0.002` so the derivation is in the source and
// not only in this comment: it is main.cpp's ladder constant with the ladder's eight cells taken
// back out. It cannot be shared with main.cpp's own `kRefineSplitAt` -- that is a translation unit
// constant in the same namespace, and declaring it here as well is a redefinition -- so if one
// moves, the other must, and `test_node_pool_evict.cpp` pins both against D674's distances.
inline constexpr f64 kLadderSplitAt = 8.0 * 0.002;
inline constexpr f64 kLadderCellsPerSide = 8.0;
inline constexpr f64 kSubPixelAngle = kLadderSplitAt / kLadderCellsPerSide;

// The coarsest level `node_march` will march at. Must match `kNodeMaxDetail` in shaders/node.glsl.
//
// It matters here and it is easy to miss: the marcher clamps its descent target to this, so past
// about 1.6 km it stops asking coarser and goes on asking for level 7. A rule that let the pool
// erode past level 7 would take the very node those rays land on.
inline constexpr u32 kMarcherMaxDetail = 7;

// The finest node level a ray at this distance will ever descend to, SIGNED — R8a.
//
// `floor(log2(footprint))`, done with bit_width rather than std::log2 so it cannot round the other
// way from the shader's `floor` on a boundary value -- 16.0 must answer 4 and 15.999 must answer 3,
// and a float log2 of an exact power of two is not reliably exact.
//
// Distance is to the NEAREST point of the node's box, which is the conservative direction: a ray
// reaching the node has travelled at least that far, so its own footprint is at least this one and
// its own target is at least this level. Erring the other way -- centre distance, or the far face --
// would evict nodes rays still address.
//
// # `infinite` is the whole of R8a, and this is the only place the clamp lives
//
// With it FALSE the answer floors at `kLeafLevel` and this function is, line for line, what
// `subpixel_finest_level` was before R8a: a footprint of one voxel or less answers three, and every
// build before this one took that branch on every call. With it TRUE the floor comes off both ends —
// a footprint of an eighth of a voxel answers -3, and a camera 10 cm from a wall answers -8 — and
// nothing else in the arithmetic moves.
//
// The `> 1.0` branch is kept whole rather than folded into the general case on purpose. It is the
// path the shipped arm takes, so it executes the same instructions it always did and a difference in
// the world hash cannot be hiding in a rewrite of it. `std::ilogb` is only ever reached below one
// voxel, which is a place no build before R8a could go.
inline i32 marcher_finest_level(f64 distance_voxels, f64 pixel_angle, bool infinite) {
    const f64 footprint = distance_voxels * pixel_angle;
    const i32 floor_level = infinite ? kFinestLevel : static_cast<i32>(kLeafLevel);
    // Also the NaN guard: a comparison against NaN is false, so a camera that has not been set
    // answers "keep everything".
    if (!(footprint > 1.0)) {
        if (!infinite || !(footprint > 0.0)) return floor_level;
        // `ilogb` READS the exponent rather than computing a logarithm, so `floor(log2(x))` below a
        // voxel is exact the same way `bit_width` is exact above one: 0.125 answers -3 and 0.1249
        // answers -4, with no rounding for two machines to disagree about on the boundary itself.
        const i32 level = std::ilogb(footprint);
        return (level < floor_level) ? floor_level : level;
    }
    const i32 level = static_cast<i32>(std::bit_width(static_cast<u64>(footprint))) - 1;
    if (level < floor_level) return floor_level;
    return (level > static_cast<i32>(kMarcherMaxDetail)) ? static_cast<i32>(kMarcherMaxDetail)
                                                         : level;
}

// R2b's own reading of the rule: unsigned, floored at the leaf, and unchanged by R8a.
//
// It keeps its name, its type and its tests because R2b's rule is about what the pool STORES, and
// the finest stored node is a brick in both arms — see `level.hpp` for why nothing below one is ever
// written to a record. One implementation, so the two cannot drift apart: trap 13 is two indexes
// answering one question, and this stage adds a second reading of exactly the quantity D674 lost an
// evening to.
inline u32 subpixel_finest_level(f64 distance_voxels, f64 pixel_angle) {
    return static_cast<u32>(marcher_finest_level(distance_voxels, pixel_angle, false));
}

// Where the proximity sweep anchors itself. 64 voxels is two metres.
//
// The sweep restarts when this changes, and it takes a couple of hundred frames to cover twenty
// metres, so anchoring it any finer means a walking player restarts it before it ever finishes.
// A brick would restart it four times a metre.
inline constexpr u32 kProximityAnchorLevel = 6;

// How many frames one full erosion sweep is spread over. `cold_frames` is six hundred, so eight
// slices costs eight frames of latency on an eviction and saves seven eighths of the scan.
inline constexpr u32 kErodeSlices = 8;

// How soon after an eviction a request for the same node counts as the pool having thrown away
// something it needed.
//
// Two seconds at sixty frames. A node that is asked for again this quickly was never unwanted:
// the six-hundred-frame cold window is meant to be long enough that turning round and back does
// not cost a rebuild, so anything coming straight back is a node whose "wanted" signal was lost
// rather than a node that genuinely went out of use. This is the harm the eviction instrument
// measures, and it is mechanism-independent -- it does not care WHY the signal was lost.
inline constexpr u32 kChurnWindow = 120;

// Who asked for a node. Instrument only: the pool serves every request the same way.
//
// A node coming back tells you the pool was wrong to drop it; WHO asks for it back tells you which
// signal was missing. A ray reporting a miss is the renderer noticing a hole in what it is drawing;
// a dilated neighbour is a guess about where the next hole will be; the proximity sweep is not
// about the screen at all.
enum RequestSource : u8 {
    kRequestRay = 0,        // a primary ray could not find something it was drawing
    kRequestDilated = 1,    // a neighbour of one of those, streamed on the guess that it is next
    kRequestProximity = 2,  // the twenty-metre radius, which is not about the screen at all
    // A shadow, ambient or lamp ray stopped by a cell the pool has not built. D292 forbids a light
    // path from dragging residency towards what it CROSSES; this is the one cell that stopped it,
    // which R9i narrowed the rule for, because otherwise the cell stays unbuilt, stays opaque, and
    // casts a shadow for ever.
    kRequestOcclusion = 3,
    kRequestSourceCount = 4,
};

// A node, at any level. `level` is SIGNED and carried in a `u32` as two's complement -- see
// `world/level.hpp`, which is the whole of the convention and the reason the field is not an `i32`.
// Read it with `level_signed(key.level)` and never compare or shift it raw.
struct NodeKey {
    i64 x = 0;
    i64 y = 0;
    i64 z = 0;
    u32 level = 0;
    bool operator==(const NodeKey& other) const {
        return x == other.x && y == other.y && z == other.z && level == other.level;
    }
    constexpr i32 signed_level() const { return level_signed(level); }
};

struct NodeKeyHash {
    usize operator()(const NodeKey& k) const noexcept {
        return static_cast<usize>(hash_cell(k.x, k.y, k.z, k.level, 0x4E4F4445ull));
    }
};

// Which node at `level` contains a voxel. Arithmetic shift, not division: a voxel at -1 belongs
// to node -1, not node 0. Getting this wrong puts every negative coordinate one node out, which
// is the same trap `chunk_of` documents.
//
// R8a: below level 0 the shift runs the OTHER WAY, and the question changes shape with it. A voxel
// does not sit inside a cell finer than itself -- it CONTAINS 8^k of them -- so what comes back is
// the corner cell, the one at the voxel's own low corner, which is the cell `node_key_of` at level 0
// would have named scaled down. That makes the two directions each other's inverse: shifting the
// answer back up by the same amount returns the voxel it came from, at any depth.
constexpr NodeKey node_key_of(i64 x, i64 y, i64 z, u32 level) {
    const i32 at = level_signed(level);
    if (at >= 0) return NodeKey{x >> at, y >> at, z >> at, level};
    return NodeKey{x << -at, y << -at, z << -at, level};
}

// The voxel a cell lies in, for a key at or below level 0. The other direction of the same shift,
// named because it is the question the child source actually asks: R8b's chain is seeded from the
// VOXEL and stepped down by octant, so every sub-voxel read starts here.
constexpr NodeKey voxel_of_key(const NodeKey& key) {
    const i32 at = key.signed_level();
    if (at >= 0) return NodeKey{key.x << at, key.y << at, key.z << at, 0};
    return NodeKey{key.x >> -at, key.y >> -at, key.z >> -at, 0};
}

// Which of its parent's eight children a node is. x fastest, matching brick and cell order
// everywhere else in the engine, so a shader can walk it with no translation table.
constexpr u32 octant_of(i64 x, i64 y, i64 z) {
    return static_cast<u32>((x & 1) | ((y & 1) << 1) | ((z & 1) << 2));
}

// The bucket a root lands in.
//
// Deliberately a 32-bit mix rather than the 64-bit `hash_cell` the CPU map uses, and mixed one
// axis at a time rather than by XORing three products together. Two reasons, and both have cost
// somebody a day in this file's neighbourhood already:
//
//   the shader has to compute the identical value, and a 32-bit mix is short enough that the two
//   implementations can be compared by eye rather than trusted;
//
//   voxel coordinates are a dense lattice, and XOR-of-products over a lattice leaves whole planes
//   landing in the same few buckets — which is exactly the fault the face cache's key hit, where
//   faces that failed to find a slot rendered as scattered black voxels.
//
// Must match `node_entry_hash` in shaders/node.glsl.
constexpr u32 node_hash_mix(u32 x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

constexpr u32 entry_hash32(i32 x, i32 y, i32 z, u32 level) {
    u32 h = 0x811C9DC5u;
    h = node_hash_mix(h ^ static_cast<u32>(x));
    h = node_hash_mix(h ^ static_cast<u32>(y));
    h = node_hash_mix(h ^ static_cast<u32>(z));
    return node_hash_mix(h ^ level);
}

// ---- R8b: hashed variation, the child source for a world with no field behind it ---------------
//
// R8's mechanism, after R11 took the two general halves of it (R8c is the field answering at any
// resolution, R8d is a derived node being evictable). What is left is the source of last resort:
// **where a node's children come from when there is no field to ask.** A hand-carved world has
// none by construction, and so does a world loaded from a file whose clip is gone -- and those are
// not edge cases, they are what a player who has been building for a week is standing in.
//
// The plan's §7 names three sources in the order they are tried: the material's own field, then
// this, then whatever the player has actually carved. This one is the middle: "cheap, always
// available, never wrong-looking".
//
// # The tree is the coordinate, so depth is unbounded
//
// A sub-voxel node is identified by *(its parent, which of the eight children it is)* and by
// nothing else. So the seed of the chain is a hash of the VOXEL the chain starts in, and every
// step down is one more mix with the octant. There is no fixed-width sub-voxel coordinate to
// overflow, which is what makes "no cap -- infinite" a property of the construction rather than a
// number written down somewhere and hoped for.
//
// Same key, same children, for ever and on every machine: the whole chain is integer arithmetic
// over 32-bit words, so it has no rounding to disagree about. That is what makes a sub-voxel
// surface stable when you walk towards it and away again -- the grain does not swim, because it
// was never computed from where you are standing.
//
// # It varies the MATERIAL and not the SHAPE, and that is a decision rather than an omission
//
// All eight children of a solid voxel are solid, and the eight children of air are air. Hashing
// the shape as well would be one line and it is wrong twice over: it changes the silhouette of a
// wall as you approach it, which is the swimming this mode exists to avoid, and it does not
// conserve matter -- `21-renderer-rewrite.md` §7 is explicit that simulation, physics and the
// matter ledger stay at level 0, and a child source that erodes a voxel into seven-eighths of one
// has quietly given the renderer a different world from the one the chisel reads.
//
// So what descends is `20-clip-forge.md` §7's perturbation, taken below the voxel: colour, hashed
// from position, compounding down the chain so that sub-cells sharing a parent are more alike than
// sub-cells that do not. That is what a real surface does. Roughness is the other half of §7 and
// it is NOT here, because roughness lives in the type table and minting a record per sub-voxel is
// the thing §7's own budget exists to stop; the node carries rgba8 and this varies the rgb of it.
//
// # Why the arithmetic is 32-bit
//
// `node_hash_mix`'s comment gives the reason and it applies here word for word: the shader has to
// compute the identical value, and a 32-bit mix is short enough that the two implementations can
// be compared by eye rather than trusted. `shaders/variation.glsl` is the other copy, and
// `test_node_pool_evict.cpp` pins the constants either side would have to change together.

// The default world seed for the child source. Nothing derives it from the clip on purpose: a
// world with no clip behind it is exactly the case this serves, so the seed cannot come from one.
inline constexpr u32 kVariationSeed = 0x57538B01u;

// How far a child's colour may move from its parent's, as a fraction of full scale. 0.05 is
// `20-clip-forge.md` §7's own worked example -- `variation colour=0.05` -- and it is deliberately
// small: this is grain, not pattern.
inline constexpr f32 kVariationColour = 0.05f;

// The seed of the chain at a voxel. Must match `variation_root_hash` in shaders/variation.glsl.
//
// Truncated to 32 bits per axis, the same way `entry_hash32` truncates: the shader works in 32-bit
// integers throughout, and two voxels 2^32 apart getting the same grain is not a defect anybody
// will ever be in a position to see.
constexpr u32 variation_root_hash(i64 x, i64 y, i64 z, u32 seed) {
    u32 h = node_hash_mix(0x9E3779B9u ^ seed);
    h = node_hash_mix(h ^ static_cast<u32>(static_cast<i64>(x)));
    h = node_hash_mix(h ^ static_cast<u32>(static_cast<i64>(y)));
    h = node_hash_mix(h ^ static_cast<u32>(static_cast<i64>(z)));
    return h;
}

// One step down: this node's chain seed from its parent's, and which of the eight it is.
//
// The octant is folded in through an odd multiplier rather than XORed raw, so that the eight
// children of one parent do not share their low bits -- eight values differing only in three bits
// put through one mix are eight values that are still visibly related in the low byte, and the low
// byte is what the colour is read out of.
//
// Must match `variation_child_hash` in shaders/variation.glsl.
constexpr u32 variation_child_hash(u32 parent_hash, u32 octant) {
    return node_hash_mix(parent_hash ^ (0x85EBCA6Bu + octant * 0x9E3779B9u));
}

// The perturbation amount, quantised to 1/256ths, so that everything below it is integer.
//
// This conversion is the ONLY floating-point step in the source and it happens once, at `create`,
// from a knob a human wrote. `shaders/field_leaf.glsl` states the rule this follows and states it
// better than a restatement would: the one place not allowed to lose precision is the hashing,
// because a cell that has lost a low bit is not a slightly wrong grain, it is a different grain on
// one machine and not the other. Two floating-point pipelines that agree to within an ulp are two
// pipelines that disagree about a rounded byte a few times in a thousand -- and a few times in a
// thousand over a wall is a wall that shimmers on one computer.
constexpr u32 variation_amount_q(f32 amount) {
    if (!(amount > 0.0f)) return 0u;            // also the NaN guard
    const f32 scaled = amount * 256.0f + 0.5f;
    return (scaled >= 256.0f) ? 256u : static_cast<u32>(scaled);
}

// Eight bits of a hash. `which` picks the byte: 0 red, 1 green, 2 blue, 3 spare.
constexpr u32 variation_byte(u32 h, u32 which) { return (h >> (which * 8u)) & 0xFFu; }

// One channel nudged and clamped, in integer arithmetic from end to end.
//
// The byte is read as a signed offset in [-128, 127] and scaled by `amount_q / 256` of full scale:
// `delta = (bits - 128) * amount_q * 255 / (128 * 256)`, which is one multiply and a shift of 15.
// The magnitude is shifted rather than the signed value, because a right shift of a negative
// integer is implementation-dependent in GLSL and defined in C++20 -- so the one form that is the
// same on both sides is the one that never shifts a negative.
//
// Must match `variation_nudge` in shaders/variation.glsl.
constexpr u32 variation_nudge(u32 base, u32 bits, u32 amount_q) {
    const i32 offset = static_cast<i32>(bits) - 128;
    const u32 magnitude = static_cast<u32>(offset < 0 ? -offset : offset);
    const i32 scaled = static_cast<i32>((magnitude * amount_q * 255u) >> 15u);
    const i32 moved = static_cast<i32>(base) + ((offset < 0) ? -scaled : scaled);
    if (moved < 0) return 0u;
    if (moved > 255) return 255u;
    return static_cast<u32>(moved);
}

// A child's colour from its parent's, perturbed by the child's own chain seed.
//
// Alpha is carried through untouched. Alpha is COVERAGE (D139), not opacity, and a solid child of
// a solid parent covers exactly what its parent covered -- inventing a new coverage here would be
// this source having an opinion about shape, which is the thing it does not have.
//
// Must match `variation_child_colour` in shaders/variation.glsl.
constexpr u32 variation_child_colour(u32 parent_colour, u32 child_hash, u32 amount_q) {
    const u32 r = variation_nudge(parent_colour & 0xFFu, variation_byte(child_hash, 0), amount_q);
    const u32 g =
        variation_nudge((parent_colour >> 8) & 0xFFu, variation_byte(child_hash, 1), amount_q);
    const u32 b =
        variation_nudge((parent_colour >> 16) & 0xFFu, variation_byte(child_hash, 2), amount_q);
    return (parent_colour & 0xFF000000u) | (b << 16) | (g << 8) | r;
}

// The eight children of a node, derived and not stored.
//
// `mask` is the parent's own presence spread over all eight, for the reason above: this source
// varies the material and never the shape. It is a field rather than a constant 0xFF so that a
// later source -- R11c's field, which genuinely does know the shape -- can fill the same struct.
struct VariationChildren {
    u32 mask = 0;
    u32 colour[8]{};
    u32 hash[8]{};
};

// Must match `variation_children` in shaders/variation.glsl.
constexpr VariationChildren variation_children(u32 parent_hash, u32 parent_colour, bool parent_has,
                                               u32 amount_q) {
    VariationChildren out{};
    out.mask = parent_has ? 0xFFu : 0u;
    for (u32 octant = 0; octant < 8; ++octant) {
        out.hash[octant] = variation_child_hash(parent_hash, octant);
        out.colour[octant] =
            parent_has ? variation_child_colour(parent_colour, out.hash[octant], amount_q) : 0u;
    }
    return out;
}

// What a walk down the hashed source found, at whatever sub-voxel depth it was asked for.
//
// `depth` is levels BELOW a voxel: 0 is the voxel itself, 1 is a cell 1.5625 cm across, k is
// 3.125/2^k cm.
//
// **R8a has landed and this comment used to promise what happens then**: it said `depth` was a plain
// count rather than a signed level only because there was no signed level to put it in, and that
// when R8a arrived it would become `-level`. It has, and `signed_level()` below is that -- the count
// is KEPT as the field because it is what the chain walk counts down, and the level is what the key,
// the descent and the face all speak. One quantity, two spellings, and the conversion in one place
// rather than a `-` written out at each call site.
struct VariationSample {
    bool matter = false;
    VoxelTypeId type = kAir;
    u32 colour = 0;    // rgba8, alpha carried down from the voxel's own coverage
    u32 hash = 0;      // this cell's chain seed, which is what its own children come from
    u32 depth = 0;
    constexpr i32 signed_level() const { return -static_cast<i32>(depth); }
};

// ---- R8e: `--infinite-detail`, off by default --------------------------------------------------
//
// The mode that unclamps. It ships OFF and this is where that is written down, as a compile-time
// constant rather than as a literal in the budget, because `src/app/main.cpp` does not carry the
// flag yet -- the integrator owns that file and the hunk is in this change's report. Until it does,
// this constant is the only way in, and it is defaulted to the behaviour of every build before R8a
// so that a tree with this change in it draws exactly what a tree without it drew.
//
// The gate R8e is judged on is three sentences, from `21-renderer-rewrite.md`:
//
//   standing 10 cm from a wall costs within 30% of standing 2 m from it;
//   resident bytes stay bounded by resolution;
//   a carved sub-voxel edit survives a save and reload.
//
// The second holds by construction and `test_node_pool.cpp` pins it: nothing below a brick is ever
// stored, so descending to level -8 over a box of world moves `stats().total_bytes` by nought. The
// first is a ratio of two timings and cannot be settled on a machine running seven agents at once.
// The third needs `world/serialize.*`, which this change does not own and does not touch.
inline constexpr bool kInfiniteDetailDefault = false;

// What a descent BELOW a voxel found. R8a's third quantity, after the key and the level.
//
// It is not a `NodeFind`, and the difference is the point rather than an inconvenience. `NodeFind`
// answers about a SLOT -- "here is the record, at this level" -- and there is no record below a
// brick to answer with. This answers about CONTENT: what a ray arriving at that cell would draw,
// derived on the spot from the voxel the cell is in and the chain of octants down to it.
//
// `derived` says which of the two happened, and it is separate from `matter` for the reason trap 7
// gives at every level of this engine: "nothing is here" and "I could not go there" must never be
// the same answer. With `--infinite-detail` off, a sub-voxel key answers `derived = false` and the
// content of its VOXEL, which is a stand-in and exactly what a build without this mode draws.
struct SubVoxelFind {
    bool matter = false;
    bool derived = false;
    i32 level = 0;          // the level answered at, which is the level asked for
    VoxelTypeId type = kAir;
    u32 colour = 0;         // rgba8
    u32 hash = 0;           // this cell's chain seed, which is what its own children come from
};

// What the GPU sees. Thirty-two bytes, one cache line per two nodes.
struct GpuNode {
    // The node's own coordinate at its own level, truncated to 32 bits. The shader works in
    // 32-bit integers throughout; these exist so a hash probe can confirm it found the node it
    // asked for. A false match needs two nodes 2^32 apart at the same level, which at level 14
    // is 137 billion kilometres.
    i32 x = 0;
    i32 y = 0;
    i32 z = 0;

    // level | flags << 8 | child_mask << 16
    //
    // The child mask is the empty-space test and the reason a descent costs no memory traffic
    // for the space it skips: one byte says which of the eight children exist, so seven of them
    // can be dismissed without ever being fetched.
    u32 packed = 0;

    // Two meanings, told apart by kNodeLeaf, because a node has only ever one of them and
    // thirty-two bytes is two nodes to a cache line.
    //
    //   interior   the base slot of this node's eight children, contiguous
    //   leaf       an index into the leaf array, where this brick's header and occupancy live
    //
    // A leaf has no children by definition, so nothing is lost and nothing has to be kept in
    // step. What it does need is a guard on every descent: walking into a leaf's `children`
    // would treat a payload offset as a slot index.
    u32 children = kNoNode;

    // rgba8, filtered over the subtree. Alpha is coverage.
    //
    // Coverage never rounds to nothing (D139): a node holding one solid voxel reports "present,
    // however faintly". Without that floor a railing or a wire does not look thin at a distance,
    // it vanishes — at exactly the range where you can no longer see well enough to notice.
    u32 colour = 0;

    // How much of this node is matter as seen ALONG each of the six face directions, one byte
    // each, packed +x -x +y -y in the first word and +z -z in the low half of the second.
    //
    // This is the quantity Stage 4 needed and did not have. It tried volumetric fill fraction as
    // an alpha and distant window frames dithered into the sky, because a brick on the surface of
    // the ground is about an eighth full and completely opaque when you look at it. Fill fraction
    // and screen coverage coincide only for a node seen edge-on. Folded per direction at build
    // time, this is exact, and it is what lets the composite anti-alias an edge analytically
    // rather than with a temporal filter.
    u32 coverage_xy = 0;
    // +z and -z, one byte each, and nothing else.
    //
    // The first version stashed the brick's encoding in the top half of this word to avoid a
    // second array. It collided with the -z byte, so `index_bits` read back as a coverage value
    // and decode_voxel divided by it — a crash rather than a wrong picture, which was luck. A
    // leaf now carries a real GpuBrickHeader in its own array: the layout the renderer has used
    // since Stage 2, already tested against the world.
    u32 coverage_z = 0;
};
static_assert(sizeof(GpuNode) == 32, "GpuNode must stay 32 bytes");

inline constexpr u32 kNodeLeaf = 1u << 0;        // a brick; `payload` and occupancy are valid
inline constexpr u32 kNodeUniform = 1u << 1;     // the whole subtree is one voxel type
inline constexpr u32 kNodeEmissive = 1u << 2;    // something under here emits light
inline constexpr u32 kNodeTransmissive = 1u << 3;   // something under here lets light through

// R12c's other half: this node's child mask came from the CLIP and not from the world.
//
// It is here rather than in a side table because the one question anybody will ask of a seeded
// node is "why does this mask disagree with `world_has`", and the answer has to travel with the
// record that disagrees. `stale_masks` reads it to leave those nodes alone, `mirror_seed` reads it
// to find them, and `retire_clip_seed` clears it.
//
// The shader never tests it. `node_flags_of` is read in exactly one place in shaders/node.glsl --
// the kNodeLeaf test -- so a fifth bit is inert on the card, and the card is where the seeding has
// to be visible for a descent to answer WANTED at all.
inline constexpr u32 kNodeSeeded = 1u << 4;

// The raw level byte, and NOUGHT IS THE SENTINEL: a free slot is `GpuNode{}` and reads as level
// nought, which is this pool's spelling of "the world has this and I have not built it". Every
// caller that tests `node_level(n) == 0` is asking whether the slot is live, and R8a leaves all of
// them alone -- see `world/level.hpp` for why that sentinel is still safe with signed levels, and it
// is one sentence: the pool stores no node finer than a brick, in either arm.
constexpr u32 node_level(const GpuNode& n) { return n.packed & 0xFFu; }
// ...and the same byte read as a signed level. Defensive rather than used by the pool -- no record
// it writes ever carries a level below three -- and here so that a reader of this file does not have
// to work out from the packing whether it could.
constexpr i32 node_level_signed(const GpuNode& n) { return level_from_byte(n.packed); }
constexpr u32 node_flags(const GpuNode& n) { return (n.packed >> 8) & 0xFFu; }
constexpr u32 node_child_mask(const GpuNode& n) { return (n.packed >> 16) & 0xFFu; }
constexpr u32 pack_node(u32 level, u32 flags, u32 child_mask) {
    return (level & 0xFFu) | ((flags & 0xFFu) << 8) | ((child_mask & 0xFFu) << 16);
}

// Where a descent stopped, and why.
//
// Three answers, not two, and the difference between them is what streaming is made of:
//
//   empty_below   the child cell one level down holds nothing. A ray jumps the width of it, and
//                 that jump is the coarse skip — it falls out of the descent instead of needing
//                 five separate occupancy grids to carry it.
//   wanted        the world has something here and the pool does not. A ray reports it, which is
//                 the only reason anything is ever streamed.
//   otherwise     `slot` is the finest node covering the point, at `level`.
//
// Conflating the first two is the fault this whole structure exists to make unrepresentable: an
// unstreamed building that reads as open sky is never requested, so it stays open sky.
struct NodeFind {
    u32 slot = kNoNode;
    u32 level = 0;
    bool empty_below = false;
    bool wanted = false;
};

// ---- R12c's other half: the clip's extent, before anything has been sampled or pasted ----------
//
// R12c (D699) makes a descent that reaches a node the pool has not built EVALUATE the field there
// and draw it. It works from the moment the ladder pastes anything, and it cannot touch the FIRST
// frame of a cold world, for a reason that is entirely this file's:
//
//     frame 1 holds 0 chunks -> `index_world` finds nothing -> the pool seeds no roots ->
//     `node_locate` answers kFoundEmpty rather than kFoundWanted -> there is nothing to derive.
//
// D673 shipped R11d with the coarse paste OFF (`no_coarse_paste` defaults true), so this is not an
// edge case: on every cold load the world really is empty for the frames before the ladder's first
// batch lands, and the screen really is sky. The marcher already knows how to draw the building
// there. It is only never asked.
//
// So the pool is given its roots from the CLIP, which knows where the building is before anything
// has sampled it.
//
// # What is seeded is the ADDRESSING, and that distinction is the whole safety argument
//
// A root is not a licence to allocate. What goes in is a chain of SHELLS -- a coordinate, a level
// and a child mask, thirty-two bytes each -- from kEntryLevel down to `kClipSeedCellLevel`, and
// nothing else. No leaf, no occupancy word, no payload byte, and no descent below the floor. The
// pool's budget, its eviction policy and its residency argument (R2) are untouched, because a shell
// is exactly what the pool would hold anyway the first time a ray asked about that block; the seed
// only asks earlier and from a different source.
//
// # And the mask is the clip's, which means it does NOT agree with the world
//
// That is deliberate and it is the one dangerous thing here, so it is stated rather than hidden.
// D621 is what happens when the render tree believes matter is somewhere the pool cannot build:
// 304 lumps on screen and three separate audits agreeing perfectly with all of them, because every
// one was downstream of the reader that was wrong. The defences are three:
//
//   * every seeded node carries `kNodeSeeded`, so a mask that disagrees with `world_has` says so
//     in its own record rather than being inferred;
//   * `mirror_seed` is an independent walker: it reads the pool's records, asks the FIELD through
//     the same oracle the seed was built from and asks the WORLD through `world_has`, and reports
//     the three facts separately -- agrees with the field, exceeds the world, misses the world.
//     The claim it establishes is that the seed never claims LESS than the world has and never
//     claims MORE than the field allows;
//   * the seed RETIRES. It is in force only while the world holds no chunks at all, and the first
//     `update` that sees one drops the seeded subtree, re-derives every seeded root's mask from the
//     world, and clears the flag -- after which the pool is byte-for-byte the pool the control arm
//     would have had. So a mask that exceeds the world cannot outlive the world being empty, which
//     is what stops this being D133 wearing a new hat.
//
// The narrowness is the point and it is not an oversight: this serves the window before the world
// exists, and hands over to D699's own mechanism the moment it does. Carrying the clip's mask into
// a PARTIALLY built world needs a per-octant rule and is a separate change.
//
// # Where the floor is, and why it is not the leaf
//
// The seed's finest statement is "this 2^kClipSeedCellLevel cell may hold matter". Below that the
// marcher derives. The floor is a trade between two costs that both grow eightfold a level: the
// oracle is one field evaluation per candidate box, and a floor at the brick would be millions of
// them on the estate (D686 -- there is no grain that is instant). What the floor buys is DERIVATION
// pruned: a ray crossing a cell the clip says is empty jumps it analytically instead of paying a
// field evaluation for it, and on frame 1 of a cold world every pixel is a candidate, so that is
// the difference between a budget spent on sky and a budget spent on the building.
//
// **Level 8, and the reason it is not finer is a measurement rather than a budget.** On the estate
// `forge::box_may_hold_matter` answers "may hold" for EVERY box inside the clip's own bounds, at 8 m,
// 2 m, 1 m and 0.5 m alike -- 1,275 of 1,275, then 68,096 of 68,096, then 531,468 of 531,468, then
// 4,160,325 of 4,160,325. It is not broken: the prune root's distance up the middle of the clip runs
// 1.30 m at the ground to **17.29 m at the top of the extent**, and a 4 m half-box there still
// answers "may", so the test is carrying about ten metres of slack -- which is D644 and D646's
// finding arriving from the other side. Ten metres of slack cannot narrow a seed to the quarter of a
// metre the marcher marches.
//
// So the only pruning that works here is the EXTENT ITSELF, and once the shells resolve the clip's
// box, finer buys nothing: measured, the marcher derived 1,526 cells at the peak with an 8 m floor
// and 1,407 with a 2 m one, for 11,153 shells instead of 308. Level 4 would be 610,717 shells --
// twenty megabytes of addressing, which is no longer addressing.
//
// Level 8 is 256 voxels, which is what used to be a chunk and is the grain the ladder's coarsest
// node is cut at. On the estate it is 308 shells and 1,275 field questions in 0.1 ms against a load
// that is 297 ms to playable. `--derive-seed-level N` moves it, and nothing depends on its value.
inline constexpr u32 kClipSeedCellLevel = 8;

// "May this box of absolute voxels hold matter?" Supplied by the caller, because the answer lives
// in `forge` and this file must not grow an opinion about the field.
//
// It must be CONSERVATIVE in one direction only: answering true over emptiness costs a derivation,
// and answering false over matter loses a building. `forge::box_may_hold_matter` is written to that
// standard and is what main.cpp passes.
//
// Both corners are inclusive, in absolute voxels, and already clipped to the extent.
using NodeSeedOracle = std::function<bool(const i64 lo[3], const i64 hi[3])>;

// What the seed cost and what it put in, so a run's log can say it rather than imply it.
struct NodeSeedReport {
    u32 roots = 0;        // entry-level shells placed in the table
    u32 shells = 0;       // every shell, roots included
    u64 asked = 0;        // oracle calls
    u32 cells = 0;        // mask bits set at kClipSeedCellLevel -- what a ray may derive into
    bool live = false;    // false when nothing was seeded, or when the seed has already retired
    f64 ms = 0.0;
};

// The independent walker. Three facts kept apart, because a single "agrees" would be the shape of
// audit D621 warns about.
struct NodeSeedMirror {
    u32 shells = 0;          // seeded nodes walked, from the pool's own records
    u32 bits = 0;            // mask bits set across them
    u32 agrees_field = 0;    // ...of which the oracle also says may hold matter
    u32 differs_field = 0;   // ...and of which it does not. MUST be nought: the seed invented one
    u32 exceeds_world = 0;   // bits the world has nothing under. Expected -- it is the whole point
    u32 misses_world = 0;    // the world HAS it and the mask does not. MUST be nought
    u32 leaves = 0;          // leaves under a seeded shell. MUST be nought -- no storage was seeded
    u32 children = 0;        // built children under a seeded shell that are not themselves seeded
};

struct NodePoolBudget {
    u32 max_nodes = 1u << 20;                       // 1M nodes, 32 MB
    u64 payload_bytes = 512ull * 1024 * 1024;
    u32 max_occupancy_leaves = 1u << 18;            // 64 B each, 16 MB

    // Per-frame work caps. Streaming is allowed to fall behind — a node that does not arrive
    // this frame arrives next, and the renderer draws its parent in the meantime. What it is
    // never allowed to do is blow the frame budget (documentation/09 §2: 0.8 ms on a Deck).
    // Raised from 2048 once a request meant a whole path rather than one level of one. A frame
    // that builds more converges sooner, and the cap bounds a spike rather than rationing
    // steady-state work - which is nil, because a converged tree builds nothing at all.
    // A count, not a time, and that is worth knowing before tuning it.
    //
    // It was lowered to 2,048 and then 4,096 chasing a 27 ms worst frame, which moved it by four
    // milliseconds and then by nothing -- because the worst frame turned out to be frame SIXTEEN,
    // during load, and lowering this only slowed the fill-in that a player sees. Restored.
    u32 max_builds_per_frame = 16384;
    u64 max_upload_bytes_per_frame = 8ull * 1024 * 1024;

    // How long a node may go unwanted before its slot can be taken, in frames. Several seconds
    // at any frame rate: a node just off screen has to survive turning round and back.
    u32 cold_frames = 600;

    // Held resident regardless of visibility, at full detail, because collision, physics and
    // editing have to touch what is behind you and under your feet (D199). Twenty metres.
    i64 proximity_voxels = 20 * 32;
    // How much of the proximity sweep is done per frame. Twenty metres is eighty bricks, so the
    // volume is millions of cells even though only the ones the world holds are ever requested,
    // and walking it in one frame is a stall rather than a guarantee.
    // Restored, for the same reason as the build budget above: it was cut to a quarter to make
    // a spike smaller and the spike did not move, because it was a startup frame rather than
    // anything the sweep did. A quarter of the slice is four times as long to reach twenty
    // metres, and that is a real cost paid for nothing.
    u32 proximity_per_frame = 32768;

    // ---- R2b's second half, and why it ships OFF -----------------------------------------------
    //
    // On is the policy; off is the behaviour of every build before this one, so the two arms are
    // one binary and one flag (D407) rather than two builds that differ by a rebuild as well.
    //
    // **It defaults to OFF, and the reason is a measurement rather than a doubt about the rule.**
    // The rule is right and the tests pin it against the ladder at every metre from 1 to 4000. It
    // simply does not fire on this scene: at 1280x800 the marcher addresses a LEAF out to 250 m,
    // and nothing on either measured camera is that far away.
    //
    //   enclosed `0,0,0,-90,0`, warm cache and again on a cold --no-clip-cache load:
    //       0 refused, 0 given up, and the independent witness reads 0 sub-pixel nodes held on
    //       BOTH arms, every 600 frames, from the first frame to the last.
    //   outdoor `0,10,-60,90,-6`: the same, 0 throughout, to frame 20,400.
    //   a camera 400 m out, which is the nearest one where it fires at all: the control arm held
    //       6,837 sub-pixel leaves and 1,119,960 bytes -- 55% of the 2.05 MB the screen was paying
    //       for -- and this arm held none. The estate is ten pixels across at that range.
    //
    // And it is not free. The erosion sweep can answer "is this cold" from a four-byte timestamp
    // and cannot answer "where is this" from anything but the 32-byte record, so switching it on
    // costs the sweep its short-circuit: measured 0.187 and 0.214 ms of pool CPU against a
    // control's 0.067, on a frame whose whole node pass has a 0.80 ms budget. Inside the budget,
    // three times the line, nothing bought.
    //
    // So: kept, pinned and dormant, rather than deleted. Turn it on for a world large enough to
    // have a quarter of a kilometre in it, and turn it on at 4K only after `NodeView::pixel_angle`
    // is wired -- see `pixel_angle` below for which direction that error runs in.
    //
    // **The witness is worth more than the policy right now, and it is not gated on this flag.**
    // D621 left "the load's peak leaf demand is eight times the settled demand" undiagnosed. It
    // reads 0 through an entire cold load, on the control arm, which rules out one whole class of
    // answer: whatever the pool is holding eight times too much of, it is not detail finer than the
    // pixel.
    //
    // Nothing in `main.cpp` sets this yet -- the integrator owns that wiring -- so it is also
    // readable from the environment as `WS_SUBPIXEL=1`, which is what the measurements above were
    // taken with. That is a stopgap and it says so: it belongs on the command line beside
    // `--no-paste-drop`, where a run's arm is visible in the run's own log.
    bool subpixel_rule = false;

    // The angle one pixel subtends. Overridden per frame by `NodeView::pixel_angle` when the caller
    // fills it in; this is what the pool falls back to, and what the tests use.
    //
    // The fallback has been described as "the 1280x800 figure" since it was written and it is not
    // one. It is a RESOLUTION, and the arithmetic is one line: the angle is `2*tan(fov/2)/lines`,
    // the lens is 90 degrees so the tangent is exactly 1, and `kSubPixelAngle` is 0.002 -- so
    // 0.002 rad IS ONE THOUSAND LINES. Not 1280, and not 800.
    //
    // The direction of the error is the one that matters: a HIGHER resolution has a smaller pixel
    // angle, so rays there address finer nodes than this constant admits, and a pool trusting it
    // would evict what they are looking at. So it is safe at or below **1000** lines and unsafe
    // above them -- which is why `NodeView` carries the real one and why this is only the default.
    //
    // Measured over every metre from 1 to 5,000 (`test_node_pool_evict.cpp`): against 4K's true
    // 0.000926 rad the constant is 2.160x too coarse, the two first part company at **250 m**,
    // they disagree at **4,070 of the first 5,000 metres**, and they are two whole levels apart
    // between 500-540 m, 1000-1080 and 2000-2160. Every disagreement has the constant too COARSE;
    // none has it too fine. The shipped 1280x800 is 800 LINES and so has always been on the safe
    // side of it -- the figure in this comment was wrong by 28% and the behaviour never was.
    f64 pixel_angle = kSubPixelAngle;

    // How many levels finer than the marcher's own target the pool may still keep. Nought is the
    // rule exactly; one is a level of slack for measuring what the rule is worth without changing
    // its shape. Not a tuning knob -- a way to price the margin.
    u32 subpixel_margin_levels = 0;

    // ---- R8b: the hashed child source ----------------------------------------------------------
    //
    // Off by default, and off is the arm every build before this one ran. It changes nothing that
    // is drawn while it is off and nothing that is drawn while it is on either, until R8a's signed
    // levels and the `node.glsl` call site land -- what it does today is derive, mirror and
    // fingerprint, headless, so that the renderer is walking a structure something has already
    // compared against the world. That order is Stage 2's and the reason for it is that a
    // structure the renderer walks and nobody compares against the world is a renderer debugging a
    // mirage.
    bool hashed_variation = false;
    u32 variation_seed = kVariationSeed;
    f32 variation_colour = kVariationColour;

    // ---- R8e: `--infinite-detail`, and it ships OFF --------------------------------------------
    //
    // Off is the arm every build before this one ran, so leaving the flag out IS the control and the
    // two arms are one binary (D407). It defaults from `kInfiniteDetailDefault` rather than from a
    // literal so that a reader who wants to know which way it ships has one place to look.
    //
    // **What it changes, exactly.** The clamp at `max(level, kLeafLevel)` comes off the descent
    // TARGET: `marcher_finest_for` answers a negative level, `locate_below` answers what is there,
    // and a face may be claimed at one. What it does NOT change is what the pool stores — no record
    // finer than a brick is written in either arm, which is why R8e's second gate ("resident bytes
    // stay bounded by resolution") holds by construction here rather than by a policy that could be
    // wrong. Everything below a voxel is derived by R8b's child source and costs nothing.
    //
    // **And it changes nothing on screen until the shader has it.** The descent that would DRAW a
    // sub-voxel cell is `node_march` in `shaders/node.glsl`, which clamps its own target the same
    // way and is a file this stage was not allowed to edit. So with the flag on today the picture is
    // identical and the CPU answers a question nothing is yet asking — which is Stage 2's order and
    // R8b's own: the structure is compared against the world before the renderer walks it.
    bool infinite_detail = kInfiniteDetailDefault;
};

// What changed this frame and must be copied to the GPU.
struct NodeUploadBatch {
    std::vector<u32> nodes;        // node slots whose record changed
    std::vector<u32> leaves;       // leaf slots whose occupancy changed
    std::vector<u32> payload_from; // payload byte ranges that changed
    std::vector<u32> payload_size;
    u64 payload_bytes = 0;
    u32 built = 0;
    u32 evicted = 0;
    // Eviction EVENTS above -- one per slot the erosion sweep took and one per root that shed --
    // and nodes below. A root sheds a whole subtree through one event, so the two are not the same
    // number and must not be compared with each other. The two below are commensurate with one
    // another, and with `NodePoolStats::evictions`, which counts nodes.
    u32 evicted_nodes = 0;
    // ...of which this many were inside the view frustum when they went.
    //
    // The question eviction cannot answer for itself. `node_last_read_` decides what is cold, it
    // is stamped from feedback, and feedback is the thing under suspicion -- so a count taken from
    // it would agree with itself no matter how wrong it was. The frustum is an independent
    // witness: it is built from the same four vectors the marcher builds its rays from, and it
    // knows nothing about what any ray reported. See D426.
    u32 evicted_on_screen = 0;
    // Nodes requested within kChurnWindow frames of this pool evicting them. The harm itself,
    // rather than a proxy for it.
    u32 churned = 0;
    u32 deferred = 0;              // wanted, but the frame's budget ran out
    // Wanted, and the pool had nowhere to put it: a node slot, a run of eight, an occupancy leaf
    // or a stretch of payload. Distinct from `deferred`, which is a budget this frame and is
    // served on the next one, and distinct again from a cell the world is empty at.
    //
    // It exists because `out_of_memory` was set in exactly one place -- the entry-level shell --
    // so a pool jammed at its leaf ceiling reported `deferred 0` and counted every failed refine
    // as `built`. That is trap 7 living in the instrument: "I could not fit it" arriving as
    // "here you are". D621.
    u32 no_room = 0;
    // R2b: requests the pool declined to descend into because the node asked for is finer than any
    // ray at that distance can address, and evictions taken for the same reason rather than for age.
    //
    // Separate from `deferred` and `no_room` on purpose, and it is the same distinction those two
    // were split for. "I ran out of budget", "I had nowhere to put it" and "nothing will ever look
    // at this" are three different facts about a frame, and a pool that reports the third as either
    // of the first two reads as jammed while it is working exactly as designed.
    u32 refused_subpixel = 0;
    u32 evicted_subpixel = 0;
    bool out_of_memory = false;
    void clear();
};

// Where the camera is and what it can see, in absolute voxel coordinates.
//
// Passed in rather than derived, because the pool must not grow a second opinion about where the
// camera is pointing: these are the same vectors that go into the parameter block the marcher
// reads, so the frustum this describes is the frustum the rays actually swept. Trap 13 is what
// happens when two structures answer one question.
struct NodeView {
    f64 origin[3] = {0.0, 0.0, 0.0};
    f32 forward[3] = {0.0f, 0.0f, 1.0f};
    f32 right[3] = {1.0f, 0.0f, 0.0f};
    f32 up[3] = {0.0f, 1.0f, 0.0f};
    f32 tan_half_fov = 0.0f;   // lens.x, the vertical half-angle
    f32 aspect = 1.0f;         // width / height, which is how the marcher widens it
    bool valid = false;        // false disables the test rather than making it always true

    // What one pixel subtends, for R2b's rule and for nothing else. Nought means "not told", and
    // the pool falls back to `NodePoolBudget::pixel_angle`.
    //
    // It is a field rather than something derived from `tan_half_fov` because the marcher's own
    // expression carries two things this struct does not have and must not grow a second opinion
    // about -- the render height and the detail bias:
    //
    //     pixel_angle = 2.0 * push.lens.x / push.resolution.y      (visibility.comp)
    //     footprint   = t * pixel_angle * push.lens.z              (node.glsl, lens.z is the bias)
    //
    // so the value to put here is `2 * tan_half_fov / render_height * detail_bias`. Trap 13 is what
    // happens when two structures answer one question, and the answer to "how big is a pixel" has
    // to come from the same place the rays got it.
    f32 pixel_angle = 0.0f;
};

struct NodePoolStats {
    u32 nodes = 0;
    u32 leaves = 0;
    u64 payload_in_use = 0;
    u64 payload_capacity = 0;
    u64 node_bytes = 0;
    u64 occupancy_bytes = 0;
    u64 total_bytes = 0;
    // What the screen is actually paying for: everything except the entry table, which is sized
    // once from the budget and never changes.
    //
    // R2b's rule is about resident bytes following resolution, and the table is a fixed
    // 1,048,576 bytes at the default budget -- on a distant camera that is 95% of `total_bytes`,
    // so the total moved 3% while the thing the rule is about moved by 3.4x. A gate measured
    // against a number with a megabyte constant in it cannot be met by any amount of eviction.
    u64 screen_bytes = 0;
    u64 builds = 0;
    u64 evictions = 0;
    // Over the run: how many evictions took a node that was inside the view, and how many nodes
    // came straight back. Lifetime rather than per frame, because a settled camera evicts a
    // handful a frame and a single frame of either is noise.
    u64 evictions_on_screen = 0;
    u64 churn = 0;
    // Which kind of request brought each churned node back.
    u64 churn_by_source[kRequestSourceCount]{};
    // How full the bricks involved are, out of 512 voxels, and the same figure over every leaf
    // the pool is holding. This is what tells the two candidate causes apart: a brick a ray
    // STOPS on is a wall and is nearly full, while a brick a ray passes THROUGH on its way to
    // one is mostly air. If the leaves coming back are much emptier than the leaves the pool
    // holds, the signal that was lost is "a ray read this", not "a ray was sampled here".
    f64 churn_fill = 0.0;
    f64 evicted_fill = 0.0;
    f64 resident_fill = 0.0;
    // ...and how many of them no ray had EVER reported reading. The other discriminator, and the
    // sharper one: a node that was read and went quiet is a sampling problem, and a node no ray
    // ever reported is a reporting problem.
    u64 evictions_never_read = 0;
    u64 churn_never_read = 0;
    // What level the churned nodes were at. A leaf coming back is a brick flickering; a level-8
    // node coming back is eight metres of building doing it.
    u32 churn_per_level[32]{};
    // Resident nodes by level, which is the shape a total cannot show.
    //
    // R2b's rule is that halving the resolution moves every ray's stopping point exactly one
    // level coarser, so the histogram should shift by one and lose three quarters of its finest
    // level. A single number says memory fell by 22% and cannot say which levels did not move.
    u32 per_level[32]{};

    // ---- R2b's second half, and the one number that is not taken from the policy ---------------
    //
    // The first two are counters the rule increments as it works, and they are worth exactly what
    // trap 26 says such counters are worth: they agree with the rule because they ARE the rule. A
    // pool whose distance test is wrong reports refusals and evictions just as briskly.
    //
    // `subpixel_resident` is the independent witness. It is computed in `stats()` by walking every
    // slot in the pool and asking the rule about the node that is actually there -- a different
    // reader, over a different set (the whole array, not the erosion slice), at a different time
    // (an audit, not a frame). With the rule ON it must fall to nothing and STAY there; with the
    // rule OFF it is the size of the prize, measured on a build that is not taking it. Those two
    // readings are the measurement, and neither of them is a timing figure.
    u64 subpixel_refused = 0;
    u64 subpixel_evicted = 0;
    u32 subpixel_resident = 0;
    // ...and what they occupy: 32 bytes a node, plus the occupancy and payload of any that are
    // leaves. Bytes rather than a count because a leaf costs a hundred times what an interior node
    // does, and R2b's claim is about bytes.
    u64 subpixel_bytes = 0;
    // Which levels they are at. A pool holding sub-pixel LEAVES is holding bricks nobody can
    // resolve; a pool holding sub-pixel interior nodes is holding 32-byte records, which is a
    // different and much smaller problem wearing the same name.
    u32 subpixel_per_level[32]{};
    // Which arm the run was taken on, printed beside the figures so a table of numbers cannot be
    // read without it. D621's leaf-ceiling table is the argument for this: three rows of counters
    // that mean opposite things depending on a flag nothing recorded.
    bool subpixel_rule = false;

    u64 requests = 0;
    u64 hits = 0;
    f64 hit_rate() const {
        return (requests > 0) ? static_cast<f64>(hits) / static_cast<f64>(requests) : 0.0;
    }
};

class NodePool {
public:
    void create(const NodePoolBudget& budget, const VoxelTypeTable& types);

    // "A ray wanted this node and could not find it." The only reason anything is ever built.
    //
    // A node smaller than a pixel is never requested, because the descent stops at the pixel
    // footprint — so it is never fetched, never uploaded, and does not exist (D190).
    void request(const NodeKey& key, u8 source = kRequestRay);

    // A ray READ this node, so it is wanted whether or not anything is missing under it.
    //
    // Without this, "wanted" only ever meant "missing", and a finished tree stopped being wanted
    // by everything at once. See D247 and the note over the eviction loop.
    void touch(const NodeKey& key);

    // The same, for a node the marcher can name outright.
    //
    // A ray knows the SLOT it stopped on -- it descended to it -- so it reports that rather than a
    // key, and this is a store instead of a descent. Per node rather than per root, because
    // eviction at the root can only drop the scene or nothing (D260).
    void touch_slot(u32 slot);

    // "The copy you are holding is out of date." Pushed by whatever edited the world, and
    // remembered until it is served.
    //
    // This cannot be pulled. Feedback reports nodes the renderer wanted and *could not find*; a
    // node that is resident but stale is found, so it is never reported, so it is never
    // refreshed. Only the code that changed it knows. A one-frame request is not enough either:
    // the frame budget serves a bounded number and a large edit touches far more, so all but the
    // first few would be stranded, drawing pre-edit contents until something unrelated evicted
    // them (D131).
    void invalidate(i64 x, i64 y, i64 z);

    // The same thing for a REGION, and the difference is not convenience (D515).
    //
    // Per brick, the caller has to enumerate every brick in the box, and the pool then has to
    // deduplicate and descend to each one. A 36-million-voxel delete announces **1,573,269**
    // bricks that way, of which the pool holds almost none — the answer it computed from that list
    // was 13,325 ancestors and *no* rebuilt leaves, and getting there cost **714 ms in one frame**:
    // 457 to put the keys in a set and 257 to walk them, against 4 ms of actual work.
    //
    // The pool knows what it holds and the caller does not. Given the box, it descends from its own
    // roots and prunes to it, so the cost is the number of built nodes the edit touches rather than
    // the number of bricks the box contains. Single-voxel edits are unaffected: one root, one
    // chain, the same eleven nodes as before.
    void invalidate_box(const i64 lo[3], const i64 hi[3]);

    // Serves this frame's requests, holds the proximity radius, evicts what has gone cold, and
    // returns what the GPU layer must copy.
    //
    // `view` is for the eviction instrument only and nothing in the policy reads it: a null view
    // costs the tests nothing and leaves every other answer identical.
    const NodeUploadBatch& update(const World& world, const f64 camera_voxel[3], u64 frame,
                                  const NodeView* view = nullptr);

    // The slot holding a node, or kNoNode. Walks the same path the shader walks: hash the entry
    // ancestor, then descend by octant.
    u32 find(const NodeKey& key) const;
    bool resident(const NodeKey& key) const { return find(key) != kNoNode; }

    // Where a descent stopped, and why — the three-way answer a ray needs.
    NodeFind locate(const NodeKey& key) const;

    // Reads one voxel the way the shader will — entry hash, descent, child mask, leaf payload —
    // so that "the GPU holds what the CPU holds" is a test rather than a hope. This is the
    // successor to mirror_voxel_world and is held to the same standard.
    VoxelTypeId mirror_voxel(i64 x, i64 y, i64 z) const;

    // How many built leaves hold a shape the world no longer has. The other half of the same
    // standard: `mirror_voxel` asks whether the pool agrees with itself, and this asks whether it
    // agrees with the WORLD.
    //
    // It exists because that question had no instrument and the answer was "thirty thousand". The
    // world is written by more than the edit path — a region paste sharpens a box of it in the
    // background — and a leaf is a copy taken at build time, so a writer that does not call
    // `invalidate` leaves the pool holding the shape it had before. Nothing anywhere said so: the
    // GPU mirror matched, the node count was healthy, and the picture was of a building that had
    // been sharpened everywhere except in the tree the renderer walks.
    //
    // Occupancy only, which is the cheap half and the half that carries a paste: comparing
    // payloads would mean re-encoding every brick. `first` receives the first disagreeing leaf's
    // key, because a count says there is a fault and a coordinate says where to look.
    u32 stale_leaves(const World& world, NodeKey* first = nullptr) const;

    // The same question one level up: how many built nodes carry a child mask the world disagrees
    // with. `stale_leaves` covers what a ray SEES; this covers what a ray is allowed to look for.
    //
    // A mask bit set over nothing is a ray reporting a node the world does not have, every frame,
    // for ever — D133 — and a bit clear over something is geometry that has become invisible and
    // that no feedback will ever ask for, because feedback reports what a ray could not find and a
    // ray never goes there. Both are silent: the GPU mirror agrees, the leaves agree, the node
    // count is healthy, and the fault is in the one field nothing compares.
    //
    // It had no instrument until the edit refresh was rewritten to descend the tree rather than
    // enumerate the box (D515), because that rewrite changes precisely which nodes get their mask
    // re-derived and nothing could have told a correct answer from a plausible one.
    u32 stale_masks(const World& world, NodeKey* first = nullptr) const;

    // How much of each array is in use, so the GPU copies a prefix rather than a capacity. An
    // empty world uploads nothing and the facility uploads a couple of megabytes, where copying
    // the whole pool would be 32 MB whatever it held.
    u32 node_watermark() const { return next_free_; }
    u32 leaf_watermark() const { return next_leaf_; }
    u64 payload_watermark() const { return payload_high_; }

    const std::vector<GpuNode>& nodes() const { return nodes_; }
    const std::vector<u64>& occupancy() const { return occupancy_; }
    const std::vector<GpuBrickHeader>& leaves() const { return leaves_; }
    const std::vector<u8>& payload() const { return payload_; }
    // The entry-level hash table, as the shader reads it: slot per bucket, kNoNode when empty.
    const std::vector<u32>& entries() const { return entries_; }

    // What changed since the last upload, so the GPU is sent that rather than every used byte.
    //
    // The prefixes above are what the first version copied, every frame anything changed at all.
    // That is fine while the tree is converged and quiet, and it is 10 MB a frame while somebody
    // is walking -- measured at 2.725 ms mean and 8.915 ms worst against a 0.80 ms budget, the
    // largest single cost in the frame and the reason the pool read as laggy while marching
    // faster than what it replaced.
    const DirtySet& dirty_nodes() const { return dirty_nodes_; }
    const DirtySet& dirty_leaves() const { return dirty_leaves_; }
    const DirtySet& dirty_entries() const { return dirty_entries_; }
    // Payload is bytes rather than records, and a block allocation is contiguous, so ranges are
    // recorded where they are written instead of through a bitmap over half a gigabyte.
    const std::vector<std::pair<u64, u64>>& dirty_payload() const { return dirty_payload_; }

    bool nothing_dirty() const {
        return dirty_nodes_.empty() && dirty_leaves_.empty() && dirty_entries_.empty() &&
               dirty_payload_.empty();
    }

    // Cleared by whoever uploads, and only once the copy is actually recorded. Held otherwise,
    // so a frame that ran out of staging retries rather than leaving the card a stale byte.
    void clear_dirty() {
        dirty_nodes_.clear();
        dirty_leaves_.clear();
        dirty_entries_.clear();
        dirty_payload_.clear();
    }

    // Everything in NodePoolStats that is a counter, and nothing that is a walk.
    //
    // `stats()` sweeps every node for the level histogram and popcounts every resident leaf for
    // the average fill -- 1.5 million popcounts on the facility -- and its own comment says it is
    // read once at an audit. It is, from the screenshot. The OVERLAY reads a report every frame,
    // and pointing that at `stats()` when chunk residency's cheap counters went away cost **4.5 ms
    // of CPU a frame** against a 4 ms frame: measured 9.458 ms against a control's 4.985, on a
    // change that only deleted work. Hence two functions, and the expensive one keeps the name
    // that sounds expensive.
    NodePoolStats live_stats() const;
    NodePoolStats stats() const;
    bool validate() const;

    // Which arm this pool is running, after the budget and the environment have both had their say.
    // Read by the tests, and by anything that prints a figure R2b changes the meaning of.
    bool subpixel_rule() const { return subpixel_rule_; }

    // The finest level the pool will keep at this node's position, from wherever the camera was on
    // the last `update`. Public because the rule is the interesting part of this change and a test
    // that cannot ask the pool what it thinks has to re-derive it -- which is a second copy of the
    // arithmetic and the exact way D674 went wrong.
    u32 subpixel_finest_for(const NodeKey& key) const;

    // ---- R12c's other half: the clip's extent ---------------------------------------------------

    // Give the pool its roots from the clip, before anything has been sampled or pasted.
    //
    // `lo`/`hi` are the clip's own extent in absolute voxels, inclusive. `may_hold` is the field's
    // conservative answer for a box. Shells are built from kEntryLevel down to `cell_level`, whose
    // mask bits are the finest statement the seed makes; nothing below it is addressed and nothing
    // at all is stored.
    //
    // **Refused, and it says so, when the world is not empty.** A world with chunks in it has real
    // roots or is about to get them from `index_world`, and seeding a clip mask over one would be
    // exactly the disagreement `mirror_seed` exists to forbid. So this is a no-op on a warm load,
    // which is the correct behaviour rather than a guard against misuse: a cached world does not
    // need deriving.
    //
    // Call it once, after `create` and before the first `update`. Calling it again replaces
    // nothing -- a seed already in force is kept and the report says `live`.
    NodeSeedReport seed_from_clip(const World& world, const i64 lo[3], const i64 hi[3],
                                  const NodeSeedOracle& may_hold,
                                  u32 cell_level = kClipSeedCellLevel);

    // What the last `seed_from_clip` put in, and whether it is still in force.
    const NodeSeedReport& clip_seed() const { return seed_report_; }
    bool clip_seed_live() const { return seed_live_; }

    // The mirror. Walks the pool's own seeded records and compares each mask bit against the FIELD
    // (through the same oracle) and against the WORLD (through `world_has`). See NodeSeedMirror for
    // which of the six numbers are allowed to be non-zero and which are not.
    NodeSeedMirror mirror_seed(const World& world, const NodeSeedOracle& may_hold) const;

    // ---- R8b -----------------------------------------------------------------------------------

    // Which arm the child source is running, after the budget has had its say.
    bool hashed_variation() const { return hashed_variation_; }
    u32 variation_seed() const { return variation_seed_; }

    // The mirror walker: one sub-voxel cell, read the way the shader will read it.
    //
    // `(sx, sy, sz)` are the cell's coordinates at `depth` levels below a voxel, so the voxel that
    // contains it is `(sx >> depth, ...)` -- the same arithmetic `node_key_of` does one direction
    // up, and the same arithmetic `node_descend` does with a point and a target. At depth 0 this
    // must answer exactly what `mirror_voxel` answers, and the test asserts that over the whole
    // facility rather than taking it as read.
    //
    // This exists BEFORE the renderer touches the source, and that ordering is the point. Residency
    // was built this way in Stage 2 and two of R1a's four bugs were caught by a mirror and by
    // nothing else, because a structure the renderer walks and nobody compares against the world is
    // a renderer debugging a mirage.
    VariationSample mirror_variation(i64 sx, i64 sy, i64 sz, u32 depth) const;

    // The eight children this source derives for a voxel, or for a cell below one. Air in gives
    // nothing out; the mask is the parent's own presence, never a hashed shape.
    VariationChildren variation_children_of(i64 sx, i64 sy, i64 sz, u32 depth) const;

    // A fingerprint of what the source derives, over a fixed box of the world at a fixed stride.
    //
    // Fixed, and stated in the call rather than chosen from the camera, because "same key, same
    // children, across two runs" is only a claim if the two runs asked the same question. A
    // fingerprint taken over whatever happened to be resident would differ between two runs for
    // reasons that have nothing to do with the hash.
    //
    // It reads the WORLD rather than the pool for the same reason: residency is a race and world
    // content is not.
    u64 variation_fingerprint(const World& world, const i64 lo[3], const i64 hi[3], i64 stride,
                              u32 depth, u64* cells_out = nullptr) const;

    // ---- R8a and R8e -----------------------------------------------------------------------------

    // Which arm the mode is running, after the budget has had its say. Read by the tests and by
    // anything that prints a figure whose meaning depends on it.
    bool infinite_detail() const { return infinite_detail_; }

    // The finest level a ray at this node's distance can address, SIGNED and mode-aware.
    //
    // `subpixel_finest_for` is the same question floored at the leaf, and it stays exactly as it was
    // because R2b's rule is about stored nodes. This is the one the DESCENT asks: with the mode off
    // it can never answer below `kLeafLevel`, and with it on a node 10 cm away answers -8.
    i32 marcher_finest_for(const NodeKey& key) const;

    // What is at a cell below a voxel, read the way the shader will read it.
    //
    // `key.level` must be 0 or below; a key above it is answered as the voxel it contains, which is
    // the honest answer rather than an assertion -- the caller asked about a cell coarser than the
    // thing this can talk about.
    //
    // The voxel itself is read through `mirror_voxel`, which is the pool's own walk -- entry hash,
    // descent, child mask, leaf payload -- so this is not a second reader that could agree with
    // itself about a walk nobody checked. Below the voxel the answer comes from R8b's chain, one mix
    // per octant, and nothing is allocated, uploaded or remembered.
    SubVoxelFind locate_below(const NodeKey& key) const;

    // How many bytes the pool is holding that are finer than a brick. Nought, always, in both arms.
    //
    // A counter that can only ever read nought looks like a waste of a line until you ask what it is
    // FOR, which is R8e's second gate: "resident bytes stay bounded by resolution". That claim is
    // made by construction here -- no record below `kLeafLevel` is ever written -- and a claim made
    // by construction is worth exactly as much as an instrument that would notice if it stopped
    // being true. This is that instrument, and it walks the array rather than trusting a total.
    u64 sub_voxel_bytes() const;

private:
    struct Resident {
        u32 slot = kNoNode;
        u64 last_wanted = 0;
        u64 revision = 0;      // the chunk revision this copy was folded from
    };

    // Builds a node and everything under it, bounded by the frame's budget. Returns the slot, or
    // kNoNode — and kNoNode means one of two completely different things, which is why
    // `out_of_room_` exists beside it. "Nothing is here" and "I could not fit it" have to be told
    // apart: treating the second as the first is how a tree stops building and reports open sky,
    // and it is the same silence D133 and D147 are both about.
    // A node on its own: coordinates, level, and a child mask taken from the world. No children
    // and no colour — a shell gains its colour when it is folded from children that arrive later,
    // which is the only exact way to get one (D152).
    u32 build_shell(const World& world, const NodeKey& key, u32& budget);
    u32 build_leaf(const World& world, const NodeKey& key, u32& budget);

    // Walks from a root down to the level that was asked for, creating what is missing and
    // folding the chain back up. Bounded by that level, which is what makes the pool follow the
    // pixels instead of building everything: a node smaller than the pixel that asked for it is
    // never created (D190).
    u32 refine(const World& world, const NodeKey& key, u32 root_slot, u32& budget);
    void fold_children(u32 slot);
    // Frees what a node owns; the caller decides what happens to its own slot, because a
    // node inside a run is not separately allocated and must not be freed as though it were.
    void release_contents(u32 slot);
    // Frees the subtree under a node and leaves the node itself standing as a shell: its level,
    // its coordinates and its child mask are kept, and `children` becomes kNoNode. A shell is a
    // node that says "something is here, I do not yet know what", which is the answer an evicted
    // root has to give. Clearing the node instead makes the pool say "nothing is here", and a
    // shadow ray believes it and flies out of a sealed room (D324).
    void release_children(u32 slot);
    void release(u32 slot);
    void note_no_room();
    u32 allocate_node();
    u32 allocate_children();

    // Does the world hold anything at all under this node?
    //
    // Without it, building a node at the entry level descends into eight children
    // unconditionally, and 8^11 is 8.6 billion nodes for a world holding one brick. The first
    // version of the summary octree hit exactly this and never returned (D153); this one hit it
    // and was rescued by an allocator that started failing, which was worse, because failing
    // looked like empty.
    bool world_has(const World& world, const NodeKey& key) const;
    void index_world(const World& world);

    // R12c's other half. One shell of the clip seed and everything under it, or kNoNode when the
    // oracle says the whole box is empty. Recursive, and bounded by `cell_level` rather than by any
    // per-frame budget: the seed is one call at load and its size is the clip's, not the screen's.
    u32 seed_shell(const NodeKey& key, const NodeSeedOracle& may_hold, u32 cell_level,
                   NodeSeedReport& into);
    // Put a root in the entry table. Shared with `update`'s own root seeding so the two cannot
    // disagree about how a root is addressed. False when the table is full.
    bool place_entry(const NodeKey& key, u32 slot);
    // Hand the addressing back. Every seeded root sheds its seeded subtree and re-derives its mask
    // from the world, so what is left is what `index_world` would have built on its own.
    void retire_clip_seed(const World& world);
    // Free a seeded shell's seeded descendants and its run, and nothing else.
    //
    // Deliberately NOT `release_children`, which is the eviction path: that counts every node it
    // frees into `evictions_` and into the eviction instrument, and retirement is not an eviction.
    // A pool that reported a thousand evictions for handing back addressing nobody had read would
    // make every churn figure on the run unreadable.
    void free_seeded_subtree(u32 slot);
    // A node's box clipped to the seeded extent, in absolute voxels. False when they do not meet,
    // which is how the recursion prunes to the clip rather than to the 512 m block round it.
    bool seed_box_of(const NodeKey& key, i64 lo[3], i64 hi[3]) const;

    // Does any part of this node's box fall inside the camera's frustum?
    //
    // Conservative on purpose -- a node straddling an edge counts as visible -- and it does not
    // ask about occlusion, so a node behind a wall inside the frustum counts too. Both make the
    // number an UPPER bound on what is really on screen, which is the direction an instrument
    // about wrongly-evicted nodes has to err in: an over-count says "look here", an under-count
    // says nothing at all. It is also exactly the set a "do not evict what the camera is looking
    // at" rule would have to hold resident, so the same number prices that fix.
    bool in_view(const GpuNode& node) const;

    // Remembers that this node was evicted, so a request for it can be recognised as a rebuild.
    void note_eviction(const GpuNode& node, u64 frame, u32 slot);

    // R2b. Is this node finer than any ray at its distance can address?
    //
    // Takes the record rather than a key because the erosion sweep has the record in hand and a key
    // would mean rebuilding one from it. Answers false with the rule off, so every caller reads as
    // the control arm without a second branch at each site.
    bool node_is_subpixel(const GpuNode& node) const;
    // The box the two above measure to, in voxels, and the distance from the camera to its nearest
    // point. One place, so the sweep and the request path cannot disagree about where a node is.
    //
    // R8a made `level` signed. At or above nought the arithmetic is the integer shift it always was,
    // byte for byte; below it the box is a FRACTION of a voxel and there is no integer to shift, so
    // that branch works in f64. The split is deliberate: the shipped arm runs the same instructions
    // it always did, and a difference in a measured figure cannot be hiding in a rewrite of it.
    f64 distance_to_box(i64 x, i64 y, i64 z, i32 level) const;

    // R8b. The depth-0 sample: a voxel's own colour and the seed its children come from. Takes the
    // type rather than reading it, because the mirror gets it from the POOL and the fingerprint
    // gets it from the WORLD, and those are two different questions on purpose.
    VariationSample variation_at_voxel(VoxelTypeId type, i64 x, i64 y, i64 z) const;

    NodePoolBudget budget_;
    const VoxelTypeTable* types_ = nullptr;
    // The same segregated-fit pool residency uses for brick payloads, for the same
    // reason: a brick's encoded size varies from eight bytes to two kilobytes, and a
    // bump allocator that never reclaims cannot survive a camera that moves.
    BlockPool payload_pool_;

    std::vector<GpuNode> nodes_;
    std::vector<u64> occupancy_;     // kBrickWords per leaf
    std::vector<u8> payload_;
    std::vector<u32> entries_;       // open-addressed table over entry-level nodes

    // Leaves, in their own array indexed by a leaf id rather than by a node slot.
    //
    // Two things fall out of the separation and both matter. A leaf's header is the brick layout
    // the renderer has used since Stage 2 — payload offset, palette, index bits, the two
    // occupancy mips — so nothing about brick decoding has to be reinvented, and the ugly attempt
    // to fold it into spare coverage bits goes away. And a leaf keeps its id when its node is
    // moved into a parent's contiguous run, so moving a node copies thirty-two bytes rather than
    // its sixty-four bytes of occupancy as well.
    std::vector<GpuBrickHeader> leaves_;
    std::vector<u32> leaf_payload_size_;   // CPU only: what to hand back to the pool on eviction

    // Marked at every write into the arrays above. A missed mark is a stale byte on the GPU and
    // a wrong picture, which is exactly what NodeBuffers::audit exists to catch: it walks the
    // real buffers against these arrays and names the first byte that disagrees.
    u64 touch_frame_ = 0;   // the frame `touch` stamps, set by update()
    // The resumable proximity sweep: where it is, and the brick it was started for. Restarted
    // when the camera crosses into a new brick, or when the world's chunk set changes.
    NodeKey proximity_at_{0, 0, 0, 0xFFFFFFFFu};
    u64 proximity_cursor_ = 0;
    usize proximity_chunks_ = 0;
    bool proximity_done_ = false;
    u32 erode_cursor_ = 0;   // where the rolling erosion sweep is
    // When each node was last read by a ray. CPU-side and parallel to `nodes_`, rather than a
    // field in GpuNode: the record is thirty-two bytes, which is two nodes to a cache line, and
    // the GPU never reads this. Four bytes a node, so four megabytes at the default budget.
    std::vector<u32> node_last_read_;
    // Kept between frames so it is not reallocated every one. Cleared at the top of update().
    std::unordered_set<NodeKey, NodeKeyHash> seen_requests_;
    DirtySet dirty_nodes_;
    DirtySet dirty_leaves_;
    DirtySet dirty_entries_;
    std::vector<std::pair<u64, u64>> dirty_payload_;
    std::vector<u32> free_leaves_;
    u32 next_leaf_ = 0;
    // The highest payload byte in use, so the upload copies a prefix rather than the
    // whole pool. It only rises: a released block is reused before this moves, and
    // tracking it downward would mean walking the free lists to find the new maximum.
    u64 payload_high_ = 0;

    std::unordered_map<NodeKey, Resident, NodeKeyHash> live_;
    std::vector<NodeKey> requested_;
    std::vector<u8> requested_source_;   // instrument only, parallel to `requested_`
    // Boxes an edit changed, in voxels, inclusive. One entry per announcement rather than one per
    // brick — see `invalidate_box`.
    struct EditBox {
        i64 lo[3];
        i64 hi[3];
    };
    std::vector<EditBox> dirty_;

    // How many nodes a box refresh looked at and what it did, so the cost of an edit is reportable
    // rather than inferred from a frame time.
    struct EditRefresh {
        usize visited = 0;
        usize leaves_rebuilt = 0;
        usize folded = 0;
    };

    // Post-order: children are refreshed before the parent that folds them, which is the ordering
    // the flat version had to sort for.
    void refresh_box(const World& world, u32 slot, const NodeKey& key, const i64 lo[3],
                     const i64 hi[3], u32& budget, EditRefresh& done);

    // A bump pointer plus two free lists, and never a search for a contiguous run.
    //
    // The first version carved runs off the tail of a single free list and checked that the
    // eight it found happened to be consecutive. They are, exactly until something is freed —
    // after which the check fails, allocation returns "nothing", and the caller reads that as an
    // empty region. A tree that stops building because it ran out of memory must not look like a
    // tree that stopped because the world is empty.
    u32 next_free_ = 0;
    std::vector<u32> free_singles_;
    std::vector<u32> free_runs_;
    bool out_of_room_ = false;

    // Which blocks hold anything, at every level from the chunk up. Rebuilt when the set of
    // chunks changes — not when their contents change, which is what `dirty_` is for.
    std::unordered_set<NodeKey, NodeKeyHash> occupied_;
    u64 indexed_chunks_ = 0;

    NodeUploadBatch batch_;
    u64 builds_ = 0;
    u64 evictions_ = 0;
    u64 requests_ = 0;
    u64 hits_ = 0;

    // ---- the eviction instrument ---------------------------------------------------------------
    //
    // Kept apart from the pool's own state and read by nothing that decides anything. D425 left
    // the standing-still flicker measured but not explained, and named the instrument to build
    // first: a count of evictions of nodes that were on screen. The two candidate fixes -- report
    // every node a ray TOUCHES rather than only the one it stops on, and refuse to evict anything
    // inside the frustum -- cost very differently, and nothing had measured which is needed.
    NodeView view_;
    // True while the pool is giving a node up because it has gone cold, as against re-deriving one
    // an edit touched. Only the first is an eviction, and only the first is counted.
    bool evicting_ = false;
    // When each recently evicted node went, keyed by its coordinate as the record carries it.
    // Entries leave on the request that matches them, and the rest are swept when the erosion
    // cursor wraps, so this cannot grow without bound over a long run.
    struct Evicted {
        u32 frame = 0;
        u16 fill = 0;    // occupied voxels of 512, or 0 for a node that is not a leaf
        bool never_read = false;
    };
    std::unordered_map<NodeKey, Evicted, NodeKeyHash> evicted_at_;
    u64 evictions_on_screen_ = 0;
    u64 churn_ = 0;
    u32 churn_per_level_[32]{};
    u64 churn_fill_sum_ = 0;
    u64 churn_leaves_ = 0;
    u64 evicted_fill_sum_ = 0;
    u64 evicted_leaves_ = 0;
    // How many times a RAY has reported reading each slot since it was allocated. Separate from
    // `node_last_read_`, which is stamped by builds and by the proximity sweep as well, so it
    // cannot tell "a ray stopped reporting this six hundred frames ago" from "no ray has ever
    // reported it at all". Those are the two candidate causes, and they need different fixes.
    std::vector<u32> node_reads_;
    u64 evicted_never_read_ = 0;
    u64 churn_never_read_ = 0;
    u64 churn_by_source_[kRequestSourceCount]{};

    // ---- R2b's second half ---------------------------------------------------------------------
    //
    // The camera the rule measures from, kept from the last `update`. It is the same array the
    // caller passes for the proximity sweep, so there is one answer to "where is the camera" in
    // this file and the rule cannot drift from the radius that is holding twenty metres resident.
    f64 camera_voxel_[3] = {0.0, 0.0, 0.0};
    bool camera_known_ = false;
    bool subpixel_rule_ = false;
    f64 pixel_angle_ = kSubPixelAngle;
    u32 subpixel_margin_ = 0;
    u64 subpixel_refused_ = 0;
    u64 subpixel_evicted_ = 0;
    // Frame of the last line this pool logged about the rule, so a policy that is doing something
    // is visible in a `--settle` run without the frame report having to be taught about it, and a
    // policy that is doing nothing costs one compare a frame. The pool already logs its own edit
    // refreshes on the same principle.
    u64 subpixel_said_ = 0;

    // ---- R12c's other half ----------------------------------------------------------------------
    //
    // The roots the clip seeded, kept so retirement can find them without walking the whole array,
    // and the flag that says whether the clip's masks are still standing. `seed_cell_level_` is
    // kept because `mirror_seed` has to ask the oracle about the same boxes the seed asked about.
    std::vector<u32> seed_roots_;
    i64 seed_lo_[3] = {0, 0, 0};
    i64 seed_hi_[3] = {-1, -1, -1};
    u32 seed_cell_level_ = kClipSeedCellLevel;
    bool seed_live_ = false;
    NodeSeedReport seed_report_;

    // ---- R8e -------------------------------------------------------------------------------------
    // Read once at `create` and logged, on the same terms as R2b's and R8b's arms: a policy that can
    // be switched has to say which way it was switched, or a table of numbers taken over two runs
    // means nothing (D621).
    bool infinite_detail_ = kInfiniteDetailDefault;

    // ---- R8b ------------------------------------------------------------------------------------
    bool hashed_variation_ = false;
    u32 variation_seed_ = kVariationSeed;
    // Quantised once at `create`. Everything downstream of it is integer, which is what makes
    // "and on every machine" a property rather than a hope.
    u32 variation_amount_ = variation_amount_q(kVariationColour);
    // Frame of the last fingerprint this pool logged, on the same principle as `subpixel_said_`:
    // the two runs whose hashes have to match are `--settle` runs of a binary whose `main.cpp`
    // knows nothing about the source, so the source says it itself.
    u64 variation_said_ = 0;
};

}  // namespace ws
