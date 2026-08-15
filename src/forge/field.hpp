#pragma once
// Fields: the one mechanism every shape and every pattern in a clip is made of.
//
// A field is a function from a point in space to a number. That is all. What the number *means*
// is decided by whoever reads it, and there are only two readers:
//
//   as a distance   negative inside the shape, positive outside, and the magnitude is roughly
//                   how far away the surface is. Signed distance. Every solid is one of these.
//   as an amount    a pattern — a wave, a grain, a checker — read for its value rather than its
//                   sign, and used to push a surface about or to choose a material.
//
// One mechanism for both is not a saving, it is the point. It means a wave can be carved into a
// wall by *adding* it to the wall's distance, that the same wave can then decide which voxels
// are moss, and that neither needs to know anything about the other. A pattern is a shape whose
// sign nobody looked at; a shape is a pattern somebody took the sign of.
//
// # Why distance rather than a mesh or a voxel array
//
// Because the questions this has to answer are all about *place*. Is this point inside? How far
// is it from the surface? What is the surface like here? A mesh answers none of those without
// work. A distance field answers all three by being evaluated, which is also exactly what
// filling a voxel volume needs: ask once per voxel.
//
// It also makes the operations trivial and exact. Union is a minimum. Intersection is a maximum.
// Carving is a maximum against a negation. A shell is the absolute value minus a thickness. A
// rounded edge is a subtraction. None of those need special cases, and none of them can produce
// a shape the next operation cannot handle, which is what makes it possible to say "any pattern,
// any structure" and mean it.
//
// # Why a flat array of nodes rather than a tree of objects
//
// A clip of eight metres cubed is sixteen million voxels, and every one of them evaluates the
// whole expression. A tree of virtual calls or std::function would spend most of that time in
// call overhead and pointer chasing. The nodes live in one contiguous vector, children are
// indices into it, and evaluation is a switch — so the whole expression is usually in cache and
// the cost is arithmetic rather than dispatch.
//
// It is also the shape a file wants. A clip is authored as text, and text parses naturally into
// exactly this: a list of nodes referring to earlier ones by name.
//
// # Units
//
// Metres, everywhere in this file. A clip is authored at human scale — a doorway is two metres,
// a step is eighteen centimetres — and converting to voxels is the sampler's job, once, at the
// end. Nothing here knows what a voxel is.

#include <string>
#include <vector>

#include "core/types.hpp"

namespace ws {
namespace forge {

struct Vec3 {
    f64 x = 0.0;
    f64 y = 0.0;
    f64 z = 0.0;
};

inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(Vec3 a, f64 s) { return {a.x * s, a.y * s, a.z * s}; }
inline Vec3 operator*(Vec3 a, Vec3 b) { return {a.x * b.x, a.y * b.y, a.z * b.z}; }
f64 length(Vec3 v);
f64 dot(Vec3 a, Vec3 b);
Vec3 normalise(Vec3 v);

// What a node does. The order is not meaningful; the grouping is, and it is the grouping the
// clip file's vocabulary follows.
enum class Op : u8 {
    // --- constants and coordinates ---------------------------------------------------
    Constant,      // a[0]
    Parameter,     // slot a[0] of the field's parameter table — see `parameter()`
    Coordinate,    // a[0] selects x, y or z — the raw position, for gradients and ramps
    Radius,        // distance from a point a[0..2], for radial patterns

    // --- solids, as exact signed distances --------------------------------------------
    Sphere,        // centre a[0..2], radius a[3]
    Box,           // centre a[0..2], half extent a[3..5], corner radius a[6]
    Cylinder,      // centre a[0..2], radius a[3], half height a[4], axis a[5]
    Capsule,       // end a[0..2], end a[3..5], radius a[6]
    Torus,         // centre a[0..2], ring radius a[3], tube radius a[4], axis a[5]
    Cone,          // base centre a[0..2], base radius a[3], height a[4], axis a[5]
    Plane,         // normal a[0..2], offset a[3]: the half space behind it
    Ellipsoid,     // centre a[0..2], radii a[3..5]
    Prism,         // regular n-gon: centre a[0..2], circumradius a[3], half height a[4],
                   // sides a[5], axis a[6], turn a[7] in turns
    Platonic,      // centre a[0..2], circumradius a[3], which a[4]: 0 tetra 1 cube 2 octa
                   // 3 dodeca 4 icosa
    Wedge,         // a ramp: centre a[0..2], half extent a[3..5], rise along a[6] from a[7]
    Stairs,        // centre a[0..2], half extent a[3..5], step run a[6], step rise a[7]

