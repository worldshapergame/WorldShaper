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
//
// **`children` is NOT just the count any more.** The low byte is the count, exactly as it was, and
// the bits above it are the R12 accelerator's word — what the host worked out about this node's box
// and its children's boxes ONCE PER CLIP, so that the walk does not rediscover it at every one of
// the hundred million points it is asked about. See WS_NODE_* below and `pack_cull_word` in
// `src/gpu/field_gpu.cpp`, which is the only thing that writes it.
//
// Packed into the spare bits of a word that only ever held 0..4, rather than added as a field of its
// own: the record is 80 bytes and every one of them is read from global memory by every turn of the
// walk, so a ninth word would be a 10% tax on the traffic to buy a summary that fits in eight bits.
// A caller that writes a bare count still works — every bit above the count reads as nought, which
// means "nothing is known", and the walk falls back to exactly what it did before R12c.
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
// --no-field-accel: the walk as it was before R12c — a union's children asked in the order the
// author wrote them, and a difference's carves all asked. The control arm, and the arm every figure
// taken before R12c was measured in. See the fold-op case in field_walk.glsl.
#define WS_FIELD_FLAG_NO_ACCEL 4u


// ---------------------------------------------------------------------------------------------
// THE DIVERGENCE INSTRUMENT, and the hole in D727 it exists to close.
//
// D727 concluded that the card is at parity per node visit with ten scalar cores — 813 million
// visits a second against 645 million, about 14,000 lane-cycles a visit against a core's seventy —
// and named the cause as **every lane paying for every other lane's opcode** in a switch over
// sixty-seven ops. From that it named the lever: specialise the shader to the ops one clip uses.
//
// **That inference has a hole and it decides whether the lever is worth pulling.** Taking ops out
// of the switch shrinks the INSTRUCTION STREAM. It does not remove divergence among the ops that
// remain: if the thirty-two lanes of a warp are standing on fifteen different ops at one turn, the
// warp executes fifteen branches whether the shader knows about twenty ops or sixty-seven, and the
// specialisation buys instruction cache and very little else.
//
// So this counts the thing the argument is actually about: **how many distinct ops the ACTIVE lanes
// of one warp are spread over, at each turn of the walk.** Two lanes on the same op cost the warp
// one branch between them; two lanes on different ops cost two.
//
// It is measured with a ballot rather than inferred, and the measurement is of the real estate —
// the ladder's own nodes on a real clip, at the turn where the switch is about to be entered.
//
// **And it counts distinct NODES beside distinct ops, because the two answers point at completely
// different repairs and a divergence figure alone cannot tell them apart.** A turn reads an 80-byte
// record out of a buffer indexed at random; if the lanes of a warp stand on one node the load is a
// broadcast and the walk is control flow, and if they stand on thirty the same turn is thirty cache
// lines and the walk is memory. Distinct ops can never exceed distinct nodes, so the pair also
// says how much of the op agreement is lanes genuinely walking together and how much is different
// nodes that merely share an op.
//
// **What a ballot can and cannot see.** `subgroupBallot(true)` reports the lanes that are executing
// this instruction together, which is the warp's mask at this point and exactly the set that shares
// the branch. It cannot see lanes that have already left the loop, which is correct — a lane that
// has finished is not paying for anybody's opcode. It also tends to FORCE reconvergence where the
// hardware might otherwise let lanes drift, so the figure is the number for a warp that is as
// converged as it can be, which is the favourable case for the specialising argument rather than
// the harsh one.
// ---------------------------------------------------------------------------------------------
#ifdef WS_FIELD_MEASURE_DIVERGENCE
// Per-lane accumulators, over the turns THIS lane took. All the active lanes of a warp see the same
// two numbers at a given turn, so summing them over lanes on the host weights each turn by how many
// lanes were actually in it — which is the weighting that matters, since a turn with two lanes left
// in it is not what the card is spending its time on.
uint ws_div_ops;
uint ws_div_nodes;

