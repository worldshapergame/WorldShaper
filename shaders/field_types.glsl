// The field, as the card reads it — R12a's record and R12b's contract.
//
// `src/forge/field.hpp` says, in the block above `class Field`, that the node array is shaped so
// that it can be evaluated somewhere else: *"nodes are plain data of a fixed size with no pointers,
// and evaluation is a switch over them with an explicit stack of at most a handful of entries.
// That is a shape that transliterates to a compute shader without changing."* This file is the
// other end of that sentence.
//
// # Three files, one evaluator, and why they are three
//
//   field_types.glsl   this file: the records, the buffers, the op numbers, the prototypes
//   field_leaf.glsl    `field_leaf(at, p)` — the ops with no children. Primitives and grain
//   field_walk.glsl    `field_eval(root, p)` — the stack machine over everything else
//
// Split because a leaf is arithmetic and the walk is control flow, and the two fail in completely
// different ways: a wrong primitive is a shape that is wrong everywhere it appears, and a wrong
// walk is a shape that is wrong only where it is nested under something. Keeping them apart means
// `--gpu-field-check` can say which of the two it is.
//
// # The one rule that governs every line of all three
//
// **This is the second evaluator and it must agree with the first.** `documentation/21-renderer-
// rewrite.md` D200k: *"One field evaluator, mirrored, or two renderers compute the world two ways."*
// The CPU's own second evaluator, `Field::mirror_eval`, was written first precisely so that this
// one is a transliteration of something already proved rather than a fresh attempt — it walks the
// same nodes with the same explicit stack and agrees with `Field::eval` to one ulp (D644).
//
// So: when this file and `forge/field.cpp` disagree, `forge/field.cpp` is right. Every op here is
// the same arithmetic in the same order, and where an order looks arbitrary it is not — see
// `Op::Scale`, whose smallest factor is applied on the way OUT, which field.hpp names as the half a
// transliteration is most likely to drop.
//
// # Single precision, and the measurement that allowed it
//
// Every argument, every point and every answer here is `float` where the CPU carries `double`.
// **D676 measured that and it is enough**: `--field-single` walks the estate on a grid projected
// onto the isosurface and reports 0 sign changes over 44,084 points, 10,683 of them within half a
// voxel of a surface, worst difference 0.47 µm against a 31.25 mm voxel. The error is a function of
// DISTANCE FROM THE ORIGIN rather than of graph depth — 2.25e-7 m in a 12 m box against 4.35e-6 m
// out at 63 m — so this stays safe while a world stays within kilometres of its origin, and it
// degrades linearly in that and not in how complicated a clip is.

#ifndef WS_FIELD_TYPES_GLSL
#define WS_FIELD_TYPES_GLSL