    // --- swept: a curve or a section made into a solid -----------------------------------
    //
    // A classical building is almost entirely surfaces of revolution and one spiral. A base, a
    // baluster, an urn, a dome, and every moulding that runs round a column is one profile drawn
    // once and turned about an axis; the Ionic capital is a logarithmic spiral. Faked out of
    // stacked cylinders those come out as staircases of rings, and the fake costs more nodes than
    // the real thing does.
    Revolve,       // child 0 asked at (radius, height) instead of (x, y, z): centre a[0..2],
                   // axis a[3]. The profile is drawn in the plane of the first cross-axis and
                   // the axis itself, at the third coordinate zero
    Spiral,        // a logarithmic spiral swept as a tube: centre a[0..2], starting radius a[3],
                   // radius multiplier per turn a[4], tube radius a[5], turns a[6], axis a[7]

    // --- combining ---------------------------------------------------------------------
    Union,         // min over children
    Intersection,  // max over children
    Difference,    // child 0 minus the rest
    SmoothUnion,   // as above but blended over a[0] metres
    SmoothDifference,
    SmoothIntersection,

    // --- moving the point before asking -------------------------------------------------
    Translate,     // by a[0..2]
    Rotate,        // euler xyz, a[0..2] in turns
    Scale,         // by a[0..2]
    Mirror,        // fold about the plane through the origin with normal on axis a[0]
    Repeat,        // tile with period a[0..2]; a[3..5] limit how many either side
    PolarRepeat,   // a[0] copies about the axis a[1]

    // --- changing the answer --------------------------------------------------------------
    Shell,         // hollow: |d| - a[0]
    Round,         // d - a[0]
    Offset,        // d + a[0], which is the same thing said the other way and reads better
    Displace,      // child 0's distance plus child 1's value times a[0]
    Twist,         // turn about axis a[1] by a[0] turns per metre
    Bend,          // bend about axis a[1] by a[0] turns per metre

    // --- patterns: read for value, not sign -------------------------------------------------
    Sine,          // along axis a[0], period a[1] metres, phase a[2] turns
    Waves,         // two sines crossed, on the plane perpendicular to a[0]
    Noise,         // value noise, feature size a[0], seed a[1]
    Fbm,           // stacked noise: size a[0], octaves a[1], gain a[2], lacunarity a[3], seed a[4]
    Ridged,        // 1 - |fbm|, the sharp-crested one: same arguments
    Rasp,          // high frequency ridges, for a filed or scratched surface
    Cells,         // distance to the nearest of a scattered set of points: size a[0], seed a[1]
    CellEdge,      // how near the boundary *between* two cells: size a[0], seed a[1]. This is
                   // what a crack is — cells are not the pattern, the seams between them are

    // --- what the shape is doing here, rather than what is here -------------------------
    //
    // Weathering is not a texture laid over a surface, it is a consequence of the surface's
    // own geometry: rain runs off a sill and stains beneath it, sand piles where a wall meets
    // the ground, moss grows where a corner stays damp, an exposed arris wears round while the
    // hollow behind it keeps its edge. All of that follows from two questions — which way is
    // this surface bending, and how much of the sky can it see — and both are answerable from
    // the distance field alone.
    Curvature,     // child 0, sampled at radius a[0]. Positive on a convex edge, negative in a
                   // concave corner, near zero on a flat face
    Occlusion,     // child 0, radius a[0]. How much of a sphere at this point is inside the
                   // shape: 0 out in the open, 1 buried. The cavity term
    Facing,        // child 0's surface normal along axis a[0]. Up-facing collects, down-facing
                   // stays dry and takes soot
    Checker,       // alternating, cell a[0..2]
    Stripes,       // along axis a[0], period a[1], duty a[2]
    Bricks,        // running bond: brick a[0..2], mortar a[3], on the plane facing axis a[4]