// How many distinct values of `what` the active lanes of this warp hold, and how many lanes those
// are.
//
// The loop is the standard one: take the lowest lane still unaccounted for, broadcast its value,
// strike out every lane that agrees, and count. It runs once per distinct value, so a converged
// warp costs one turn of it and a fully scattered one costs thirty-two — the instrument is cheap
// exactly where the answer is "cheap" and dear where the answer is "dear", which is the right way
// round for a run that must not reach the driver's watchdog.
//
// `subgroupBroadcast`'s lane may be dynamically uniform rather than a compile-time constant from
// SPIR-V 1.5 onwards, and the build targets vulkan1.3, so `subgroupBallotFindLSB` is a legal
// source for it and no shuffle capability is needed.
uint ws_distinct(uint what, out uint lanes) {
    uvec4 pending = subgroupBallot(true);
    lanes = subgroupBallotBitCount(pending);
    uint distinct = 0u;
    for (uint k = 0u; k < 32u; ++k) {
        if (subgroupBallotBitCount(pending) == 0u) break;
        const uint lane = subgroupBallotFindLSB(pending);
        const uint value = subgroupBroadcast(what, lane);
        pending &= ~subgroupBallot(what == value);
        ++distinct;
    }
    return distinct;
}
#endif   // WS_FIELD_MEASURE_DIVERGENCE

