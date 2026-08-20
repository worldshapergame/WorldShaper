#pragma once
// Turning a field into voxels, and deciding what each one is made of.
//
// The field says where the matter is. This says how finely to ask, over what volume, and which
// voxel type each answer becomes. Those are three separate decisions and they are kept separate:
// a clip authored at human scale should be re-samplable at a different resolution without a
// single number in it changing, and re-painted without its geometry being touched.
//
// # Painting is a stack of rules, not a property of a shape
//
// The obvious design gives every shape a material. It falls apart on the first real surface:
// a wall is stone, except where it is damp, except where the damp has moss on it, except in the
// mortar courses. Those are not four shapes, they are one shape and four rules about it.
//
// So a material is a rule — a field, a range that field has to fall in, and optionally a
// direction the surface has to face — and the rules are applied in order, each one painting over
// what came before. `stone everywhere`, then `moss where the grain is high and the face points
// up`, reads as what it does and composes without either rule knowing about the other.
//
// # What "inside the clip" means, and why it is not "solid"
//
// A clip carries a mask saying which cells are part of it at all, separate from which cells hold
// matter. The distinction matters for stamping: an empty cell inside the clip is *air the clip
// asserts*, and stamping it clears whatever was there, while a cell outside the clip is not the
// clip's business and must be left alone. A field fills a box, so by default the whole box is
// inside; a `bounds` shape narrows it, which is how a clip ends up L-shaped or round rather than
// always rectangular.

#include <functional>
#include <vector>

#include "core/types.hpp"
#include "forge/field.hpp"
#include "game/clip.hpp"
#include "world/node_pool.hpp"
#include "world/voxel_type.hpp"

namespace ws {

class JobSystem;

namespace forge {

// One layer of paint.
//
// `test` is any node: a pattern, a coordinate, a shape's own distance — anything that has a
// value. The rule applies where that value falls within [low, high]. That one mechanism covers
// "above two metres" (a coordinate), "in the mortar" (a brick pattern), "on the rough bits" (a
// noise), and "within five centimetres of the surface" (the shape itself).
struct PaintRule {
    u32 test = 0;
    f64 low = -1e30;
    f64 high = 1e30;
    VoxelTypeId type = 0;

    // Optional: only where the surface faces this way. The normal costs six extra field
    // evaluations, so it is computed only for rules that ask — `facing_axis` of 3 means "do not
    // ask", which is the default.
    u32 facing_axis = 3;
    f64 facing_min = 0.5;   // dot product with the positive axis direction; negative for down

    // Optional: a shape saying WHERE this rule may apply, as distinct from `test`, which says
    // what it applies to.
    //
    // The two are different questions and separating them is worth a field. A test can be a
    // pattern — a noise, a curvature, how much sky a face can see — and a pattern is true or
    // false at a point with no way to settle it for a region, so a rule keyed on one is asked at
    // every voxel there is. A place is a shape, and a shape has a bounding box, so a region that
    // lies outside it settles the rule to "no" for nothing.
    //
    // The facility measured it: six of its hundred and thirty-three rules were pattern-keyed
    // weathering, and those six were eight hundred and forty million of its nine hundred and
    // twelve million field evaluations — ninety-two per cent of the build. All six were already
    // confined to thin bands of the building by `weather ... on=`; the sampler simply had no way
    // to know it.
    u32 place = 0;
    bool has_place = false;
};

// Where to sample, how finely, and what to fill with.
struct SampleSettings {
    // The volume to sample, in metres. Everything outside is not part of the clip.
    Vec3 low{0, 0, 0};
    Vec3 high{1, 1, 1};

    // How many voxels to a metre. Defaults to the world's own resolution, which is what a clip
    // destined for the world wants; a preview can ask for less and get an answer sooner.
    i32 voxels_per_metre = kVoxelsPerMetre;

    // Sampled at the centre of each voxel rather than its corner. A voxel is a small cube of
    // space, and a surface that passes through the middle of one either fills it or does not —
    // asking at the corner biases every surface half a voxel in the same direction, which shows
    // up as a clip that is consistently a voxel narrow on two sides and a voxel wide on the
    // other two.
    bool sample_at_centre = true;

