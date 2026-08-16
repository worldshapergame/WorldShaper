#pragma once
// Judging and cleaning the WORLD's specks: one walk, across the pool, at the ladder's fixed point.
//
// This is R11d route 1b, and D630 is the whole reason it has a file of its own. The verdict --
// which materials are a deliberate dither and must not have their lone voxels repainted -- has to
// be a property of the WHOLE building (D629), and the only whole-building measurement the game has
// today is the up-front metre-8 sample, which is the loading bar R11d exists to delete. Taking the
// verdict from the world instead is built and works (D629, D642: it costs 154 voxels of 3.8 M).
// What kept it behind `--stipple-from-world` is its price, and D630 wrote the price down in the
// shape of its own cure:
//
//     "19.0 -> 40.3 s. 68 chunks of 258 cubed, 1.17 G cells walked TWICE, SERIAL."
//
// Both halves of that sentence are addressed here, and they are two separate savings:
//
//   TWICE   The judging pass and the cleaning pass walk the same cells and apply the same
//           six-neighbour test. They cannot be MERGED -- the verdict is a property of the whole
//           building, so nothing may be repainted until every chunk has been counted, and that is
//           D629's finding and it is not negotiable. But the READ half is shareable, because
//           finding a speck does not need the verdict; only deciding what to do with one does.
//           So one walk records the counts AND every speck it saw, with the material that speck
//           would be repainted to; the verdict is taken from the complete counts afterwards; and
//           the repaint is then a few hundred writes over a list rather than a second walk of a
//           thousand million cells. Half the cells, and half the `capture_clip` calls that fetch
//           them out of the world -- which is most of the cost, not a fringe of it.
//
//   SERIAL  The boxes are disjoint and the counts are additive, which is the easy shape to hand to
//           a pool. `stipple_counts_from_world` was already threaded; the cleaning sibling never
//           was, and it is the 19 s of the 21.3.
//
// # Why the writes are deferred, and what it changes
//
// `forge::despeckle` states the rule it obeys in its own comment: it reads from a copy and writes
// to the clip, "or it is not a rule" -- a repainted voxel must not become one of the neighbours the
// next voxel is judged against, or the answer depends on the scan order. Inside one box the shipped
// code has always done that. ACROSS boxes it did not: `despeckle_world_serial` writes chunk k back
// into the world before it captures chunk k+1, so chunk k+1's one-voxel skirt sees chunk k's
// repaints. The rule was simultaneous within a chunk and sequential between chunks.
//
// Deferring every write until every box has been read makes the whole world one simultaneous step,
// which is the rule `despeckle` already claims. It is therefore not a concession to threading; the
// threading is what made it necessary to look.
//
// AND THE CHUNK SEAM IS NOT WHY THE TWO ARMS DIFFER, which is worth knowing because it is the
// obvious answer and it is wrong. If it were the seam, every voxel the two treat differently would
// be within one voxel of a chunk face; measured, that is 11 of 94 on `clips/sampler.clip` and 0 of
// 61 on the facility. The cause is inside `forge::despeckle` itself: its comment says it reads from
// a copy "so that a repainted voxel cannot become one of the neighbours the next voxel is judged
// against", and only `mine` comes from the copy -- the six neighbours go through `type_at`, which
// reads the clip being written. So the shipped clean is sequential WITHIN a chunk as well as
// between chunks, and the one walk here is the simultaneous step that comment describes. That is
// why it repaints slightly more: a speck the shipped pass spares because its neighbour was
// repainted into its own material a moment earlier is still a speck when the whole world is judged
// at once. `forge::despeckle` is deliberately NOT changed -- the ladder despeckles every node it
// sharpens through it, so a fix moves the world every load produces -- and
// `tests/test_world_stipple.cpp` pins which of the two it does. D662.
//
// # Why the result cannot depend on the order the boxes finish in
//
// Three things, and each is a property of the construction rather than of a lock:
//
//   - Nothing writes the world while the walk is running, so every box reads the same world
//     whatever order the workers take them in.
//   - Each box's counts and specks land in a slot of their own, indexed by the box. No two
//     threads touch one slot, and the merge afterwards runs on the main thread in box order.
//   - `StippleCounts::add` is u64 addition into a map keyed by material. Integer addition is
//     associative and commutative, so a sum taken in any order is bit-identical -- and there is no
//     floating point anywhere in the accumulation. The one division `stipple_verdict` does is over
//     totals that are already order-independent. (This is exactly why the world scan passes a
//     margin of 1: `stipple_counts`'s margin-0 path reconstructs `surface` out of a DOUBLE
//     fraction, which is not a quantity anybody should be summing.)
//
// # Boxes, not chunks
//
// A chunk with its skirt is 258 cubed, which is 17.2 M cells and about 86 MB of clip. One of those
// per worker is fine on four cores and is 1.3 GB on sixteen, and this runs on the machine the game
// is being played on. So a chunk is cut into slabs of `kCleanSlab` voxels and each slab is captured
// with its own skirt. The interiors still tile the world exactly once, which is the only property
// D629's method needs, so the counts are unchanged voxel for voxel; the peak is a quarter, and the
// price is the 2% of extra cells the additional skirts cost.