// ---------------------------------------------------------------------------------------------
// The op numbers. These MUST match `enum class ws::forge::Op` in src/forge/field.hpp, in order.
//
// Held to that by a test rather than by care: `tests/test_field_gpu.cpp` reads these defines out of
// this file and asserts each against the C++ enumerator of the same name. Three copies of one
// number is what D204 is about, and the copy on the card is the one nobody can put a breakpoint in.
// ---------------------------------------------------------------------------------------------
#define WS_OP_CONSTANT             0u
#define WS_OP_PARAMETER            1u
#define WS_OP_COORDINATE           2u
#define WS_OP_RADIUS               3u
#define WS_OP_SPHERE               4u
#define WS_OP_BOX                  5u
#define WS_OP_CYLINDER             6u
#define WS_OP_CAPSULE              7u
#define WS_OP_TORUS                8u
#define WS_OP_ARC                  9u
#define WS_OP_CONE                10u
#define WS_OP_PLANE               11u
#define WS_OP_ELLIPSOID           12u
#define WS_OP_PRISM               13u
#define WS_OP_PLATONIC            14u
#define WS_OP_WEDGE               15u
#define WS_OP_STAIRS              16u
#define WS_OP_REVOLVE             17u
#define WS_OP_SPIRAL              18u
#define WS_OP_UNION               19u
#define WS_OP_INTERSECTION        20u
#define WS_OP_DIFFERENCE          21u
#define WS_OP_SMOOTH_UNION        22u
#define WS_OP_SMOOTH_DIFFERENCE   23u
#define WS_OP_SMOOTH_INTERSECTION 24u
#define WS_OP_CHAMFER_UNION       25u
#define WS_OP_CHAMFER_DIFFERENCE  26u
#define WS_OP_CHAMFER_INTERSECTION 27u
#define WS_OP_TRANSLATE           28u
#define WS_OP_ROTATE              29u
#define WS_OP_SCALE               30u
#define WS_OP_MIRROR              31u
#define WS_OP_REPEAT              32u
#define WS_OP_POLAR_REPEAT        33u
#define WS_OP_SCATTER             34u
#define WS_OP_SHELL               35u
#define WS_OP_ROUND               36u
#define WS_OP_OFFSET              37u
#define WS_OP_DISPLACE            38u
#define WS_OP_TWIST               39u
#define WS_OP_BEND                40u
#define WS_OP_SINE                41u
#define WS_OP_WAVES               42u
#define WS_OP_NOISE               43u
#define WS_OP_FBM                 44u
#define WS_OP_RIDGED              45u
#define WS_OP_RASP                46u
#define WS_OP_CELLS               47u
#define WS_OP_CELL_EDGE           48u
#define WS_OP_CURVATURE           49u
#define WS_OP_OCCLUSION           50u
#define WS_OP_FACING              51u
#define WS_OP_CHECKER             52u
#define WS_OP_STRIPES             53u
#define WS_OP_BRICKS              54u
#define WS_OP_ADD                 55u
#define WS_OP_MULTIPLY            56u
#define WS_OP_MIN                 57u
#define WS_OP_MAX                 58u
#define WS_OP_BLEND               59u
#define WS_OP_REMAP               60u
#define WS_OP_ABS                 61u
#define WS_OP_NEGATE              62u
#define WS_OP_STEP                63u
#define WS_OP_SMOOTHSTEP          64u
#define WS_OP_CLAMP               65u
#define WS_OP_POWER               66u
#define WS_OP_COUNT               67u

// ---------------------------------------------------------------------------------------------
// The records. Scalars only, so std430 lays them out with a stride of four and the C++ structs in
// `src/gpu/field_gpu.hpp` match member for member with no padding to reason about. A `vec3` in a
// storage buffer aligns to sixteen and would put a hole in the middle of every one of these.
// ---------------------------------------------------------------------------------------------

// One node of the field: what it does, what it does it to, and the box it is known to fit in.
//
// `lo`/`hi` are `Field::bounds_of`, and they are here for the same reason the CPU has them: a union
// of thirty parts costs thirty evaluations at every point, and at all but a handful of points
// twenty-nine of them are answering about something metres away. A node whose box cannot be worked
// out carries the infinite one, which never culls and is always correct.
struct FieldNode {
    uint op;
    uint children;
    uint child[4];
    float a[8];
    float lo[3];
    float hi[3];
};

// A paint rule, and the box outside which it cannot apply.
//
// `place` is NOT here, and that is not an omission: a rule's place shape is folded into `lo`/`hi`
// (and into the piece list the CPU keeps) by `forge::plan_sample`, and is never evaluated per
// voxel. See the `has_place` block in `src/forge/sample.cpp`.
//
// The bands are the WIDENED ones — `SamplePlan::widened`, grown by what a displacement can move a
// surface — because those are what the CPU descent actually tests against.
//
// **`slack` is not decoration and it decides whether `facing` is asked at all.** In `descend`'s
// settle loop a rule with a finite slack is settled to yes-or-no from its band, and `paint_solid`
// then applies a settled rule WITHOUT looking at its facing; only a rule the descent could settle
// for nothing — `rule_slack >= kInfiniteSlack`, which is what a pattern-keyed rule has — reaches
// the branch that takes a normal. Copying that exactly is the difference between agreeing with the
// CPU sampler and being a second renderer computing the world a second way.
struct GpuPaintRule {
    uint test;
    uint type;
    uint facing_axis;   // 3 means "do not ask", which is the default and costs nothing
    float facing_min;
    float low;
    float high;
    float slack;        // SamplePlan::rule_slack; >= 1e30 means "asked at every solid voxel"
    uint piece_from;    // [from, to) into rule_pieces, empty when the zone is one piece
    uint piece_to;
    uint pad;
    float lo[3];
    float hi[3];
};

// A bare box, for the pieces a placed rule's zone is made of.
//
// One box round a union of two distant pieces is a box round everything between them, and a
// weathering zone is exactly that — the sunny zone is the great steps at the ground and the cornice
// wash at the top, so its bounding box is the whole building and rejects nothing.
struct GpuBox {
    float lo[3];
    float hi[3];
};