    // Optional: a node that says which cells are part of the clip at all. Negative is inside.
    // Zero means the whole box.
    u32 bounds = 0;
    bool has_bounds = false;

    // Count field evaluations per paint rule, so a slow build can say WHICH rule it was slow on.
    //
    // Off by default, and worth the flag rather than always on: it is an atomic increment beside
    // every paint evaluation, and the builds this exists to diagnose are the ones with hundreds of
    // millions of them.
    bool count_rule_cost = false;
};

// --------------------------------------------------------------------------------------
// A node is the unit the world is sampled in — R11.
//
// The renderer has asked for geometry a NODE at a time since R1: `NodePool::request` carries a
// level, and a level is a pixel footprint (R2a, D190). Everything to the left of `World` went on
// answering in regions — eighteen fixed boxes at one of two fixed resolutions, all chosen before
// the first frame is drawn — which is the whole reason a pixel-driven renderer still had a
// loading bar, a blocky first minute and detail arriving in slabs (D611).
//
// What follows is the entire mapping from a node to a sample, and it lives here rather than at
// each caller for D204's reason: the box a node covers and the resolution it is asked at must be
// one answer in one place, or the world is derived at a detail its own level disagrees with.
//
//   level 3   a brick:                    0.25 m at 32 voxels a metre
//   level 6   two metres:                 2.00 m at  4
//   level 8   what used to be a chunk:    8.00 m at  1
//
// Note what falls out of `256 / 2^level`: EVERY node is eight voxels a side, at every level. A
// node is one brick's worth of data whatever its size — which is why one budget covers the whole
// tree, and why "resolution follows how many pixels a thing covers" needs no second rule to keep
// it true. It is the same rule the marcher already runs on.
// --------------------------------------------------------------------------------------

// Eight, at every level, and derived rather than written down.
inline constexpr i32 kNodeVoxels = 1 << kLeafLevel;

// The coarsest node that can be sampled at all. Below one voxel per metre there is no integer
// resolution left to ask for, so a node above this is FOLDED from its children — which is what
// the pool already does with colour and coverage, and is not this file's business.
constexpr u32 coarsest_sampled_level() {
    u32 level = 0;
    for (i32 per_metre = kVoxelsPerMetre << kLeafLevel; per_metre > 1; per_metre >>= 1) ++level;
    return level;
}
inline constexpr u32 kCoarsestSampledLevel = coarsest_sampled_level();

// How finely a node at `level` is asked: 256 / 2^level. Zero above kCoarsestSampledLevel, which
// is the caller's signal to fold rather than to sample.
constexpr i32 node_voxels_per_metre(u32 level) {
    return (level <= kCoarsestSampledLevel) ? ((kVoxelsPerMetre << kLeafLevel) >> level) : 0;
}

// The box a node covers, in metres. A `NodeKey` holds a node's coordinate at its own level, so it
// spans world voxels [c << level, (c+1) << level) and this is that, divided by the world's own
// resolution.
struct NodeBox {
    Vec3 low;
    Vec3 high;
};
NodeBox node_box(const NodeKey& key);

// Where to sample one node. Everything that is not the box and the resolution — the bounds shape,
// centre sampling, whether rule costs are counted — is carried over from `base`, which is the
// clip's own settings, so a node is sampled the way the whole building is sampled and differs
// from it in exactly the two things this function sets.
SampleSettings node_sample_settings(const SampleSettings& base, const NodeKey& key);

// No two voxels alike.
//
// A real surface has no two square centimetres the same. Photograph a concrete wall, scan a
// weathered stone, and every patch differs from every other in colour and in how it catches the
// light — not by much, and never by nothing. A clip built from a handful of materials has the
// opposite property: millions of voxels sharing a dozen records, which is why a voxel wall reads
// as a voxel wall however good the lighting is. The repetition is the tell.
//
// So every voxel is given its own perturbation of its material: a little colour, a little
// roughness, hashed from where it is, so the same clip always comes out the same. Under a path
// tracer this is nearly free — the variation lives in the type table, not per voxel — and it is
// the difference between a surface and a texture.
//
// What it cannot honestly be is *literally* unique. Uniqueness at the facility's scale means one
// visual record per voxel: nine million records, a hundred and forty megabytes, for a difference
// no eye can resolve. So the amounts below set how finely the perturbation is quantised, the
// measurement reports how many distinct records actually resulted and how large the biggest
// group of identical voxels is, and the author can see exactly how close to unique they are.
struct Variation {
    // How far a channel may stray, as a fraction of full scale. 0 turns it off.
    f64 colour = 0.0;
    f64 roughness = 0.0;
    u32 seed = 1;