    // --- arithmetic on values ------------------------------------------------------------
    Add,           // children summed
    Multiply,      // children multiplied
    Min,
    Max,
    Blend,         // mix(child0, child1, a[0])
    Remap,         // from a[0..1] to a[2..3], clamped
    Abs,
    Negate,
    Step,          // 1 where child 0 is above a[0], else 0
    Smoothstep,    // the same with a[0]..a[1] of easing
    Clamp,         // to a[0..1]
    Power,         // |child| ^ a[0], keeping the sign
};

// What an op is called, in the words the clip file uses. For diagnostics only — no parser reads
// it back — and it exists because a histogram over `Op` printed as numbers is a histogram nobody
// can act on without the header open beside it.
const char* op_name(Op op);

// Whether an op's answer can be trusted as at least the distance to its own bounding box.
//
// **Measured, not reasoned (D644).** Sampling each primitive outside its own box, the worst ratio
// of answer to box distance is 1.0000 for the sphere, box, cylinder, capsule, torus, wedge, stairs
// and spiral -- and **0.5877 for the ellipsoid, 0.5300 for the cone, 0.8660 for the prism and
// 0.5774 for the platonic**, because those four are bounded approximations rather than exact
// distances (an ellipsoid has no closed form; the other three are intersections of half planes,
// which under-state out past a corner).
//
// Under-stating a distance is otherwise the SAFE direction and this engine prefers it everywhere:
// a march that steps short is slow, one that steps long goes through the wall. The box cull in
// `eval` is the one place that assumes the opposite, and `build_bounds` already refuses a box to a
// non-uniform scale in a comment that describes this exactly without knowing four primitives had
// it too. **The cull still reads these boxes** -- see D644 for what fixing that measured at.
bool op_reports_true_distance(Op op);

struct Node {
    Op op = Op::Constant;
    f64 a[8]{};
    u32 child[4]{};
    u32 children = 0;
};

// A field: the nodes, their named parameters, and which node is the answer.
//
// Every builder returns the index of the node it added, so an expression is written inside out
// exactly as it reads — `f.round_off(f.box(...), 0.05)` — and any sub-expression can be handed
// to two parents without being built twice.
//
// # Three things this representation has to survive, and why it is shaped like this
//
// **A player editing it visually.** A flat list of nodes with indices for children *is* a node
// graph. A visual editor draws it directly, and the text file is a serialisation of the same
// thing rather than a separate language that has to be kept in step. Neither is the "real" form:
// the node array is, and text and wires are two views of it.
//
// **Parameters moving while you watch.** A number typed into a node is baked into it, and
// changing one would mean re-parsing the file and rebuilding the graph. So numbers that are
// meant to move are not typed into nodes — they are *slots*, and a node refers to a slot.
// Turning a dial writes one double and re-evaluates. Nothing is rebuilt, nothing is allocated,
// and the graph a visual editor is displaying does not change under it.
//
// **Being evaluated somewhere else.** Nodes are plain data of a fixed size with no pointers, and
// evaluation is a switch over them with an explicit stack of at most a handful of entries. That
// is a shape that transliterates to a compute shader without changing: upload the array, upload
// the parameters, and a clip can be re-voxelised on the GPU as fast as its parameters change.
// Nothing here does that yet, and everything here is arranged so that it can.
class Field {
public:
    // --- parameters --------------------------------------------------------------------
    //
    // A number the clip exposes by name, so it can be moved without touching the graph. Returns
    // a node that reads it; the same name asked for twice gives the same slot.
    u32 parameter(const char* name, f64 initial);
    // Writes a slot by name. Cheap by design: this is what a dial being dragged calls.
    bool set_parameter(const char* name, f64 value);
    f64 get_parameter(const char* name, f64 fallback = 0.0) const;
    usize parameter_count() const { return parameters_.size(); }
    const char* parameter_name(usize slot) const { return names_[slot].c_str(); }
    f64 parameter_value(usize slot) const { return parameters_[slot]; }

    // --- constants and coordinates ------------------------------------------------------
    u32 constant(f64 value);
    u32 coordinate(u32 axis);
    u32 radius(Vec3 centre);