// ---------------------------------------------------------------------------------------------
// R12d - THE SHADER, SPECIALISED TO THE CLIP IN FRONT OF IT.
//
// One Vulkan specialisation constant per op, `constant_id` being the op's own number, and the host
// sets each from the ops the clip's field can actually reach. The driver folds the constant and
// deletes the case, so the walk a clip runs is a walk with the sixty-seven-way switch narrowed to
// the ops that clip contains -- no runtime shader compiler, no generated GLSL, no new dependency,
// and one pipeline per distinct op set.
//
// **Defaulting to `true` is what makes this safe.** A pipeline created with no specialisation info
// at all -- which is what `ComputePipeline` does, and what every hot reload and every other caller
// gets -- is byte for byte the shader that was there before. Specialisation can only ever REMOVE,
// and a set that wrongly says an op is absent is a wrong building rather than a slow one, which is
// why `reachable_ops` in field_gpu.cpp walks from EVERY root the shader is handed and not just from
// the solid.
//
// **The op number is never written twice.** `WS_USES(WS_OP_TORUS)` expands through the define above
// to `ws_uses_8u`, so the guard on a case is derived from the same `#define` the case label is. The
// two-level macro is what makes the argument expand before the paste.
//
// # What the measurement says this can and cannot buy, so nobody re-derives it
//
// D727 named this lever on the reasoning that every lane pays for every other lane's opcode. The
// ballot in `ws_distinct` measured what that is worth: **a warp's 31.1 active lanes stand on 4.03
// DISTINCT ops at a turn, and no warp in 23,024 averaged above ten.** So the warp already runs about
// four op bodies a turn and this cannot make it run fewer -- what it removes is the ops NO lane is
// on, which is instruction stream and instruction cache and not divergence.
//
// And how much of that there is, is a fact about the clip rather than about the shader:
// **`clips/facility.clip` reaches 49 of the 67 ops**, from 629 roots. So on the clip this engine
// ships, specialising deletes eighteen cases of sixty-seven and nothing else. A clip with a small
// expression is where this has room: `sampler.clip` reaches 17 and `mirror_hall.clip` 5.
//
// # And it was built anyway, and measured, and it is 1.00x
//
// **`WS_GPU_SPECIALISE=1` in the environment, OFF by default.** Over the SAME 1,089 dispatches and
// 17,842,176 cells in every arm — the ladder's order is deterministic, so the two arms are over the
// same nodes and not over whatever each reached before its deadline:
//
//   specialised to 49 ops   3.503, 3.450, 3.507 µs a cell
//   the change ABSENT       3.471, 3.497, 3.462 µs a cell
//
// So the lever D727 named is worth nothing on the clip that ships, and the ballot above says why
// before the timing does. It stays here, gated and instrumented, because the argument still has a
// clip-shaped hole in it: nobody has yet measured a heavy clip with a SMALL op set, and the two
// small-op clips here finish their whole run in 38 ms and 9 ms of card, which is not a measurement.
// ---------------------------------------------------------------------------------------------
#ifdef WS_FIELD_SPECIALISE
#define WS_USES(op) WS_USES_(op)
#define WS_USES_(op) ws_uses_##op
// WS_GPU_DIVERGE=1 in the environment. A specialisation constant and not a push-constant bit,
// because the instrument's ballot loop is code the SHIPPED pipeline must not carry: a uniform branch
// still costs registers and instruction space, and the thing being measured after this change is
// exactly instruction space. It defaults to false, so the base pipeline -- the one `ComputePipeline`
// builds with no specialisation info at all, and the one every hot reload gets -- has no instrument
// in it whatever.
layout(constant_id = 67) const bool ws_measure_divergence = false;
layout(constant_id = 0) const bool ws_uses_0u = true;   // constant
layout(constant_id = 1) const bool ws_uses_1u = true;   // parameter
layout(constant_id = 2) const bool ws_uses_2u = true;   // coordinate
layout(constant_id = 3) const bool ws_uses_3u = true;   // radius
layout(constant_id = 4) const bool ws_uses_4u = true;   // sphere
layout(constant_id = 5) const bool ws_uses_5u = true;   // box
layout(constant_id = 6) const bool ws_uses_6u = true;   // cylinder
layout(constant_id = 7) const bool ws_uses_7u = true;   // capsule
layout(constant_id = 8) const bool ws_uses_8u = true;   // torus
layout(constant_id = 9) const bool ws_uses_9u = true;   // arc
layout(constant_id = 10) const bool ws_uses_10u = true;   // cone
layout(constant_id = 11) const bool ws_uses_11u = true;   // plane
layout(constant_id = 12) const bool ws_uses_12u = true;   // ellipsoid
layout(constant_id = 13) const bool ws_uses_13u = true;   // prism
layout(constant_id = 14) const bool ws_uses_14u = true;   // platonic
layout(constant_id = 15) const bool ws_uses_15u = true;   // wedge
layout(constant_id = 16) const bool ws_uses_16u = true;   // stairs
layout(constant_id = 17) const bool ws_uses_17u = true;   // revolve
layout(constant_id = 18) const bool ws_uses_18u = true;   // spiral
layout(constant_id = 19) const bool ws_uses_19u = true;   // union
layout(constant_id = 20) const bool ws_uses_20u = true;   // intersection
layout(constant_id = 21) const bool ws_uses_21u = true;   // difference
layout(constant_id = 22) const bool ws_uses_22u = true;   // smooth_union
layout(constant_id = 23) const bool ws_uses_23u = true;   // smooth_difference
layout(constant_id = 24) const bool ws_uses_24u = true;   // smooth_intersection
layout(constant_id = 25) const bool ws_uses_25u = true;   // chamfer_union
layout(constant_id = 26) const bool ws_uses_26u = true;   // chamfer_difference
layout(constant_id = 27) const bool ws_uses_27u = true;   // chamfer_intersection
layout(constant_id = 28) const bool ws_uses_28u = true;   // translate
layout(constant_id = 29) const bool ws_uses_29u = true;   // rotate
layout(constant_id = 30) const bool ws_uses_30u = true;   // scale
layout(constant_id = 31) const bool ws_uses_31u = true;   // mirror
layout(constant_id = 32) const bool ws_uses_32u = true;   // repeat
layout(constant_id = 33) const bool ws_uses_33u = true;   // polar_repeat
layout(constant_id = 34) const bool ws_uses_34u = true;   // scatter
layout(constant_id = 35) const bool ws_uses_35u = true;   // shell
layout(constant_id = 36) const bool ws_uses_36u = true;   // round
layout(constant_id = 37) const bool ws_uses_37u = true;   // offset
layout(constant_id = 38) const bool ws_uses_38u = true;   // displace
layout(constant_id = 39) const bool ws_uses_39u = true;   // twist
layout(constant_id = 40) const bool ws_uses_40u = true;   // bend
layout(constant_id = 41) const bool ws_uses_41u = true;   // sine
layout(constant_id = 42) const bool ws_uses_42u = true;   // waves
layout(constant_id = 43) const bool ws_uses_43u = true;   // noise
layout(constant_id = 44) const bool ws_uses_44u = true;   // fbm
layout(constant_id = 45) const bool ws_uses_45u = true;   // ridged
layout(constant_id = 46) const bool ws_uses_46u = true;   // rasp
layout(constant_id = 47) const bool ws_uses_47u = true;   // cells
layout(constant_id = 48) const bool ws_uses_48u = true;   // cell_edge
layout(constant_id = 49) const bool ws_uses_49u = true;   // curvature
layout(constant_id = 50) const bool ws_uses_50u = true;   // occlusion
layout(constant_id = 51) const bool ws_uses_51u = true;   // facing
layout(constant_id = 52) const bool ws_uses_52u = true;   // checker
layout(constant_id = 53) const bool ws_uses_53u = true;   // stripes
layout(constant_id = 54) const bool ws_uses_54u = true;   // bricks
layout(constant_id = 55) const bool ws_uses_55u = true;   // add
layout(constant_id = 56) const bool ws_uses_56u = true;   // multiply
layout(constant_id = 57) const bool ws_uses_57u = true;   // min
layout(constant_id = 58) const bool ws_uses_58u = true;   // max
layout(constant_id = 59) const bool ws_uses_59u = true;   // blend
layout(constant_id = 60) const bool ws_uses_60u = true;   // remap
layout(constant_id = 61) const bool ws_uses_61u = true;   // abs
layout(constant_id = 62) const bool ws_uses_62u = true;   // negate
layout(constant_id = 63) const bool ws_uses_63u = true;   // step
layout(constant_id = 64) const bool ws_uses_64u = true;   // smoothstep
layout(constant_id = 65) const bool ws_uses_65u = true;   // clamp
layout(constant_id = 66) const bool ws_uses_66u = true;   // power
#else
// node.glsl includes the walk as well, and five more shaders build on that. None of them wants
// sixty-seven specialisation constants in its SPIR-V for the sake of a switch it never narrows, so
// only the translation unit that asks for it gets them and everybody else compiles the whole walk.
#define WS_USES(op) true
const bool ws_measure_divergence = false;
#endif   // WS_FIELD_SPECIALISE

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
// **AND IT IS 128, THE SAME AS THE CPU'S, BECAUSE 96 WAS NOT ENOUGH AND THE WAY THAT SHOWED WAS A
// LOST DEVICE.** D676 measured the estate's deepest reached path at 80 over a grid of points, and 96
// was chosen here as that measurement plus headroom, deliberately smaller than `Field::kMirrorStack`
// on the argument that a frame is scratch memory on a card and free on a CPU. Both halves of that
// were wrong. A grid of points is not the set of points a CAMERA asks about — the enclosed camera
// stands inside the building, where the paths are deepest — and 96 refused **4,580 cells of a single
// batch**. The frame size was then measured directly and does not move a dispatch at all (see the
// note under `FieldFrame`), so the headroom costs nothing and there was never a reason to be mean
// with it. **This number tracks `Field::kMirrorStack`; if that moves, move this.**
//
// **Running out must not be mistaken for an answer.** `field_eval` returns WS_FIELD_REFUSED, which
// is a value no real field produces, and the sampler counts refusals separately and reports them.
// D676 is exactly this fault from the CPU side: at a stack of 64 the mirror refused every point of
// the estate, and a refusal reports nought sign changes over nought points, which reads precisely
// like perfect agreement.
#define WS_FIELD_STACK 128u
#define WS_FIELD_REFUSED 3.0e30