    // Optional: a field that scales the variation, so a weathered face can be more varied than
    // a sheltered one. Zero means "everywhere the same amount".
    u32 by = 0;
    bool has_by = false;

    // The most records this may create. Not a tuning knob but a safety rail: the renderer's
    // type table is a fixed GPU buffer, and a clip that asks for more records than it holds used
    // to take the renderer down with an assertion. Past the budget the pass stops minting new
    // records and reuses what it has, so the ceiling costs quality and never correctness.
    u32 budget = 1000000;

    bool any() const { return colour > 0.0 || roughness > 0.0; }
};

// The result of sampling, with the numbers a caller needs to place it in the world.
struct SampleResult {
    Clip clip;
    // Where the clip's (0,0,0) cell sits, in voxels, if the clip is placed so that its field
    // coordinates land where they were authored.
    i64 origin_voxel[3]{0, 0, 0};
    u64 evaluations = 0;
    // Split, because the two come down by completely different means: shape questions by asking
    // about boxes instead of points, paint questions by settling a rule for a whole region.
    u64 shape_evaluations = 0;
    u64 paint_evaluations = 0;
    u64 voxels_asked = 0;      // boxes that came down to a single voxel
    u64 voxels_settled = 0;    // voxels decided in bulk, never asked about

    // How many times the field was WALKED, against `shape_evaluations` points answered.
    //
    // Nought in the control arm, where every evaluation is its own walk and the two numbers are the
    // same by definition. With the block sweep on they come apart, and the gap is the whole of what
    // the sweep buys: on the facility's ladder, 638 evaluations a node arriving in six walks. A
    // clock cannot see that yet — `Field::eval_block` is still a loop of `eval` — so it is counted.
    u64 block_calls = 0;

    // Core-nanoseconds, summed across every worker, so these add up to well over the wall clock
    // on a machine with eight of them. What they are for is the RATIO: which half of the sampler
    // the build was actually spent in, which an evaluation count does not say.
    u64 paint_ns = 0;
    u64 shape_ns = 0;

    // How much the sampler had to allow for, which is what decides how early a box can settle.
    // Reported because it is the single number that most affects how long a clip takes to build,
    // and it comes from the clip rather than the code — an author who displaces a whole site by
    // five centimetres has asked for something expensive without any way of knowing.
    f64 slack = 0.0;             // the worst case, over the whole field
    f64 prune_slack = 0.0;       // the worst case for settling a box
    f64 best_part_slack = 0.0;   // the MOST any one part needs; zero when nothing moves
    f64 worst_part_reach = 0.0;  // and how much of the clip that part's box spans, 0 to 1
    usize parts = 0;             // how many parts could be told apart; 0 means none could

    // Paint rules that can never be settled for a box and so are asked at every solid voxel:
    // the ones keyed on a pattern rather than on a shape. They are the floor on what painting
    // costs, and the number an author can actually do something about.
    usize rules_total = 0;
    usize rules_per_voxel = 0;
    usize rules_placed = 0;      // how many carry an on= place the sampler can cull on