    // --- solids -------------------------------------------------------------------------
    u32 sphere(Vec3 centre, f64 r);
    u32 box(Vec3 centre, Vec3 half, f64 corner = 0.0);
    u32 cylinder(Vec3 centre, f64 r, f64 half_height, u32 axis = 1);
    u32 capsule(Vec3 a, Vec3 b, f64 r);
    u32 torus(Vec3 centre, f64 ring, f64 tube, u32 axis = 1);
    u32 cone(Vec3 base_centre, f64 base_r, f64 height, u32 axis = 1);
    u32 plane(Vec3 normal, f64 offset);
    u32 ellipsoid(Vec3 centre, Vec3 radii);
    u32 prism(Vec3 centre, f64 circumradius, f64 half_height, u32 sides, u32 axis = 1,
              f64 turn = 0.0);
    u32 platonic(Vec3 centre, f64 circumradius, u32 which);
    u32 wedge(Vec3 centre, Vec3 half, u32 rise_axis, u32 run_axis);
    u32 stairs(Vec3 centre, Vec3 half, f64 run, f64 rise);

    // --- swept ---------------------------------------------------------------------------
    //
    // Turn a profile about an axis. The profile is any expression; it is asked at (radius from
    // the axis, distance along it, 0), so a shape drawn in a plane becomes the solid that plane
    // sweeps out. Exact: the nearest point of a surface of revolution to any point is always at
    // that point's own angle, so the three-dimensional distance is the profile's own distance in
    // its plane, and nothing is approximated.
    //
    // This is the operation the orders are made of. An Attic base is a rectangle, two half-rounds
    // and a hollow drawn once and revolved; a dome is an arc; a baluster is a silhouette. Every
    // one of them built out of stacked cylinders instead is a stack of visible steps, more nodes,
    // and a shape that cannot be re-proportioned by moving one number.
    u32 revolve(u32 profile, Vec3 centre, u32 axis = 1);

    // A logarithmic spiral swept as a tube — the Ionic volute, and the only curve in the orders
    // that is not an arc or a straight line.
    //
    // `tighten` is what the radius is multiplied by over one whole turn, which is the way the
    // shape is actually specified by anyone drawing one: 0.7 means each turn is seven tenths of
    // the one outside it. The curve is chopped into segments and swept as a chain of capsules, so
    // the distance returned is the true distance to the thing that is built rather than an
    // approximation of the distance to an ideal spiral nobody voxelises.
    u32 spiral(Vec3 centre, f64 radius, f64 tighten, f64 tube, f64 turns, u32 axis = 2);

    // --- combining ----------------------------------------------------------------------
    u32 unite(const std::vector<u32>& parts);
    u32 intersect(const std::vector<u32>& parts);
    u32 subtract(const std::vector<u32>& parts);
    u32 smooth_unite(const std::vector<u32>& parts, f64 blend);
    u32 smooth_subtract(const std::vector<u32>& parts, f64 blend);
    u32 smooth_intersect(const std::vector<u32>& parts, f64 blend);

    // --- moving the point ----------------------------------------------------------------
    u32 translate(u32 child, Vec3 by);
    u32 rotate(u32 child, Vec3 turns);
    u32 scale(u32 child, Vec3 by);
    u32 mirror(u32 child, u32 axis);
    u32 repeat(u32 child, Vec3 period, Vec3 limit);
    u32 polar_repeat(u32 child, u32 count, u32 axis);

    // --- changing the answer --------------------------------------------------------------
    u32 shell(u32 child, f64 thickness);
    u32 round_off(u32 child, f64 r);
    u32 offset(u32 child, f64 by);
    u32 displace(u32 child, u32 pattern, f64 amount);
    u32 twist(u32 child, f64 turns_per_metre, u32 axis);
    u32 bend(u32 child, f64 turns_per_metre, u32 axis);