#include <vector>

#include "core/types.hpp"
#include "forge/measure.hpp"
#include "world/voxel_type.hpp"
#include "world/world.hpp"

namespace ws {

class JobSystem;

namespace forge {

// One lone voxel, in the world's own coordinates, and what the pass would repaint it to.
//
// `becomes` is the material most of its solid face neighbours wear, decided by `despeckle`'s own
// tie-break -- first in the fixed order x-, x+, y-, y+, z-, z+ -- and kAir when it has no solid
// neighbour at all, which `despeckle` reads as "leave it alone". It is worked out during the walk
// rather than at repaint time because at repaint time the box it came out of is long gone, and
// re-reading six neighbours out of `World` for every speck would put the memory reads back.
struct WorldSpeck {
    i64 at[3]{0, 0, 0};
    VoxelTypeId was = 0;
    VoxelTypeId becomes = 0;
};

// What one walk of the world found. `counts` is the verdict's evidence; `specks` is the work list.
struct WorldStippleScan {
    StippleCounts counts;
    // In box order, and within a box in the scan order `despeckle` uses. That order is fixed by
    // `World::sorted_chunk_coords` and the slab loop, so it is the same on every machine and in
    // every run -- which matters because it is what makes `repaint_world_specks` reproducible
    // without sorting anything.
    std::vector<WorldSpeck> specks;
    u64 boxes = 0;          // how many were walked
    u64 boxes_skipped = 0;  // ...and how many held nothing and were never captured
    u64 cells = 0;          // cells captured, including the skirts
};

struct WorldCleanReport {
    u64 repainted = 0;
    u64 left = 0;                     // specks in materials judged to be a deliberate stipple
    std::vector<TypeShare> by_type;   // what was repainted, biggest first
    // Every voxel this pass changed and what it changed to. Carried because "the two arms leave
    // the same world" is the claim that decides whether the shared walk may replace the two, and a
    // content hash can only say that they differ -- not which voxels, or which way. Both arms fill
    // it, so `--clean-world` can print the disagreement cell by cell instead of a verdict on it.
    std::vector<WorldSpeck> repaints;
    // The chunks that actually changed, in world order and without repeats. The renderer holds
    // copies of every brick it has built and a repaint changes what they look like without
    // changing what is where, so each of these has to be announced. D397 is what happens when it
    // is not.
    std::vector<ChunkCoord> changed;
};

// The one walk. `jobs` may be null, which runs it on the calling thread.
//
// The pool must be a FOREGROUND one with no other submitter: `parallel_for` queues a take-loop over
// the whole range, so a second submitter gets no workers until this has finished and would then be
// handed this one's jobs by `wait()`. That is D511 and trap 17, and the reason this is called at
// the ladder's fixed point -- where the ladder has stood down and nothing else is submitting -- and
// not from anywhere else.
WorldStippleScan scan_world_specks(const World& world, JobSystem* jobs);

// The verdict applied. Small, serial and on the caller's thread on purpose: it is a few hundred
// writes over a list that is already decided, and `World::set` walks a hash of chunks that a
// concurrent writer would have to be locked out of for no measurable gain.
WorldCleanReport repaint_world_specks(World& world, const WorldStippleScan& scan,
                                      const StippleVerdict& verdict);

// The control arm: the shipped pass, chunk at a time, two walks, writing as it goes.
//
// Kept because an A/B in this project is two flags of one build and never two builds (D407), and
// because "identical to the serial version" is a claim that has to be measured rather than argued --
// it turned out to be false, and not for the reason anybody expected (D662). In the game it is
// `--world-clean-serial`.
StippleCounts stipple_counts_from_world_serial(const World& world, JobSystem* jobs);
WorldCleanReport despeckle_world_serial(World& world, const StippleVerdict& verdict);

}  // namespace forge
}  // namespace ws