    // One evaluation count per rule, in the order the rules were given. Empty unless
    // `SampleSettings::count_rule_cost` asked for it.
    std::vector<u64> rule_evaluations;
};

// The block of voxels a node covers inside a sample of a BIGGER box taken at that node's own
// resolution — the same node, read out of the box that contains it. False when the node is not
// wholly inside that sample. Either output may be null, and each holds kNodeVoxels cubed entries.
//
// This is here for R11a's agreement check, and that check is not a formality: a small box is not
// obviously the same question as a big one. The sampler settles paint rules for a region, and a
// rule keyed on a pattern has almost no box to settle against when the box is eight voxels wide.
// `--clip-part` already paints differently from the whole building (D609, D610) — that is a part
// restriction rather than a box restriction, and nobody has told the two apart at this size.
bool node_block(const SampleResult& sampled, const NodeKey& key, VoxelTypeId* types, u8* inside);

// Everything about a field and its paint rules that does not depend on the box or the resolution.
//
// # Why this is a type rather than a few locals inside `sample`
//
// It was a few locals inside `sample`, and R11a measured what that costs when the box is small.
// Working out each rule's slack, its bounding box and the pieces its zone is made of is a walk of
// the field per rule — a hundred and thirty-nine of them on the facility — and it came out the
// same every time, because none of it is a question about the box. A whole-building sample pays it
// once against nine million voxels and it is invisible. A NODE pays it in full for an answer about
// five hundred and twelve, and it was **0.213 ms of the 1.389 a node cost, and 85% of what a node
// holding nothing cost at all**: the same clip with four rules instead of a hundred and thirty-nine
// answered in 0.012 ms. That is not the sampling, it is the arriving. D613, D614.
//
// So a plan is made once per clip and handed to every sample taken from it. `sample` keeps its old
// signature and builds one internally, so nothing that took a single sample had to change.
//
// # What it does NOT hold
//
// The box, the resolution, the bounds shape, the clip being filled, and the per-rule cost counters.
// Those are `SampleSettings` and they change per call, which is the whole point of the split: one
// plan, many boxes. It borrows the field by pointer and does not own it — a plan outliving its
// field is a dangling walk, and the ladder that holds one holds the script beside it.
struct SamplePlan {
    const Field* field = nullptr;
    u32 root = 0;
    u32 prune_root = 0;          // the shape under its outermost displacements
    f64 slack = 0.0;             // what a single voxel has to allow for
    f64 prune_slack = 0.0;       // what a box has to allow for
    f64 amplitude = 0.0;         // how far the whole-shape displacement can move a surface

    // The rules with their accepted bands grown by that displacement, which is what the descent
    // actually tests against. See the long note where they are widened.
    std::vector<PaintRule> widened;
    std::vector<f64> rule_slack;
    std::vector<Field::Aabb> rule_box;
    std::vector<Field::Aabb> rule_piece;    // flat, with a begin/end pair per rule below
    std::vector<u32> rule_piece_at;

    // The parts the shape is made of, each with its own box and its own allowance.
    std::vector<Field::Aabb> part_box;
    std::vector<f64> part_slack;
    bool parts_usable = false;

    // Reported by every sample taken from this plan, because they are properties of the clip
    // rather than of the box: how many rules there are, how many name a place, and how many can be
    // settled for neither a box nor a region and are therefore asked at every solid voxel.
    usize rules_total = 0;
    usize rules_placed = 0;
    usize rules_per_voxel = 0;

    // Whether this plan was made with the rule bounds on, which is `forge::rule_bounds_used()` at
    // the moment it was made. Recorded on the plan rather than read again where it is used, so a
    // sample cannot be planned under one rule and descended under another — the control arm is one
    // flag on one build, and a setting read twice is two.
    bool rule_bounds = true;