    // --- patterns ---------------------------------------------------------------------------
    u32 sine(u32 axis, f64 period, f64 phase = 0.0);
    u32 waves(u32 axis, f64 period_a, f64 period_b, f64 phase = 0.0);
    u32 noise(f64 size, u32 seed);
    u32 fbm(f64 size, u32 octaves, f64 gain, f64 lacunarity, u32 seed);
    u32 ridged(f64 size, u32 octaves, f64 gain, f64 lacunarity, u32 seed);
    u32 rasp(f64 size, f64 depth, u32 seed);
    u32 cells(f64 size, u32 seed);
    u32 cell_edge(f64 size, u32 seed);

    // --- what the shape is doing, for weathering ------------------------------------------
    u32 curvature(u32 child, f64 radius);
    u32 occlusion(u32 child, f64 radius);
    u32 facing(u32 child, u32 axis);
    u32 checker(Vec3 cell);
    u32 stripes(u32 axis, f64 period, f64 duty);
    u32 bricks(Vec3 brick, f64 mortar, u32 face_axis);

    // --- arithmetic ---------------------------------------------------------------------------
    u32 add(const std::vector<u32>& parts);
    u32 multiply(const std::vector<u32>& parts);
    u32 minimum(const std::vector<u32>& parts);
    u32 maximum(const std::vector<u32>& parts);
    u32 blend(u32 a, u32 b, f64 t);
    u32 remap(u32 child, f64 from_lo, f64 from_hi, f64 to_lo, f64 to_hi);
    u32 absolute(u32 child);
    u32 negate(u32 child);
    u32 step(u32 child, f64 edge);
    u32 smoothstep(u32 child, f64 lo, f64 hi);
    u32 clamp_to(u32 child, f64 lo, f64 hi);
    u32 power(u32 child, f64 exponent);

    // The value of node `at` for a point, in metres.
    f64 eval(u32 at, Vec3 p) const;

    // How far the field's answer can under-state the real distance to the surface.
    //
    // A signed distance is what lets the sampler skip empty space: a point that reports being
    // half a metre from anything can have the next sixteen voxels filled in without asking. That
    // is only sound if the number is a *lower bound* on the true distance, and displacement
    // breaks that — adding a pattern to a distance moves the surface without telling the field,
    // so a point may be nearer than it claims by as much as the displacement amount.
    //
    // This adds up that error over the whole expression and returns it, so the sampler can take
    // it off before skipping. Returns infinity when the expression contains a displacement by
    // something whose range is not known, which turns skipping off rather than guessing — a
    // clip that samples slowly is a nuisance, and one with holes in it is a bug nobody can see.
    f64 skip_slack() const;

    // The same question asked of one expression rather than the whole field.
    //
    // `skip_slack` is a blunt instrument: it charges every displacement anywhere in the field
    // against every skip, and it says nothing about whether the expression is a distance at all.
    // This walks a single subtree and answers both — the slack to allow, or kInfiniteSlack when
    // the expression is not a distance in metres and so says nothing about its neighbourhood.
    //
    // That second answer is what makes block sampling possible. A paint rule keyed on a shape
    // ("water where the pool is") can be decided for a whole 8³ block from one reading at its
    // centre, because a distance bounds what the value can be anywhere within reach. A rule keyed
    // on a noise cannot be, because noise says nothing about the next voxel along. Knowing which
    // is which turns fifteen evaluations per voxel into fifteen per block for most of a clip.
    static constexpr f64 kInfiniteSlack = 1e30;
    f64 metric_slack(u32 at) const;

    // The range of values an expression can take, when that can be worked out.
    //
    // Only ever asked about *patterns* — the second child of a displacement — and the answer
    // decides how much a displaced surface can move. It used to be a list of the ops whose output
    // happens to lie in [-1, 1], which is right for a bare noise and wrong for everything built
    // out of one: `multiply { edge amount }` is a perfectly ordinary way to say "only where this
    // is an arris, and only this much", and the list answered "unknown" to it. Unknown means
    // infinite slack, infinite slack means no box in the clip can ever settle, and a clip that
    // cannot settle a box samples every voxel of its bounding volume through the whole
    // expression. That is why weathering was too slow to ship: not the weathering, the arithmetic
    // in front of it.
    //
    // So it is an interval walk instead. Conservative everywhere — a wider range than the truth
    // is safe, a narrower one is a hole in a wall — and false when a subexpression says nothing,
    // which is exactly the old answer for the cases the old answer was right about.
    bool value_range(u32 at, f64& low, f64& high) const;

