#pragma once
// Measuring a clip, so that "is this right?" has an answer other than squinting at it.
//
// A screenshot tells you a room looks plausible. It does not tell you the doorway is 2.05 m when
// you meant 2.00, that the two halves differ by three voxels, that the stair rise is 17.6 cm
// where building regulations and the eye both want 18, or that the volume of the pillar you
// copied sixteen times is not sixteen times the volume of one. Those are the errors that survive
// looking, and they are the ones that make a built space feel subtly wrong without anyone being
// able to say why.
//
// So everything here answers a question in numbers, in both voxels and metres, because a clip is
// authored in metres and stored in voxels and almost every mistake lives in the conversion.
//
// # What is measured
//
//   extent        the bounding box of the matter, which is rarely the box that was sampled
//   volume        solid voxels, and the same in cubic metres and litres
//   area          exposed faces — the surface a path tracer will actually light
//   centroid      where the mass is, which catches a shape that is not the symmetry it claims
//   spans         the width of the matter along a line, which is how a doorway is checked
//   profile       a slice printed as text, for when a number is not enough and a picture is too
//                 much
//   histogram     how much of each material, which catches a paint rule that never fires
//   specks        voxels alone in their material, which catches a paint rule that fires
//                 where it should not -- see paint_specks
//
// # Why exposed faces rather than a triangle count
//
// Because that is the quantity the renderer cares about and the quantity that changes when a
// surface is rough. A wall given a raspy displacement has the same volume and the same bounding
// box as a smooth one; its exposed area is half as much again. It is the only number here that
// notices texture.

#include <string>
#include <map>
#include <vector>

#include "core/types.hpp"
#include "game/clip.hpp"
#include "world/voxel_type.hpp"

namespace ws {
namespace forge {

struct Extent {
    bool any = false;
    i32 low[3]{0, 0, 0};
    i32 high[3]{0, 0, 0};   // inclusive

    i32 span(u32 axis) const { return any ? (high[axis] - low[axis] + 1) : 0; }
};

struct TypeShare {
    VoxelTypeId type = 0;
    u64 count = 0;
    f64 fraction = 0.0;
};

struct Measurement {
    i32 size[3]{0, 0, 0};       // the sampled box, in voxels
    i32 voxels_per_metre = kVoxelsPerMetre;

    Extent extent;              // of the matter, not the box
    u64 solid = 0;
    u64 covered = 0;            // cells that are part of the clip, full or empty
    u64 exposed_faces = 0;      // solid faces with air or nothing on the other side

    f64 centroid[3]{0, 0, 0};   // in voxels, relative to the clip's own origin

    std::vector<TypeShare> types;

    // Derived, in metres, because that is what the author was thinking in.
    f64 metres(i32 voxels) const {
        return static_cast<f64>(voxels) / static_cast<f64>(voxels_per_metre);
    }
    f64 cubic_metres() const {
        const f64 per = static_cast<f64>(voxels_per_metre);
        return static_cast<f64>(solid) / (per * per * per);
    }
    f64 square_metres() const {
        const f64 per = static_cast<f64>(voxels_per_metre);
        return static_cast<f64>(exposed_faces) / (per * per);
    }
};

Measurement measure(const Clip& clip, i32 voxels_per_metre = kVoxelsPerMetre);

// How wide the matter is along one axis, at a fixed position on the other two.
//
// This is the doorway check, the corridor check and the wall-thickness check. It reports the
// first and last solid cell and whether anything in between was empty, because "two metres wide"
// and "two metres wide with a gap in the middle" are different answers to the same question.
struct Span {
    bool any = false;
    i32 first = 0;
    i32 last = 0;
    i32 solid = 0;      // how many of the cells between first and last hold matter
    bool contiguous = false;