    bool ok() const { return field != nullptr; }
};

SamplePlan plan_sample(const Field& field, u32 root, const std::vector<PaintRule>& paint);

// How small a box has to get before the descent stops recursing and runs the rest of itself a
// LEVEL at a time, asking `Field::eval_block` about every box of a level in one traversal.
//
// D722 measured what the recursion costs at the size of a render-tree node: about **640 evaluations
// of the shape to fill 512 cells**, and `65,536 voxels asked` of `65,536 cells` over a batch of a
// hundred and twenty-eight nodes — the descent comes all the way down to a single voxel for nearly
// every cell of every ladder node. All 640 of those walks visit the same field nodes and take the
// same turns, because all 640 are about points inside the same quarter of a metre. Batched by level
// they become about SIX walks for the same 640 evaluations.
//
// The number is in CELLS rather than in voxels a side because a box of the descent is not always a
// cube — an edge box is whatever is left. 512 is one whole node of the render tree.
//
// It is a ceiling on memory as much as a choice: a level of the sweep holds every box abreast with
// its inherited paint state, and the facility carries 628 rules, so a 512-cell box bottoms out at
// 512 × 628 bytes and the whole-clip sampler's own 64³ top box would be five hundred times that.
//
// **Zero is the control arm**: the descent recurses to single voxels exactly as it did before, and
// nothing calls `eval_block` at all. The two arms build the same world, bit for bit, and that is
// the gate — `clips/sampler.clip` at `d0d5f84c685be847`, 1,430,104 solid voxels, both ways.
void set_sample_block_cells(u32 cells);
u32 sample_block_cells();

// --------------------------------------------------------------------------------------
// The box-against-cell agreement audit — D725's open question, made answerable.
// --------------------------------------------------------------------------------------
//
// The descent's whole correctness argument is that the box test and the cell test cannot disagree:
// a box settled SOLID in bulk fills cells the per-cell rule would also have filled, and a box
// settled EMPTY clears cells the per-cell rule would also have cleared. D725 found that they do
// disagree — 220 voxels of 1,430,104 on `clips/sampler.clip` — and could not say which was right,
// because `tests/test_sample.cpp`'s brute-force reference is downstream of the SAME descent it is
// checking (it calls `sample`, then compares that clip with a per-voxel walk of `Field::eval`) and
// the descent's own settled boxes are what it reads. Trap 26: every audit agreeing is not evidence
// when they all read the same source.
//
// This asks the question with nothing shared. When it is on, every box the descent settles in bulk
// walks its own cells afterwards and evaluates `Field::eval(root, cell centre)` at each — the
// finest level, no descent above it, the exact rule a single voxel is decided by including the
// thin-feature rescue — and records every cell whose own answer differs from what the box claimed.
// The evaluations are counted apart and go into no figure the sampler reports, so a gate taken
// with the audit on is the same gate.
//
// Off by default and one predictable branch per settled box when off.
struct BoxCellFault {
    i64 voxel[3]{};        // the disagreeing cell, in the sample's own voxel coordinates
    i64 box_low[3]{};      // the settled box's low corner, same coordinates
    i32 side[3]{};         // how many cells the settled box spans on each axis
    f64 centre_value = 0;  // what the box read at its own centre (the undisplaced shape)
    f64 cell_value = 0;    // what the cell reads at its own centre (the real shape)
    f64 radius = 0;        // the box's half-diagonal over its sample points
    f64 reach = 0;         // radius + the slack the box was charged
    f64 voxel_metres = 0;
    bool box_solid = false;  // the box said SOLID and the cell says air, or the reverse
    bool cell_rescued = false;
};

void set_box_cell_audit(bool on);
bool box_cell_audit();

// Everything found since the last call, and the counters beside it. Clears the list.
struct BoxCellAudit {
    std::vector<BoxCellFault> faults;
    u64 boxes_solid = 0;    // boxes settled SOLID in bulk and checked
    u64 boxes_empty = 0;    // boxes settled EMPTY in bulk and checked
    u64 cells_checked = 0;
    u64 solid_over_claimed = 0;   // cells the box filled that the per-cell rule would not have
    u64 empty_over_claimed = 0;   // cells the box cleared that the per-cell rule would have kept
};
BoxCellAudit take_box_cell_audit();

// Could this box hold any matter at all, at this resolution?
//
// A conservative answer: `true` means "sample it and find out", and only `false` is a promise. It
// is the same test the descent settles its very first box with — the field read once at the
// centre, against the half-diagonal, what displacement can hide, and the reach of the thin-feature
// rescue — so a box this says no to is a box `sample` would hand back filled with air.
//
// One field evaluation, and R11b needs it for a reason worth stating. Refining node by node means
// asking about thousands of boxes that hold nothing, and the cheap way to skip them is to ask the
// WORLD whether it has anything there — which is wrong, because the world at that moment holds a
// coarse approximation, and a feature the coarse pass was too blunt to find is a feature the skip
// would make permanent. Measured on `clips/sampler.clip`: 4,923 voxels of 1.43 million went
// missing that way. Asking the field instead cannot miss anything the sampler would have found,
// because it is the sampler's own argument.
bool box_may_hold_matter(const SamplePlan& plan, Vec3 low, Vec3 high, i32 voxels_per_metre);

// Fill a clip from a field.
//
// `jobs` may be null, in which case it runs on the calling thread. With a job system it splits
// by z slab, which is the axis a clip's memory is laid out along, so no two workers ever write
// the same cache line.
// Somebody to tell how far along the sampling is, called once per slab of work rather than per
// voxel — a store on a shared line at voxel rate would be its own performance problem.
//
// A plain function rather than anything the forge has to know the shape of. Sampling a building is
// nearly all of a cold load, so a loading bar that cannot see inside it has nothing to report for
// the fifty seconds that matter, and a bar that does not move for fifty seconds is not a bar.
using SampleWatcher = std::function<void(f64 fraction, u64 done, u64 expected)>;

SampleResult sample(const Field& field, u32 root, const std::vector<PaintRule>& paint,
                    const SampleSettings& settings, JobSystem* jobs = nullptr,
                    const SampleWatcher& watch = {});

// The same, over a plan that has already been worked out. This is the one a caller taking many
// samples from one clip wants; the signature above is this one with `plan_sample` in front of it.
SampleResult sample(const SamplePlan& plan, const SampleSettings& settings,
                    JobSystem* jobs = nullptr, const SampleWatcher& watch = {});

// Give every voxel its own version of its material.
//
// A second pass over the clip in place, replacing each solid voxel's type with a perturbed
// variant. The awkward part is that interning is a shared table and the perturbing is not, so it
// runs in three phases: every slab works out its own perturbations against a private table, the
// private tables are then interned into the real one on a single thread, and a last parallel pass
// swaps each voxel's private slot for the type id it turned into.
//
// The private tables cost a little duplicated work — two slabs that arrive at the same colour
// each hold it — and interning collapses that anyway, so the only thing lost is a few thousand
// wasted table entries. What is gained is that the expensive part, which is hashing sixty million
// voxels and deduplicating them, runs on every core instead of one.
struct VariationReport {
    u64 voxels = 0;
    u64 distinct_types = 0;
    u64 largest_group = 0;   // how many voxels share the most common record
    u64 reused = 0;          // voxels that had to share a record because the budget ran out