// One node of the render tree to fill: where its first voxel sits on the sample grid, and how big
// a voxel is. Always `kNodeVoxels` cubed — eight a side at every level, which is what makes the
// dispatch a flat index with no prefix sum.
struct GpuSampleBox {
    int lo[3];
    float voxel;
};

layout(std430, set = 0, binding = 0) readonly buffer FieldNodes { FieldNode items[]; } field_nodes;
layout(std430, set = 0, binding = 1) readonly buffer FieldParams { float items[]; } field_params;
layout(std430, set = 0, binding = 2) readonly buffer PaintRules { GpuPaintRule items[]; } paint_rules;
layout(std430, set = 0, binding = 3) readonly buffer SampleBoxes { GpuSampleBox items[]; } sample_boxes;
layout(std430, set = 0, binding = 4) writeonly buffer OutTypes { uint items[]; } out_types;
layout(std430, set = 0, binding = 5) writeonly buffer OutInside { uint items[]; } out_inside;
layout(std430, set = 0, binding = 6) readonly buffer RulePieces { GpuBox items[]; } rule_pieces;

layout(push_constant) uniform FieldPush {
    uint root;          // the solid, displacement and all
    uint bounds;        // the shape saying which cells are part of the clip; see has_bounds
    uint has_bounds;
    uint rule_count;
    uint box_count;
    uint first_type;    // paint.front().type — what a solid cell no rule matched is given
    float half_cell;    // kHalfCellDiagonal, the reach of the thin-feature rescue
    uint flags;
} field_push;

#define WS_FIELD_FLAG_NO_RESCUE 1u      // --no-gpu-rescue: the thin-feature rescue off, as a control
#define WS_FIELD_FLAG_COUNT_VISITS 2u   // --gpu-visits: count nodes walked instead of building a world

// How many nodes the walk has stepped through for this cell, when WS_FIELD_FLAG_COUNT_VISITS is on.
//
// An INSTRUMENT and not a feature, and it exists because the first attempt to make this shader
// faster was about to be a guess. Cost here is (nodes walked) x (what a step costs), those two are
// fixed by completely different things -- the shape of the clip's expression on one side, the stack
// and the divergence on the other -- and no timing can tell them apart. This counts one of them so
// the other can be divided out.
uint ws_field_visits;

// ---------------------------------------------------------------------------------------------
// The stack.
//
// One frame per node on the current path. **The depth is measured, not chosen**: D676 walked the
// estate and the deepest path reached is 80, and what grows it is the number of BUILDINGS rather
// than the complexity of any one of them — each is `part_x = translate { x_assembly }` joining one
// union of seven, so every painted shape carries a translated twin. 96 is that measurement with the
// same headroom `Field::kMirrorStack` carries, and it is deliberately smaller than the CPU's 128:
// a frame here is registers or scratch, one stack per invocation, and the CPU's headroom is free
// where this is not.
//
// **Running out must not be mistaken for an answer.** `field_eval` returns WS_FIELD_REFUSED, which
// is a value no real field produces, and the sampler counts refusals separately and reports them.
// D676 is exactly this fault from the CPU side: at a stack of 64 the mirror refused every point of
// the estate, and a refusal reports nought sign changes over nought points, which reads precisely
// like perfect agreement.
#define WS_FIELD_STACK 96u
#define WS_FIELD_REFUSED 3.0e30

struct FieldFrame {
    uint node;
    uint step;      // which child, or which SAMPLE POINT, this frame is on
    vec3 p;         // the point this node was asked at
    float acc;      // the answer so far
    // `repeat` and `scatter` work their folded point and their leaning neighbours out once on the
    // way in and then walk up to eight combinations of them; `revolve` keeps one cap's out-of-plane
    // leg here. A frame is the only place any of that can wait — the recursive evaluator holds them
    // in locals, and this one has no locals that survive a push.
    vec3 fold;
    vec3 lean;
    uint axes;      // three axis indices packed two bits each, low first
    uint neighbours;
    float scale;    // what the copy being asked about was scaled by, so it can be undone on the way out
};

// ---------------------------------------------------------------------------------------------
// The two halves. Defined in field_leaf.glsl and field_walk.glsl; declared here so either may be
// included first and neither has to know where the other lives.
// ---------------------------------------------------------------------------------------------

// A node with no children, evaluated directly. `Field::eval`'s leaf cases, transliterated.
float field_leaf(uint at, vec3 p);

