#pragma once
// The paint stack, exported so that the ◉ view can shade what it marches.
//
// documentation/24-clip-viewer.md §4b is what this is for. In one paragraph: the raw view draws the
// clip as it was written, ray-marched at infinite resolution, and until now every shape came out the
// same flat grey — because **a shape has no material in this language**. Colour comes from the paint
// stack: a list of rules of the form `paint <material> where=<field> below=... above=...
// facing=... at=...`, applied in order, each painting over the last, evaluated per point against
// fields that are usually not shapes at all. The marcher already has the one thing that makes
// running that stack possible — the true surface point and its normal, exactly, at no resolution —
// so the answer is to evaluate the rules there.
//
// Two chunks come out of here, and everything in this header exists to produce them.
//
//   FLDG   the field graph: every node a kept rule's `test` expression can reach, flattened into an
//          array a shader can walk. Op, child indices, the eight `a` doubles narrowed to floats,
//          and the box the field knows the node is contained in.
//   PANT   the rules themselves, in order: which FLDG node is the test, the band the value has to
//          fall in, the facing axis and its threshold, the material, some flags, and the box
//          outside which the rule cannot possibly apply.
//
// # The op numbers are the baker's own, and that is not a preference
//
// `src/forge/field.hpp` comes from whichever branch is being baked and `Op` is a plain enum whose
// values shift the moment anybody inserts a solid into the middle of the list — which is exactly
// what the branch that added `arc` did. `web_op` in bake_web.cpp already carries this scar for the
// shapes view. `field_op` below is the same decision made once more for a much larger table, and it
// **agrees with `web_op` on the eight solids `web_op` numbers**, so one GLSL `sdf()` can serve the
// shapes view and the paint field both. bake_web.cpp checks that agreement at startup rather than
// trusting this comment.
//
// # `Parameter` never reaches the file
//
// A `param` is a slot in the field's own table and a node that reads it. A shader has no such table,
// and shipping one for a number that cannot move in a baked clip would be machinery for nothing. So
// a `Parameter` node is written out as the `Constant` it currently evaluates to. That is exact for a
// baked clip by construction — the value is read from the same table `Field::eval` would have read.
//
// # `origin` moves the solid and the paint rules and NOT the names
//
// §3 of the clip viewer document records what that cost once already: a part came out sampled 3.5 m
// from its own matter. The same trap is in here twice and the two halves have opposite answers.
//
//   a rule's `test`   is moved. `apply_origin` wraps every rule's test in a translate, and
//                     `bake_root` gives a part the same shift before it walks its shapes. So the
//                     tests exported here are already in the space the viewer marches in, and
//                     nothing has to be done to them. The shift is visible in the dump as the
//                     outermost `translate` of every rule of a shifted clip.
//
//   a rule's `place`  is NOT. `apply_origin` translates `rule.test` and leaves `rule.place` where it
//                     was, because nothing had ever asked for one afterwards. For `facility.clip`,
//                     whose `origin` is 3.50 m, that puts every placed rule's box three and a half
//                     metres from the rule it belongs to. So the box derived from `place` is shifted
//                     by `origin_shift` HERE, before it is written. See `rule_region` below.
//
// # Why a box per rule at all
//
// A shader that walks a hundred rules of a deep graph per pixel does not run on a phone. Making that
// cheap is not this file's job, but giving the format what it needs to be made cheap is: the rules
// are in order, each carries the box outside which it cannot apply, and a flag says when a rule's
// test is a plain distance — the common case, and the one that can be rejected on a box.

#include <cmath>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "core/types.hpp"
#include "forge/field.hpp"
#include "forge/sample.hpp"
#include "world/voxel_type.hpp"