// AND THE WALK IS BOUNDED IN TURNS AS WELL AS IN DEPTH, because on a card the two failures are not
// the same failure.
//
// A stack machine whose every case does not either push or finish does not return a wrong answer;
// it does not return. On a CPU that is a hung thread somebody can attach a debugger to. On a card it
// is a dispatch that never completes, and after about two seconds Windows RESETS THE DEVICE — which
// arrives as `VK_ERROR_DEVICE_LOST` in whatever call happens next, pages of `fault type 4` from the
// fault extension, and the game gone mid-session with whatever the player was building. It cost a
// device on the enclosed camera before this constant existed, on the very first dispatch.
//
// **The number is 1,048,576 and it was 65,536, which was sized against the wrong thing.** 65,536 is
// eight times the 8,231 nodes a cell of the ENCLOSED camera walks, which sounds ample and refused
// 1,952 cells of one batch. The mistake is that this counts turns per EVALUATION and an evaluation
// is not bounded by the field's node count: `occlusion` asks its child fourteen times, `curvature`
// seven, `repeat` eight, and nested those multiply — so one honest evaluation of a weathering rule
// can walk far more nodes than the field has. Sized now at 57x the whole field rather than at a
// multiple of a mean, which is the only bound that means anything when re-entrant ops compose.
//
// A walk that reaches even this reports a REFUSAL — counted, surfaced and gated — rather than taking
// the machine down. D678, D681.
#define WS_FIELD_TURNS 1048576u