    i32 length() const { return any ? (last - first + 1) : 0; }
};

Span span_along(const Clip& clip, u32 axis, i32 a, i32 b);

// The gap: the run of empty cells along a line, which is what a doorway or a window actually is.
// Reported the same way, because an opening is measured exactly as a solid is and confusing the
// two is how a door ends up a voxel short.
Span gap_along(const Clip& clip, u32 axis, i32 a, i32 b);

// Is the clip the same when reflected about the middle of an axis?
//
// Reports how many cells disagree rather than a yes or no, because a clip that is symmetric
// except for four voxels has a typo in it, and a clip that disagrees in half its cells was never
// meant to be symmetric.
u64 mirror_mismatch(const Clip& clip, u32 axis);

// A slice through the clip as text, one character per voxel.
//
// For when a number will not do and a screenshot is too much: this is what catches a wall that
// is one voxel thick where it should be two, or a stair whose treads are not level. `axis` is
// the one held fixed, `at` is where. Scaled down by `step` so a 256-voxel slice fits on a
// screen, taking the most common cell in each block so a thin wall does not vanish.
std::string slice_text(const Clip& clip, u32 axis, i32 at, i32 step = 1);

// --- checks: the mistakes a screenshot shows and a measurement does not ---------------------
//
// A stair flight hanging in mid-air, a column growing through the floor above, a step nobody
// could climb. Every one of these was in the facility, every one was obvious the moment somebody
// looked at a picture, and not one of them moved a single number in the report above — the
// volume, the extent and the material shares were all exactly what they should have been.
//
// They are not subtle bugs. They are bugs of a *kind* that measuring quantities cannot see,
// because they are about how parts relate rather than how big they are. So they get their own
// pass, and it answers the three questions a builder would ask: is it all joined together, can
// you walk on it, and is anything inside anything else.

// Every island of matter that is not connected to the largest one.
//
// Six-connected, because that is what "held up" means for voxels — touching at a corner is not
// support. The largest component is assumed to be the building; anything else is either a
// deliberate separate object or a mistake, and the report says how many and how big so the
// author can tell which.
struct Island {
    i32 low[3]{0, 0, 0};
    i32 high[3]{0, 0, 0};
    u64 voxels = 0;
};

struct Connectivity {
    u64 components = 0;
    u64 largest = 0;
    u64 floating_voxels = 0;      // everything not in the largest component
    std::vector<Island> islands;  // the biggest few of them, for pointing at
};

Connectivity connectivity(const Clip& clip, usize report_at_most = 8);

// Can a person walk it?
//
// Looks down every column of the clip for the top of the matter, then compares neighbouring
// columns: a change of more than a step is a step nobody can take, and a change of more than a
// wall is a wall. Reports the worst rise found and where, and how much of the walkable surface
// is reachable from the lowest floor without exceeding a step.
struct Walkability {
    i32 max_rise = 0;            // in voxels, between neighbouring columns
    i32 max_rise_at[3]{0, 0, 0};
    u64 surfaces = 0;            // columns with a walkable top at all
    u64 reachable = 0;           // of those, how many are reachable from the start
    f64 reachable_fraction() const {
        return (surfaces > 0) ? static_cast<f64>(reachable) / static_cast<f64>(surfaces) : 0.0;
    }
};

// `max_step` is the tallest rise a person may take, in voxels. Anything taller is a wall, and
// walls stop the flood rather than failing it — the question is what you can reach, not whether
// the clip contains a cliff.
Walkability walkability(const Clip& clip, i32 max_step, i32 head_room);

// PAINT SPECKS — a voxel of one material alone on a surface of another.
//
// Reported from playing, twice in one sitting: *"remnants of metal on the lights"* and *"remnants
// of green voxels on the urns"*. Both were single voxels wearing a material that belongs to
// something 2 cm away, and both are invisible to every number this file already prints. Volume is
// right. Components are right — a speck is welded to what it sits on, so connectivity says
// nothing. The histogram is right to four decimal places, because two voxels out of forty
// thousand do not move a fraction. And a screenshot of the whole building is far too small to
// show one. The only witness was somebody standing next to it.
//
// A speck is a SOLID voxel that
//   1. touches air on at least one face — a buried voxel is not a stray dot, whatever it is
//      made of; and
//   2. shares its material with NONE of its six face neighbours.
//
// Diagonal contact does not count as kin, deliberately: a chain of voxels touching only at
// corners is what a shattered thin feature looks like, and it reads as scattered dots exactly
// like a bleed does. A genuine one-voxel-wide ring or inlay line is NOT flagged, because
// consecutive voxels along it are face neighbours of each other.
//
// # Reading it: a stipple and an accident look nothing alike
//
// Some materials are MEANT to be speckly. A weathering coat keyed on noise — the facility's
// desert and overgrown coats — paints a dither, and a dither is mostly specks by construction.
// So this reports a fraction as well as a count, and the two cases separate on it without any
// threshold having to be invented:
//
//   a stipple    thousands of specks, tens of per cent of that material's own surface
//   an accident  a handful of specks, a fraction of a per cent
//
// Which is the whole reason it is per material rather than one number for the clip.
//
// Run on the clip BEFORE `variation` is applied. Variation mints a distinct voxel record per
// voxel — 95% distinct on a small fitting — so after it every voxel is alone in its type and
// every voxel is a speck. That is not a subtlety this can defend itself against; it is a caller's
// obligation and it is why the parameter is a Clip and not a SampleResult.
struct Speck {
    VoxelTypeId type = 0;
    i32 at[3]{0, 0, 0};
};

struct SpeckReport {
    u64 surface = 0;                  // solid voxels touching air, the population examined
    u64 specks = 0;
    // Per material, biggest count first. `count` is specks; `fraction` is of THAT material's
    // own surface, which is what tells a dither from a mistake.
    std::vector<TypeShare> by_type;
    std::vector<Speck> examples;      // a couple per material, for pointing a camera at
};

// `examples_per_type` coordinates are kept for each material rather than for the clip, so one
// crowded material cannot fill the list. See the note where they are collected.
SpeckReport paint_specks(const Clip& clip, usize examples_per_type = 2);

// And REMOVING them, which is a different job from finding them and a much more delicate one.
//
// Reported as *"the 3x3 speckles never got fixed fix them now too"*, and that is fair: D609 built
// the instrument and then argued, correctly in both cases, that the two specks reported were not
// faults. It did not follow that the other two thousand were fine. A lone voxel of somebody else's
// material on a surface is wrong wherever it comes from — a rule reaching two centimetres past its
// own shape, two curved surfaces of different materials meeting on a 3 cm grid, a fitting buried
// 0.09 into a wall painted by both — and it is wrong the same way each time. It is not worth twenty
// separate fixes in twenty fragments. It is worth one pass.
//
// A speck is repainted with the material MOST of its face neighbours wear. Geometry is untouched:
// no voxel is added, none is removed, so the volume, the components and the walkability cannot
// move. Only the colour of an isolated voxel changes, and only to the colour of what it sits on.
//
// # What must NOT be despeckled, and how it is told apart
//
// A weathering coat keyed on noise is MEANT to be specks — the facility's desert and overgrown
// coats are a dither, and a dither with its isolated voxels smoothed away is a coat that no longer
// exists. So the pass will not touch a material whose specks are a large share of its own surface.
// That is exactly the fraction paint_specks already reports, and exactly the reading D609 gave it:
// tens of per cent is a stipple, a fraction of one is an accident. `stipple_share` is where the
// line is drawn, and a material at or above it is left entirely alone.
//
// # Why it runs on the sampler's output and not on a rule
//
// Because the fault is not in any one rule. It is in what two correct rules do where they meet, at
// this resolution — and the resolution is not a thing any fragment author can see from inside their
// own file. Twenty authors cannot each be asked to defend a boundary none of them owns.
struct DespeckleReport {
    u64 repainted = 0;
    u64 left = 0;                     // specks in materials judged to be a deliberate stipple
    std::vector<TypeShare> by_type;   // what was repainted, biggest first
};

// Which materials a lone voxel may be repainted out of, decided over a WHOLE clip.
//
// The judgement is about a MATERIAL and not about a voxel: whether a stray verde voxel is a
// mistake depends on how the other four hundred thousand verde voxels are behaving, and no amount
// of looking at its six neighbours can tell you that. Over a whole building that is a sound
// question to ask of the sample in hand. Over one NODE it is not -- five hundred cells cannot say
// whether a coat is a deliberate dither -- and R11b refines a node at a time.
//
// So the verdict is separable: taken once from a sample of the whole clip, and handed to every
// node's despeckle afterwards. A material absent from the map has no specks anywhere in the clip
// and nothing to decide.
struct StippleVerdict {
    std::map<VoxelTypeId, bool> allowed;
    bool any() const { return !allowed.empty(); }
};
StippleVerdict stipple_verdict(const Clip& clip, f64 stipple_share = 0.05);

// The two counts the verdict is a threshold over, kept separately so they can be SUMMED.
//
// R11d wants the up-front whole-clip sample gone, and the verdict is the one thing only that sample
// can produce (D625: a coarser sample gives a different verdict, not a cheaper one -- 35, 31, 26 and
// 19 materials at metres 8, 4, 2 and 1). But the verdict is a ratio of two counts per material, and
// both are additive over disjoint boxes -- so a ladder that samples the building node by node can
// accumulate them and take the same verdict at the end without ever holding the whole clip.
//
// The flaw is real and is why this is measured rather than assumed: `paint_specks` reads outside a
// clip as AIR, so a voxel on a node's face counts as surface, and counts as a SPECK if its only
// same-material neighbour is in the node next door. Summing per-node counts therefore over-counts
// both, and not equally. That is R11b's skirt problem (D615) arriving in the paint statistics, and
// whether it moves the verdict is a question for `stipple_verdict` on the sum against
// `stipple_verdict` on the whole clip, material for material.
struct StippleCounts {
    struct Pair {
        u64 surface = 0;   // solid voxels of this material touching air
        u64 specks = 0;    // ...of those, the ones with no neighbour of their own material
    };
    std::map<VoxelTypeId, Pair> by_type;
    void add(const StippleCounts& other) {
        for (const auto& [type, pair] : other.by_type) {
            Pair& mine = by_type[type];
            mine.surface += pair.surface;
            mine.specks += pair.specks;
        }
    }
    bool any() const { return !by_type.empty(); }
};

StippleCounts stipple_counts(const Clip& clip);
// The same rule `stipple_verdict(clip)` applies, over counts from wherever they came.
StippleVerdict stipple_verdict(const StippleCounts& counts, f64 stipple_share = 0.05);

// `verdict` null asks the clip in front of it, which is right for a clip that IS the whole thing.
DespeckleReport despeckle(Clip& clip, f64 stipple_share = 0.05,
                          const StippleVerdict* verdict = nullptr);

// Where two parts of a shape occupy the same voxels.
//
// Not an error in itself — a building is full of deliberate overlaps, and a union is how you
// make one. It is an error when it is *not* deliberate: a column through the floor above, a
// stair through a wall. So this answers a question the author asks about two specific things,
// and the answer is a count and a box, so it can be looked at.
struct Overlap {
    u64 voxels = 0;
    i32 low[3]{0, 0, 0};
    i32 high[3]{0, 0, 0};
    bool any() const { return voxels > 0; }
};

// The whole measurement as a report, in the order a person reads it.
//
// The names may be null; when given, entry i is the name of voxel type i, so the report says
// "stone" rather than "type 3". The script layer knows those names and nothing else does.
std::string report(const Measurement& m, const std::vector<std::string>* names = nullptr);

}  // namespace forge
}  // namespace ws