namespace ws {
namespace bake {

// --------------------------------------------------------------------------------------
// The numbering
// --------------------------------------------------------------------------------------

// 0..7 are `web_op`'s, unchanged and unchangeable: the shapes view already ships them.
enum : u32 {
    kOpSphere = 0,
    kOpBox = 1,
    kOpCylinder = 2,
    kOpCapsule = 3,
    kOpTorus = 4,
    kOpCone = 5,
    kOpPlane = 6,
    kOpEllipsoid = 7,
    // the rest of the solids
    kOpArc = 8,
    kOpPrism = 9,
    kOpPlatonic = 10,
    kOpWedge = 11,
    kOpStairs = 12,
    kOpSpiral = 13,
    // constants and coordinates
    kOpConstant = 14,
    kOpCoordinate = 15,   // the clip file calls this `axis`
    kOpRadius = 16,       // the clip file calls this `distance`
    // combining
    kOpUnion = 17,
    kOpIntersection = 18,
    kOpDifference = 19,
    kOpSmoothUnion = 20,
    kOpSmoothIntersection = 21,
    kOpSmoothDifference = 22,
    // moving the point
    kOpTranslate = 23,
    kOpRotate = 24,
    kOpScale = 25,
    kOpMirror = 26,
    kOpRepeat = 27,
    kOpPolarRepeat = 28,
    kOpRevolve = 29,
    // changing the answer
    kOpShell = 30,
    kOpRound = 31,
    kOpOffset = 32,
    kOpDisplace = 33,
    kOpTwist = 34,
    kOpBend = 35,
    // patterns
    kOpSine = 36,
    kOpWaves = 37,
    kOpNoise = 38,
    kOpFbm = 39,
    kOpRidged = 40,
    kOpRasp = 41,
    kOpCells = 42,
    kOpCellEdge = 43,
    kOpChecker = 44,
    kOpStripes = 45,
    kOpBricks = 46,
    // what the shape is doing here
    kOpCurvature = 47,
    kOpOcclusion = 48,
    kOpFacing = 49,
    // arithmetic
    kOpAdd = 50,
    kOpMultiply = 51,
    kOpMin = 52,
    kOpMax = 53,
    kOpBlend = 54,
    kOpRemap = 55,
    kOpAbs = 56,
    kOpNegate = 57,
    kOpStep = 58,
    kOpSmoothstep = 59,
    kOpClamp = 60,
    kOpPower = 61,