    // The shape underneath any displacement, and how far the displacement can move its surface.
    //
    // Worth separating because pruning and deciding want different things. Deciding whether a
    // *voxel* is matter needs the displaced value, pattern and all. Deciding whether a whole
    // *box* is settled does not: the undisplaced shape is a true distance, so it needs allowing
    // for the amplitude once rather than twice, and it costs nothing to evaluate the noise it
    // does not consult.
    //
    // Halving the allowance is not a nicety. A wall a quarter of a metre thick has four voxels
    // between its face and its middle; with the doubled allowance no box inside it is ever far
    // enough from a surface to settle, so every voxel in every wall is asked about individually.
    // Halved, most of the wall settles eight voxels at a time.
    u32 undisplaced(u32 at, f64& amplitude) const;

    // The parts a union is made of, or just the node itself when it is not one.
    //
    // The sampler wants this because slack is not a property of a field, it is a property of a
    // place. A clip whose ground is displaced by five centimetres and whose building is displaced
    // by one does not have six centimetres of uncertainty everywhere — it has five near the
    // ground and one near the building, and charging the whole clip the worst case means no box
    // anywhere in a quarter-metre wall is ever far enough from a surface to settle.
    //
    // With the parts listed separately, a box can ask which of them it is actually near — a
    // question its bounding box answers for nothing — and allow only for those.
    // One piece of a shape, and what has been done to it on the way down.
    //
    // `extra` is the allowance owed by displacements that were peeled off above this node. A
    // displacement distributes over a union — displacing a union by a pattern is the same shape
    // as displacing each of its parts by that pattern, because the same value is added to all of
    // them — so a union inside a displace can still be taken apart, as long as each piece carries
    // the allowance the displace would have charged.
    //
    // That distinction is worth the extra field. The facility rings its site with a displaced
    // hedge; undistributed, that is ONE part whose box is the whole site, so every box in the
    // building is "near a displaced thing" and is charged nine centimetres of slack for a hedge
    // it is thirty metres from. Distributed, the hedges are their own small boxes and the
    // building pays nothing for them.
    struct Part {
        u32 node = 0;
        f64 extra = 0.0;
    };
    void union_children(u32 at, std::vector<Part>& out) const;

    // A box each node is known to be contained in, so a union can skip the children that cannot
    // possibly be the nearest thing.
    //
    // This is where the time goes on a clip of any size. A union of thirty parts costs thirty
    // evaluations at every voxel, and at all but a handful of voxels twenty-nine of them are
    // answering about something metres away. With a box round each, the union asks how far the
    // point is from the *box* — three subtractions — and only evaluates the child if that could
    // beat what it already has.
    //
    // Boxes are conservative: anything whose extent cannot be worked out cheaply gets an
    // infinite one, which is always correct and simply never culls. Call it once after building;
    // it is not automatic, because a Field under construction has no meaningful bounds.
    void build_bounds();

    struct Aabb {
        Vec3 low{-1e30, -1e30, -1e30};
        Vec3 high{1e30, 1e30, 1e30};
        bool infinite() const { return low.x <= -1e29 || high.x >= 1e29; }
    };
    Aabb bounds_of(u32 node) const;

    // How many unions were worth a hierarchy, and how many leaves those hierarchies cover.
    // Reported by the clip tool, because whether the accelerator engaged at all is the first
    // question anybody optimising a slow clip will have.
    usize accelerator_count() const { return accelerators_.size(); }
    usize accelerated_leaves() const {
        usize total = 0;
        for (const Accelerator& a : accelerators_) total += a.order.size();
        return total;
    }

    // The surface normal, by central differences. Used for painting by facing — "moss on the
    // top, soot under the arch" — and for nothing else, so its cost is paid only where a rule
    // asks for it.
    Vec3 normal_at(u32 at, Vec3 p, f64 step = 0.01) const;