// The whole expression under `root`, by an explicit stack. `Field::mirror_eval`, transliterated.
// Returns WS_FIELD_REFUSED if it runs out of stack or meets an op it does not know.
float field_eval(uint root, vec3 p);

// ---------------------------------------------------------------------------------------------
// Shared arithmetic. Both halves use these, so they live where both can see them.
// ---------------------------------------------------------------------------------------------

#define WS_PI  3.14159265358979323846
#define WS_TAU 6.28318530717958647692
#define WS_INV_ROOT2 0.7071067811865476

float ws_axis_of(vec3 p, uint axis) { return (axis == 0u) ? p.x : ((axis == 1u) ? p.y : p.z); }

vec3 ws_with_axis(vec3 p, uint axis, float value) {
    if (axis == 0u) p.x = value;
    else if (axis == 1u) p.y = value;
    else p.z = value;
    return p;
}

// The two axes that are not this one, ASCENDING — not cyclic. field.hpp's block above `other_axes`
// says why, and it is not cosmetic: cyclic order makes the first axis of a y-prism's cross-section
// z, so one hexagon points three different ways depending on which axis it stands on.
void ws_other_axes(uint axis, out uint a, out uint b) {
    if (axis == 0u)      { a = 1u; b = 2u; }
    else if (axis == 1u) { a = 0u; b = 2u; }
    else                 { a = 0u; b = 1u; }
}

// `std::atan2`, including the one case GLSL leaves UNDEFINED and C++ does not.
//
// `atan(y, x)` in GLSL is undefined when both arguments are nought; `std::atan2(0, 0)` is 0. That
// is not a corner nobody reaches — every op that measures an angle about an axis (`arc`,
// `revolve`, `polar repeat`, `spiral`) asks it of a point's offset from that axis, and a shape
// centred on a grid line puts sample points exactly on its own axis. An undefined answer there is
// a NaN, and a NaN in a distance field does not stay local: it propagates out through every min
// and max above it and takes a whole building with it.
float ws_atan2(float y, float x) { return (x == 0.0 && y == 0.0) ? 0.0 : atan(y, x); }

float ws_smooth_min(float a, float b, float k) {
    if (k <= 0.0) return min(a, b);
    const float h = clamp(0.5 + 0.5 * (b - a) / k, 0.0, 1.0);
    return b * (1.0 - h) + a * h - k * h * (1.0 - h);
}

float ws_smooth_max(float a, float b, float k) { return -ws_smooth_min(-a, -b, k); }

// The FLAT version, which is what a chamfer is. The clamp is not decoration — see the long block
// above `chamfer_min` in field.cpp: without it the field falls by root2 metres per metre deep
// inside two overlapping shapes, which breaks the one promise `metric_slack` makes, and a block
// wrongly settled is matter that is silently not there.
float ws_chamfer_min(float a, float b, float k) {
    if (k <= 0.0) return min(a, b);
    const float plain = min(a, b);
    return min(plain, max((a + b - k) * WS_INV_ROOT2, plain - k * WS_INV_ROOT2));
}

float ws_chamfer_max(float a, float b, float k) { return -ws_chamfer_min(-a, -b, k); }

// A stored zero means one, so a node built before `stretch=` existed reads exactly as it did.
vec3 ws_stretched(vec3 p, float sx, float sy, float sz) {
    if ((sx == 0.0 || sx == 1.0) && (sy == 0.0 || sy == 1.0) && (sz == 0.0 || sz == 1.0)) return p;
    return vec3(p.x / ((sx != 0.0) ? sx : 1.0), p.y / ((sy != 0.0) ? sy : 1.0),
                p.z / ((sz != 0.0) ? sz : 1.0));
}

// How far a point is from a node's own box, squared, for the union cull. Zero inside it, and zero
// for the infinite box, which is what a node whose extent could not be worked out carries.
float ws_box_away_sq(uint at, vec3 p) {
    const float lx = field_nodes.items[at].lo[0];
    if (lx <= -1.0e29 || field_nodes.items[at].hi[0] >= 1.0e29) return 0.0;
    const vec3 lo = vec3(lx, field_nodes.items[at].lo[1], field_nodes.items[at].lo[2]);
    const vec3 hi = vec3(field_nodes.items[at].hi[0], field_nodes.items[at].hi[1],
                         field_nodes.items[at].hi[2]);
    const vec3 d = max(max(lo - p, p - hi), vec3(0.0));
    return dot(d, d);
}

#endif   // WS_FIELD_TYPES_GLSL