    kOpCount = 62,
    kOpUnknown = 0xFFFFFFFFu,
};

// Every op the language has, numbered. `Parameter` is deliberately absent: it is folded into a
// constant on the way out and can never appear in a file.
inline u32 field_op(forge::Op op) {
    using forge::Op;
    switch (op) {
        case Op::Sphere: return kOpSphere;
        case Op::Box: return kOpBox;
        case Op::Cylinder: return kOpCylinder;
        case Op::Capsule: return kOpCapsule;
        case Op::Torus: return kOpTorus;
        case Op::Cone: return kOpCone;
        case Op::Plane: return kOpPlane;
        case Op::Ellipsoid: return kOpEllipsoid;
        case Op::Arc: return kOpArc;
        case Op::Prism: return kOpPrism;
        case Op::Platonic: return kOpPlatonic;
        case Op::Wedge: return kOpWedge;
        case Op::Stairs: return kOpStairs;
        case Op::Spiral: return kOpSpiral;
        case Op::Constant: return kOpConstant;
        case Op::Parameter: return kOpConstant;   // folded; see the header comment
        case Op::Coordinate: return kOpCoordinate;
        case Op::Radius: return kOpRadius;
        case Op::Union: return kOpUnion;
        case Op::Intersection: return kOpIntersection;
        case Op::Difference: return kOpDifference;
        case Op::SmoothUnion: return kOpSmoothUnion;
        case Op::SmoothIntersection: return kOpSmoothIntersection;
        case Op::SmoothDifference: return kOpSmoothDifference;
        case Op::Translate: return kOpTranslate;
        case Op::Rotate: return kOpRotate;
        case Op::Scale: return kOpScale;
        case Op::Mirror: return kOpMirror;
        case Op::Repeat: return kOpRepeat;
        case Op::PolarRepeat: return kOpPolarRepeat;
        case Op::Revolve: return kOpRevolve;
        case Op::Shell: return kOpShell;
        case Op::Round: return kOpRound;
        case Op::Offset: return kOpOffset;
        case Op::Displace: return kOpDisplace;
        case Op::Twist: return kOpTwist;
        case Op::Bend: return kOpBend;
        case Op::Sine: return kOpSine;
        case Op::Waves: return kOpWaves;
        case Op::Noise: return kOpNoise;
        case Op::Fbm: return kOpFbm;
        case Op::Ridged: return kOpRidged;
        case Op::Rasp: return kOpRasp;
        case Op::Cells: return kOpCells;
        case Op::CellEdge: return kOpCellEdge;
        case Op::Checker: return kOpChecker;
        case Op::Stripes: return kOpStripes;
        case Op::Bricks: return kOpBricks;
        case Op::Curvature: return kOpCurvature;
        case Op::Occlusion: return kOpOcclusion;
        case Op::Facing: return kOpFacing;
        case Op::Add: return kOpAdd;
        case Op::Multiply: return kOpMultiply;
        case Op::Min: return kOpMin;
        case Op::Max: return kOpMax;
        case Op::Blend: return kOpBlend;
        case Op::Remap: return kOpRemap;
        case Op::Abs: return kOpAbs;
        case Op::Negate: return kOpNegate;
        case Op::Step: return kOpStep;
        case Op::Smoothstep: return kOpSmoothstep;
        case Op::Clamp: return kOpClamp;
        case Op::Power: return kOpPower;
    }
    return kOpUnknown;
}

// For the dump, and for a warning that names the op rather than its number. The words are the clip
// file's own where the two differ, because the dump is checked against a `.clip` by hand.
inline const char* field_op_name(u32 op) {
    switch (op) {
        case kOpSphere: return "sphere";
        case kOpBox: return "box";
        case kOpCylinder: return "cylinder";
        case kOpCapsule: return "capsule";
        case kOpTorus: return "torus";
        case kOpCone: return "cone";
        case kOpPlane: return "plane";
        case kOpEllipsoid: return "ellipsoid";
        case kOpArc: return "arc";
        case kOpPrism: return "prism";
        case kOpPlatonic: return "platonic";
        case kOpWedge: return "wedge";
        case kOpStairs: return "stairs";
        case kOpSpiral: return "spiral";
        case kOpConstant: return "constant";
        case kOpCoordinate: return "axis";
        case kOpRadius: return "distance";
        case kOpUnion: return "union";
        case kOpIntersection: return "intersection";
        case kOpDifference: return "difference";
        case kOpSmoothUnion: return "smooth_union";
        case kOpSmoothIntersection: return "smooth_intersection";
        case kOpSmoothDifference: return "smooth_difference";
        case kOpTranslate: return "translate";
        case kOpRotate: return "rotate";
        case kOpScale: return "scale";
        case kOpMirror: return "mirror";
        case kOpRepeat: return "repeat";
        case kOpPolarRepeat: return "around";
        case kOpRevolve: return "revolve";
        case kOpShell: return "shell";
        case kOpRound: return "round";
        case kOpOffset: return "offset";
        case kOpDisplace: return "displace";
        case kOpTwist: return "twist";
        case kOpBend: return "bend";
        case kOpSine: return "sine";
        case kOpWaves: return "waves";
        case kOpNoise: return "noise";
        case kOpFbm: return "fbm";
        case kOpRidged: return "ridged";
        case kOpRasp: return "rasp";
        case kOpCells: return "cells";
        case kOpCellEdge: return "cell_edge";
        case kOpChecker: return "checker";
        case kOpStripes: return "stripes";
        case kOpBricks: return "bricks";
        case kOpCurvature: return "curvature";
        case kOpOcclusion: return "occlusion";
        case kOpFacing: return "facing";
        case kOpAdd: return "add";
        case kOpMultiply: return "multiply";
        case kOpMin: return "min";
        case kOpMax: return "max";
        case kOpBlend: return "blend";
        case kOpRemap: return "remap";
        case kOpAbs: return "abs";
        case kOpNegate: return "negate";
        case kOpStep: return "step";
        case kOpSmoothstep: return "smoothstep";
        case kOpClamp: return "clamp";
        case kOpPower: return "power";
        default: return "?";
    }
}

// An op that asks its child about more than one point. `curvature` reads seven, `occlusion`
// fourteen, `facing` six, `repeat` up to eight; a swept op re-parameterises the point before it
// asks. None of them is wrong in a shader and every one of them multiplies what a rule costs, so a
// rule that reaches one says so in its flags and the budget conversation has a number to start from.
inline bool field_op_is_costly(u32 op) {
    switch (op) {
        case kOpCurvature:
        case kOpOcclusion:
        case kOpFacing:
        case kOpRepeat:
        case kOpPolarRepeat:
        case kOpRevolve:
        case kOpSpiral:
        case kOpTwist:
        case kOpBend:
            return true;
        default:
            return false;
    }
}

// --------------------------------------------------------------------------------------
// The records
// --------------------------------------------------------------------------------------

// FLDG, one node: 80 bytes.
//
// `child[i]` for i >= `children` is zero and means nothing. Child indices are always LESS than the
// node's own index — the field guarantees it ("children always have a lower index than their
// parent", `Field::build_bounds`) and the compaction here preserves it, which is checked rather
// than assumed.
//
// `lo`/`hi` is the box the field knows this node is contained in, in the space the node is
// evaluated in — which is the space its parent hands it, exactly as `eval` descends. A coordinate at
// or beyond ±1e29 means "everywhere", which is what a pattern always says.
struct FieldNode {
    u32 op = kOpConstant;
    u32 children = 0;
    u32 child[4]{0, 0, 0, 0};
    f32 a[8]{};
    f32 lo[3]{0, 0, 0};
    f32 hi[3]{0, 0, 0};
};

// PANT, one rule: 52 bytes.
//
// **This is 24 bytes longer than the interface four other agents stubbed against**, and the extra
// 24 are appended at the end so that every field they do read is at the offset they expect. The
// addition is `lo`/`hi`, the box outside which the rule cannot apply, which the brief asked for and
// the stub interface had nowhere to put.
//
// The band reads `above <= value <= below`, which is the clip file's own words: `paint moss
// where=grain above=0.55` accepts a value of 0.55 or more, and `below=0.02` accepts 0.02 or less.
// So `above` is `PaintRule::low` and `below` is `PaintRule::high`, and they are stored in the
// interface's order, `below` first.
//
// `facing_axis` is **-1 when the rule does not ask**, not the engine's 3. It is the one signed field
// in either chunk and -1 is what a signed axis means; `kRuleFacing` in the flags says the same thing
// again so that a shader need never test the sentinel.
//
// The facing test is not a symmetric one and it is easy to get backwards. The engine's own:
//
//     component = dot(normal, positive axis direction)
//     facing_at >= 0   ->   keep where component >= facing_at      "up-facing"
//     facing_at <  0   ->   keep where component <= facing_at      "down-facing"
struct PaintRecord {
    u32 node = 0;          // index into FLDG
    f32 below = 1e30f;     // PaintRule::high
    f32 above = -1e30f;    // PaintRule::low
    i32 facing_axis = -1;
    f32 facing_at = 0.5f;
    u32 material = 0;      // index into the clip's material table
    u32 flags = 0;
    f32 lo[3]{-1e30f, -1e30f, -1e30f};
    f32 hi[3]{1e30f, 1e30f, 1e30f};
};

enum : u32 {
    // The test is a distance in metres — `Field::metric_slack` says so. The common case, the cheap
    // case, and the only one whose box means anything.
    kRuleMetric = 1u << 0,
    // The test node's own box is finite.
    kRuleBounded = 1u << 1,
    // The rule asks which way the surface faces.
    kRuleFacing = 1u << 2,
    // `lo`/`hi` is a real bound and not the whole world.
    kRuleBoxed = 1u << 3,
    // The rule was written with `on=`, so its box came from a named place rather than from its own
    // test. Carried because it changes what the box means, not what it is.
    kRulePlaced = 1u << 4,
    // Somewhere under this rule is an op that asks its child about more than one point. See
    // `field_op_is_costly`.
    kRuleCostly = 1u << 5,
    // The first rule of the stack, which is the undercoat: the sampler falls back to it for any
    // voxel that has matter and matched nothing (`paint_solid` in src/forge/sample.cpp). It is
    // never pruned, so this flag is always on rule 0 and nowhere else.
    kRuleUndercoat = 1u << 6,
};

constexpr usize kFieldNodeBytes = 4 + 4 + 16 + 32 + 12 + 12;   // 80
constexpr usize kPaintRuleBytes = 4 + 4 + 4 + 4 + 4 + 4 + 4 + 12 + 12;   // 52

// --------------------------------------------------------------------------------------
// The export
// --------------------------------------------------------------------------------------

struct PaintExport {
    std::vector<FieldNode> nodes;
    std::vector<PaintRecord> rules;

