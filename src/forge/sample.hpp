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

#include <vector>

#include "core/types.hpp"
#include "forge/field.hpp"
#include "game/clip.hpp"
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
};

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
};

// Fill a clip from a field.
//
// `jobs` may be null, in which case it runs on the calling thread. With a job system it splits
// by z slab, which is the axis a clip's memory is laid out along, so no two workers ever write
// the same cache line.
SampleResult sample(const Field& field, u32 root, const std::vector<PaintRule>& paint,
                    const SampleSettings& settings, JobSystem* jobs = nullptr);

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

VariationReport apply_variation(Clip& clip, VoxelTypeTable& types, const Field& field,
                                const Variation& variation, const SampleSettings& settings,
                                const SampleResult& placed, JobSystem* jobs = nullptr);

}  // namespace forge
}  // namespace ws