    // ---- R12b: the same answer, walked the way a shader must walk it ---------------------------
    //
    // `eval` above is recursive, and a compute shader cannot be. So this is the second evaluator
    // the plan asks for: **one loop over an explicit stack of fixed depth**, reading the same flat
    // pointerless node array, reaching the same number. It exists to be COMPARED — `mirror_field`
    // in R12b is held to R1a's standard, and a second evaluator that disagrees with the first by
    // one voxel is D204's fault in its worst form, two renderers computing one world.
    //
    // Written on the CPU first, deliberately. The shader is the part that cannot be debugged, and
    // every mistake this shape of evaluator can make — a frame that forgets which child it was on,
    // a transform applied on the way out instead of on the way in, an op that evaluates its child
    // more than once — is a mistake it can make here too, where a test can catch it. What is left
    // for `shaders/field.glsl` is a transliteration of something already proved.
    //
    // **Three ops make this more than a push-the-children loop, and they are the reason it is
    // worth writing carefully** (D643): `curvature` evaluates its child seven times, `occlusion`
    // fourteen and `facing` six, each at a different point, and `repeat` evaluates its child up to
    // eight times to check the leaning neighbours. So a frame carries a step counter over SAMPLE
    // POINTS, not over children, and the two cases share one mechanism.
    //
    // Returns false rather than a wrong number when it meets an op it does not mirror or runs out
    // of stack: "I could not" and "the answer is nought" must never be the same reply (trap 7).
    // `kMirrorStack` is 64 because the deepest path in the facility's field is 41 and the deepest
    // any evaluation actually went is 36 (D643).
    static constexpr u32 kMirrorStack = 64;
    bool mirror_eval(u32 at, Vec3 p, f64& out, u32* deepest = nullptr) const;

    // Whether every node reachable from `at` is one `mirror_eval` implements. The point of asking
    // separately is that a clip using an op the mirror has not learned yet should say so once, by
    // name, rather than fail one voxel at a time.
    bool mirror_covers(u32 at, Op* missing = nullptr) const;

    usize size() const { return nodes_.size(); }
    const Node& node(u32 index) const { return nodes_[index]; }

    // How well the field can cull, which is what decides what ONE evaluation costs.
    //
    // A node with no box cannot be skipped, and a hierarchy built over boxes that are all
    // "everywhere" is a hierarchy that visits everything. Both are invisible from the outside —
    // the answers stay right and the build simply takes twenty times as long — so they are
    // reported rather than left to be inferred.
    usize unbounded_nodes() const {
        usize n = 0;
        for (const Aabb& box : bounds_) { if (box.infinite()) ++n; }
        return n;
    }
    usize unaccelerated_unions() const {
        usize n = 0;
        for (const Node& node : nodes_) {
            if (node.op == Op::Union && node.children >= 4) ++n;
        }
        return n;
    }

    // WHICH ops the boxless nodes are, and — the part that decides where the work is — whether
    // each one is the SOURCE of its own infinity or merely standing above one.
    //
    // `unbounded_nodes()` above says a quarter of the facility's field cannot be culled. That
    // number on its own points at nothing: an unbounded node makes every ancestor unbounded too,
    // so most of the count is unions and translates that would bound perfectly well if the one
    // twist underneath them did. Bounding a source fixes it and everything over it at once;
    // "bounding" a downstream node is not a thing that can be done.
    //
    // So the split is the instrument, and the rule for it is exactly the one build_bounds uses:
    // a boxless node with a boxless CHILD is downstream, and a boxless node whose children are
    // all bounded (or which has none) is where the infinity is made.
    struct UnboundedCause {
        Op op = Op::Constant;
        usize source = 0;       // its own box came out everywhere() with every child bounded
        usize downstream = 0;   // boxless only because something under it is
    };

    // Sorted by `source` and then by `downstream`, both descending, so the top of the list is the
    // list of things worth bounding. Ops that bound everywhere they appear are left out.
    std::vector<UnboundedCause> unbounded_by_op() const;