    // What each phase cost, in milliseconds. Reported rather than inferred, because the three do
    // very different work — one is parallel and arithmetic, one is serial and hash-bound, one is
    // parallel and memory-bound — and which of them dominates is not guessable.
    f64 perturb_ms = 0.0;
    f64 intern_ms = 0.0;
    f64 resolve_ms = 0.0;
    f64 uniqueness() const {
        return (voxels > 0) ? static_cast<f64>(distinct_types) / static_cast<f64>(voxels) : 0.0;
    }
};

// Whether a feature narrower than a voxel passes through this point, so that centre sampling does
// not delete it. Exposed because the sampler and its brute-force reference in the tests must apply
// the SAME rule -- the test compares the two, and a rule implemented in only one of them would make
// the comparison meaningless rather than strict.
//
// `evaluations`, when given, is increased by how many times the field was asked. It exists because
// this function's cost appeared in NO number the sampler reports: D613 widened the band of cells
// that reach it, the run got 37% slower, and the shape and paint core-milliseconds between them
// accounted for 8% of that. A cost that no instrument covers is this project's most repeated
// measurement fault (traps 17 and 18) and it does not need a third outing.
bool thin_feature_here(const Field& field, u32 root, Vec3 p, f64 outside_by, f64 voxel,
                       u64* evaluations = nullptr);

VariationReport apply_variation(Clip& clip, VoxelTypeTable& types, const Field& field,
                                const Variation& variation, const SampleSettings& settings,
                                const SampleResult& placed, JobSystem* jobs = nullptr);

}  // namespace forge
}  // namespace ws