// Whether this invocation hit something it could not answer for, and WHICH OP it was standing on
// when it happened. Nought is the only acceptable number and the sampler says so out loud.
//
// The op is the whole value of this. "Some cells refused" is a hunt; "1,952 cells refused, all of
// them on op 34" is a line number. It costs one word and it is written once, at the moment the walk
// gives up, so it is free on every walk that does not.
uint ws_field_refused;
uint ws_field_refused_op;

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

// **DO NOT SPEND A SESSION SHRINKING THAT STRUCT. It was measured and it does not matter.**
//
// The obvious reading of this file is that fifteen words times ninety-six frames is 5.8 KB of
// scratch memory per invocation, that only six of the fifteen are used by the ops which carry the
// walk (`fold`, `lean`, `axes`, `neighbours` and `scale` belong to `repeat`, `scatter`, `revolve`,
// `curvature` and `facing` alone), and that moving the cold half onto a small side stack would
// halve the traffic and roughly double the throughput. It is a good argument and it is wrong.
//
// The control arm is one line and it was taken before the work: **nine dead words added here**, so
// a frame is 24 words instead of 15 -- 1.6x the stack, the same everything else. Estate, outdoor
// camera, 40 seconds, same batch: **53.6 ms a dispatch against 54.7 ms**. No change at all, which
// is the answer, and it says the scratch stack is not what a dispatch is waiting on. D678.

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
// **NO `infinite()` SHORT CUT, and its absence is the point rather than an oversight.**
//
// This read `if (lo[0] <= -1e29 || hi[0] >= 1e29) return 0.0;` — the same short cut D722 pulled out
// of `squared_distance_to` on the CPU, left standing here because nothing about it is wrong enough
// to show. It asks about the **X axis only**, and a box may be unbounded on one axis and bounded on
// another: a ground plane is `y <= 0` and nothing else, and four of the estate's roofs are slanted
// half spaces. Every one of those answered "distance nought" to a point forty metres above it, and
// nought is what a cull cannot reject on.
//
// It never produced a wrong voxel, which is why it survived D691's whole pass over this file: the
// card's answers were right and only its WALK was long. Measured on `clips/facility.clip`, one
// evaluation of the root at the ladder's own cell centres — the same points in both arms —
// **1,518 nodes on the card against 866 in `Field::eval`, 1.75x.**
//
// The arithmetic needs no special case, exactly as the CPU's comment says. `max(-1e30 - p, 0)` is
// nought on an unbounded axis by construction. The `min` is the one thing f32 needs that f64 does
// not: a degenerate box whose low is above its high gives a leg of 1e30, and 1e30 squared overflows
// a float to infinity — which would then sort PAST the `3.0e38` key the union's network gives an
// empty slot and hand the walk a child index of a node that has fewer children than that. Clamped
// at 1e18 the leg still dwarfs any distance in a clip, so every cull decision is the one the CPU
// makes, and nothing overflows.
float ws_box_away_sq(uint at, vec3 p) {
    const vec3 lo = vec3(field_nodes.items[at].lo[0], field_nodes.items[at].lo[1],
                         field_nodes.items[at].lo[2]);
    const vec3 hi = vec3(field_nodes.items[at].hi[0], field_nodes.items[at].hi[1],
                         field_nodes.items[at].hi[2]);
    const vec3 d = min(max(max(lo - p, p - hi), vec3(0.0)), vec3(1.0e18));
    return dot(d, d);
}