    // How wide a union has to be before a hierarchy is built over its children's boxes — and it
    // is OFF by default, because measured on the facility it costs a fifth of the sample and
    // returns nothing (D637).
    //
    // The reason is written a few lines below this class's own accelerator field, and it was
    // written before the accelerator was built: "the parts of a building are LAYERS and not
    // regions. Walls, windows, entablature, roof — every one of their bounding boxes spans the
    // whole block, so a point in a wall is inside a dozen of them and a dozen subtrees really are
    // candidates. No ordering and no rejection changes that." A hierarchy that cannot reject is a
    // traversal paid on top of the linear scan it was meant to replace.
    //
    // Measured, interleaved, one build, two rounds, `clips/facility.clip` at metre 16:
    // **67.2 / 65.0 s with hierarchies against 53.6 / 53.9 s without**, and the same content hash
    // `6d3f383476ba93ac` either way. At metre 8 it is 11.3 / 11.3 against 8.6 / 8.8, and lowering
    // the threshold to 4 — the "more hierarchies must be better" arm — is worse again at 12.2 /
    // 12.9. On clips where no union is wide enough to engage it, the two arms measure the same,
    // which is what says the gap is real and not the harness.
    //
    // Kept rather than deleted, and settable rather than constant, for two reasons: a clip of
    // separated buildings is the case it was written for and nobody has authored one yet, and
    // both arms of the A/B have to be ONE build — D407's rule, which the renderer has followed
    // since, applied to the CPU side. Set it before `build_bounds()`; that is all that reads it.
    static constexpr usize kAccelerateNever = static_cast<usize>(-1);
    static constexpr usize kAccelerateFromDefault = kAccelerateNever;
    void accelerate_from(usize leaves) { accelerate_from_ = leaves; }
    usize accelerate_from() const { return accelerate_from_; }

private:
    u32 push(const Node& n);
    u32 combine(Op op, const std::vector<u32>& parts, f64 blend);

    // A tree over WHERE things are, built to replace the tree over how they were written.
    //
    // A union asks every child that the point could be nearest to. Written as a handful of parts
    // that is a handful of questions; written as a building it is not, because the parts of a
    // building are LAYERS and not regions. Walls, windows, entablature, roof — every one of their
    // bounding boxes spans the whole block, so a point in a wall is inside a dozen of them and a
    // dozen subtrees really are candidates. No ordering and no rejection changes that, because
    // they are all genuinely possible answers.
    //
    // Measured: the facility is 3474 nodes where the previous building was 126, and an evaluation
    // cost 3.4 microseconds against 312 nanoseconds. Settling was fine; the cost was walking the
    // tree.
    //
    // So the union is flattened to its leaves and a bounding hierarchy is built over THOSE. A
    // leaf is whatever is under a union that is not itself a union — a carved wall, a turned
    // baluster, a primitive — and its box is tight around it rather than around the layer it was
    // filed in. The hierarchy is then walked nearest-box-first, and a subtree whose box is
    // further away than the running answer is skipped whole.
    //
    // The point of it is not the constant factor. It is that the cost of evaluating a clip stops
    // depending on how the author chose to group it, which is what a language that lets twenty
    // people write one building has to be able to promise.
    struct BvhNode {
        Aabb box;
        u32 left = 0;    // internal: the first child. leaf: the first index into `order`
        u32 right = 0;   // internal: the second child
        u32 count = 0;   // 0 for an internal node
    };
    struct Accelerator {
        std::vector<BvhNode> nodes;
        std::vector<u32> order;   // field node indices, grouped by leaf
    };

    void flatten_union(u32 at, std::vector<u32>& leaves) const;
    u32 build_bvh(Accelerator& bvh, std::vector<u32>& work, usize begin, usize end) const;
    f64 eval_accelerated(const Accelerator& bvh, Vec3 p) const;

    std::vector<Node> nodes_;
    std::vector<f64> parameters_;
    std::vector<std::string> names_;
    std::vector<Aabb> bounds_;   // empty until build_bounds(); never required for correctness

    // One per node, and almost all of them empty: only a union wide enough to be worth it gets
    // an accelerator, and only the outermost union of a chain, because flattening reaches
    // everything below it anyway.
    std::vector<u32> accelerator_of_;   // node -> index into accelerators_, or kNoAccelerator
    std::vector<Accelerator> accelerators_;
    static constexpr u32 kNoAccelerator = 0xFFFFFFFFu;

    usize accelerate_from_ = kAccelerateFromDefault;
};

}  // namespace forge
}  // namespace ws