    usize rules_in = 0;        // how many the clip actually wrote
    usize rules_pruned = 0;    // and how many could not reach the part being baked
    usize rules_costly = 0;
    usize rules_placed = 0;
    usize unknown_ops = 0;     // an op with no number; zero, or the numbering is behind the enum
    usize deepest = 0;         // the longest path from a rule's root to a leaf
    usize order_faults = 0;    // a child that did not come before its parent; zero or this is broken

    usize field_bytes() const { return 4 + nodes.size() * kFieldNodeBytes; }
    usize paint_bytes() const { return 4 + rules.size() * kPaintRuleBytes; }
};

namespace detail {

inline void push_u32(std::vector<u8>& out, u32 value) {
    out.push_back(static_cast<u8>(value & 0xFFu));
    out.push_back(static_cast<u8>((value >> 8) & 0xFFu));
    out.push_back(static_cast<u8>((value >> 16) & 0xFFu));
    out.push_back(static_cast<u8>((value >> 24) & 0xFFu));
}

inline void push_f32(std::vector<u8>& out, f32 value) {
    u32 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    push_u32(out, bits);
}

// A double on its way to a float, with the two infinities the field uses kept recognisable rather
// than turned into `inf`. ±1e30 survives f32 (its range reaches 3.4e38) and every reader in this
// project already tests against 1e29.
inline f32 narrow(f64 value) {
    if (value > 1e30) return 1e30f;
    if (value < -1e30) return -1e30f;
    if (!std::isfinite(value)) return (value > 0.0) ? 1e30f : -1e30f;
    return static_cast<f32>(value);
}

}  // namespace detail

// FLDG: `u32 nodeCount`, then that many 80-byte nodes.
inline std::vector<u8> field_chunk(const PaintExport& made) {
    std::vector<u8> out;
    out.reserve(made.field_bytes());
    detail::push_u32(out, static_cast<u32>(made.nodes.size()));
    for (const FieldNode& node : made.nodes) {
        detail::push_u32(out, node.op);
        detail::push_u32(out, node.children);
        for (u32 i = 0; i < 4; ++i) detail::push_u32(out, node.child[i]);
        for (u32 i = 0; i < 8; ++i) detail::push_f32(out, node.a[i]);
        for (u32 i = 0; i < 3; ++i) detail::push_f32(out, node.lo[i]);
        for (u32 i = 0; i < 3; ++i) detail::push_f32(out, node.hi[i]);
    }
    return out;
}

// PANT: `u32 ruleCount`, then that many 52-byte rules, in the order the clip wrote them.
inline std::vector<u8> paint_chunk(const PaintExport& made) {
    std::vector<u8> out;
    out.reserve(made.paint_bytes());
    detail::push_u32(out, static_cast<u32>(made.rules.size()));
    for (const PaintRecord& rule : made.rules) {
        detail::push_u32(out, rule.node);
        detail::push_f32(out, rule.below);
        detail::push_f32(out, rule.above);
        detail::push_u32(out, static_cast<u32>(rule.facing_axis));
        detail::push_f32(out, rule.facing_at);
        detail::push_u32(out, rule.material);
        detail::push_u32(out, rule.flags);
        for (u32 i = 0; i < 3; ++i) detail::push_f32(out, rule.lo[i]);
        for (u32 i = 0; i < 3; ++i) detail::push_f32(out, rule.hi[i]);
    }
    return out;
}

// The box outside which a rule cannot possibly apply, or "everywhere" when nothing sound can be
// said. Two arguments and no third, because a third would have to be about how the weathering
// happens to be written and this must hold for a rule anybody types.
//
//   a placed rule   `on=<shape>` confines the coat: every test it generates is pushed out of its own
//                   range wherever the shape is not, which is what `only_here` in clip_script.cpp
//                   does. So outside the place shape's box the rule cannot apply. The box is grown
//                   by the whole-clip displacement for the reason `plan_sample` grows it — that is
//                   what can carry a point out of the shape it belongs to — and **shifted by
//                   `origin`, which the engine does not do to `place`**.
//
//   a rule accepted BELOW zero   `below=0.02` and no lower bound is "inside this shape, or within
//                   two centimetres of it". Outside the test's own box the distance is positive and
//                   at least the distance to that box, so the rule is false there. Grown by the
//                   accepted upper bound and by the slack, exactly as `plan_sample` does it.
//
// A rule bounded from BELOW instead — `above=0.55` on a grain — is the complement of a shape and can
// be true anywhere the shape is not, which is most of the world. Those get no box, and saying so is
// the whole of the honesty here: a wrong box is a rule that silently stops painting.
inline bool rule_region(const forge::Field& field, const forge::PaintRule& rule, f64 amplitude,
                        const f64 origin_shift[3], f64 lo[3], f64 hi[3]) {
    forge::Field::Aabb box;
    f64 shift[3] = {0.0, 0.0, 0.0};
    f64 grow = 0.0;

    if (rule.has_place) {
        box = field.bounds_of(rule.place);
        if (box.infinite()) return false;
        grow = amplitude;
        shift[0] = origin_shift[0];
        shift[1] = origin_shift[1];
        shift[2] = origin_shift[2];
    } else if (rule.high < 1e29 && rule.low <= -1e29) {
        box = field.bounds_of(rule.test);
        if (box.infinite()) return false;
        const f64 metric = field.metric_slack(rule.test);
        const bool settleable = metric < forge::Field::kInfiniteSlack;
        grow = std::max(0.0, rule.high) + (settleable ? metric : amplitude);
    } else {
        return false;
    }

    const f64 low[3] = {box.low.x, box.low.y, box.low.z};
    const f64 high[3] = {box.high.x, box.high.y, box.high.z};
    for (u32 axis = 0; axis < 3; ++axis) {
        lo[axis] = low[axis] + shift[axis] - grow;
        hi[axis] = high[axis] + shift[axis] + grow;
    }
    return true;
}

// Walk the paint stack and build both chunks.
//
// `region_low`/`region_high` is where the viewer can ever put a hit point — the union of the boxes
// the shapes view marches inside. A rule whose own box misses that entirely is pruned, which is the
// same argument the sampler makes when it skips a rule for a box (`descend` in src/forge/sample.cpp)
// and is sound for the same reason. **Rule 0 is never pruned**: it is the undercoat the sampler
// falls back to when nothing matched, and pruning it would change what an unpainted surface is.
//
// `material_index` turns a `VoxelTypeId` into an index in the clip's own material table. It is the
// mesher's own interning, so a rule and a quad that name the same matter name the same record.
inline PaintExport build_paint_export(const forge::Field& field,
                                      const std::vector<forge::PaintRule>& paint,
                                      f64 amplitude, const f64 origin_shift[3],
                                      const f64 region_low[3], const f64 region_high[3],
                                      const std::function<u32(VoxelTypeId)>& material_index) {
    PaintExport made;
    made.rules_in = paint.size();
    if (paint.empty()) return made;

    // ---- which rules survive ------------------------------------------------------------------
    struct Kept {
        usize index = 0;
        f64 lo[3]{-1e30, -1e30, -1e30};
        f64 hi[3]{1e30, 1e30, 1e30};
        bool boxed = false;
    };
    std::vector<Kept> kept;
    kept.reserve(paint.size());
    for (usize i = 0; i < paint.size(); ++i) {
        Kept entry;
        entry.index = i;
        entry.boxed = rule_region(field, paint[i], amplitude, origin_shift, entry.lo, entry.hi);
        if (entry.boxed && i > 0) {
            bool reaches = true;
            for (u32 axis = 0; axis < 3; ++axis) {
                if (entry.hi[axis] < region_low[axis] || entry.lo[axis] > region_high[axis]) {
                    reaches = false;
                }
            }
            if (!reaches) {
                ++made.rules_pruned;
                continue;
            }
        }
        kept.push_back(entry);
    }

    // ---- which nodes those rules can reach ----------------------------------------------------
    //
    // Marked by a walk from every kept rule's test, then emitted in increasing field order, which
    // keeps the field's own "children before parents" and so gives the shader child < parent for
    // nothing.
    std::vector<u8> wanted(field.size(), 0);
    std::vector<u32> stack;
    for (const Kept& entry : kept) {
        const u32 root = paint[entry.index].test;
        if (root >= field.size()) continue;
        stack.push_back(root);
        while (!stack.empty()) {
            const u32 at = stack.back();
            stack.pop_back();
            if (at >= field.size() || wanted[at]) continue;
            wanted[at] = 1;
            const forge::Node& node = field.node(at);
            for (u32 c = 0; c < node.children && c < 4; ++c) stack.push_back(node.child[c]);
        }
    }

    std::vector<u32> where(field.size(), 0xFFFFFFFFu);
    for (u32 at = 0; at < static_cast<u32>(field.size()); ++at) {
        if (!wanted[at]) continue;
        const forge::Node& source = field.node(at);
        FieldNode node;
        node.op = field_op(source.op);
        if (node.op == kOpUnknown) {
            ++made.unknown_ops;
            node.op = kOpConstant;   // a number, not a wrong shape; the count says it happened
        }
        node.children = (source.op == forge::Op::Parameter) ? 0u : std::min(source.children, 4u);
        for (u32 c = 0; c < node.children; ++c) {
            const u32 child = source.child[c];
            node.child[c] = (child < where.size()) ? where[child] : 0u;
            if (child >= at || node.child[c] == 0xFFFFFFFFu) {
                ++made.order_faults;
                node.child[c] = 0;
            }
        }
        if (source.op == forge::Op::Parameter) {
            // The slot's current value, read the way `Field::eval` reads it.
            const usize slot = static_cast<usize>(source.a[0]);
            const f64 value = (slot < field.parameter_count()) ? field.parameter_value(slot) : 0.0;
            node.a[0] = detail::narrow(value);
        } else {
            for (u32 i = 0; i < 8; ++i) node.a[i] = detail::narrow(source.a[i]);
        }
        const forge::Field::Aabb box = field.bounds_of(at);
        node.lo[0] = detail::narrow(box.low.x);
        node.lo[1] = detail::narrow(box.low.y);
        node.lo[2] = detail::narrow(box.low.z);
        node.hi[0] = detail::narrow(box.high.x);
        node.hi[1] = detail::narrow(box.high.y);
        node.hi[2] = detail::narrow(box.high.z);

        where[at] = static_cast<u32>(made.nodes.size());
        made.nodes.push_back(node);
    }

    // ---- how deep each expression is, and what it reaches -------------------------------------
    //
    // One forward pass, not one walk per rule. Children come before parents, so a node's depth and
    // whether anything under it asks its child about more than one point are both known by the time
    // the node itself is reached. Written the other way round it is a walk per rule over a shared
    // graph, which on the facility is a hundred and thirty-nine walks of the same fifteen thousand
    // nodes.
    std::vector<u32> depth_of(field.size(), 0);
    std::vector<u8> costly_under(field.size(), 0);
    for (u32 at = 0; at < static_cast<u32>(field.size()); ++at) {
        if (!wanted[at]) continue;
        const forge::Node& node = field.node(at);
        u32 under = 0;
        u8 costly = field_op_is_costly(field_op(node.op)) ? 1u : 0u;
        for (u32 c = 0; c < node.children && c < 4; ++c) {
            const u32 child = node.child[c];
            if (child >= depth_of.size()) continue;
            under = std::max(under, depth_of[child] + 1u);
            costly = static_cast<u8>(costly | costly_under[child]);
        }
        depth_of[at] = under;
        costly_under[at] = costly;
    }

    // ---- the rules ---------------------------------------------------------------------------
    for (usize k = 0; k < kept.size(); ++k) {
        const forge::PaintRule& source = paint[kept[k].index];
        PaintRecord rule;
        const u32 mapped = (source.test < where.size()) ? where[source.test] : 0xFFFFFFFFu;
        rule.node = (mapped == 0xFFFFFFFFu) ? 0u : mapped;
        rule.below = detail::narrow(source.high);
        rule.above = detail::narrow(source.low);
        rule.facing_axis = (source.facing_axis < 3) ? static_cast<i32>(source.facing_axis) : -1;
        rule.facing_at = detail::narrow(source.facing_min);
        rule.material = material_index(source.type);

        if (field.metric_slack(source.test) < forge::Field::kInfiniteSlack) {
            rule.flags |= kRuleMetric;
        }
        if (!field.bounds_of(source.test).infinite()) rule.flags |= kRuleBounded;
        if (source.facing_axis < 3) rule.flags |= kRuleFacing;
        if (source.has_place) {
            rule.flags |= kRulePlaced;
            ++made.rules_placed;
        }
        if (kept[k].boxed) {
            rule.flags |= kRuleBoxed;
            for (u32 axis = 0; axis < 3; ++axis) {
                rule.lo[axis] = detail::narrow(kept[k].lo[axis]);
                rule.hi[axis] = detail::narrow(kept[k].hi[axis]);
            }
        }
        if (k == 0) rule.flags |= kRuleUndercoat;

        if (source.test < depth_of.size()) {
            made.deepest = std::max(made.deepest, static_cast<usize>(depth_of[source.test]));
            if (costly_under[source.test] != 0) {
                rule.flags |= kRuleCostly;
                ++made.rules_costly;
            }
        }

        made.rules.push_back(rule);
    }

    return made;
}

}  // namespace bake
}  // namespace ws