// ---------------------------------------------------------------------------------------------
// R12 — the accelerator's word, in the high bits of `children`.
//
// # What it is, and what it deliberately is NOT
//
// It is the host's answer, taken once when a clip is uploaded, to two questions the walk otherwise
// asks at every point of every cell: **does this node carry a box at all**, and **can sorting this
// node's children by that box change anything**. Neither answer depends on the point, so neither
// belongs inside a walk that is run a hundred million times.
//
// **It is not a NEW bound and it must never become one.** The boxes are `Field::bounds_of`,
// narrowed, exactly as they already were — the same boxes `Field::eval`'s cull reads, with the same
// four primitives under-stating their own distance and the same intersections unable to vouch for
// their overlap. D644 measured that hole and D646 built the sound repair and REFUSED it at 45x: *a
// cull box an answer can vouch for is, on this building, a box that rejects nothing.* The card must
// make the CPU's decisions, not better ones, or the two halves of the engine build two worlds —
// which is what `--gpu-sample-check` exists to catch. So every bit here is a statement about a box
// that was already being uploaded, and never a statement about a shape.
//
// # The layout
//
//   bits 0..7    the child count, 0..4. What this whole word used to be.
//   bit  8       this node's own box bounds it on at least one AXIS.
//   bits 9..12   one per child, in slot order: that CHILD's box bounds it on at least one axis.
//
// "On at least one axis" and not "finite", and the difference is a measured 1.75x. See
// `says_nothing` in field_gpu.cpp: these bits used to be `Field::Aabb::infinite()`, which asks
// about x alone, so a ground plane bounded in y arrived here as "no box" and `ws_child_away_sq`
// answered nought for it without reading anything.
//   bit  13      this node has more than one child and at least one of them is bounded, so
//                ordering them by box distance can put a different one first.
//
// Bit 13 is not a shortcut round the CPU. `Field::eval` sorts a union of more than one child
// unconditionally; when NO child carries a box every key is nought and a stable sort is the
// identity, so skipping the sort there is provably the same order and the same decisions. That is
// the only case it skips.
#define WS_NODE_COUNT_MASK 0xFFu
#define WS_NODE_BOUNDED    0x100u    // this node's own box is finite
#define WS_NODE_CHILD_0    0x200u    // << slot: that child's box is finite
#define WS_NODE_CULLABLE   0x2000u   // more than one child, and at least one of them bounded

uint ws_child_count(uint word) { return min(word & WS_NODE_COUNT_MASK, 4u); }
bool ws_child_bounded(uint word, uint slot) { return (word & (WS_NODE_CHILD_0 << slot)) != 0u; }

// `ws_box_away_sq` for a child the parent's word already knows about.
//
// An UNBOUNDED child then costs nothing instead of two loads out of a record eighty bytes away and
// a page away in the buffer, and that is not a rounding. **D675 counted 923 of the nodes a cell
// WALKS carrying no box — a quarter of the walk** — and this build counts 2,811 of the estate's
// 18,250 in that state, 15.4%; the two are different denominators and both are worth knowing, since
// an ancestor of an unbounded node cannot be bounded either and the walk is where that compounds.
// Nought is what `ws_box_away_sq` answers for the infinite box too, so this is the same number by a
// cheaper route and not a different rule.
float ws_child_away_sq(uint word, uint slot, uint at, vec3 p) {
    if (!ws_child_bounded(word, slot)) return 0.0;
    return ws_box_away_sq(at, p);
}

#endif   // WS_FIELD_TYPES_GLSL
