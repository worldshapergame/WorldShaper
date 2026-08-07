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

    // The surface normal, by central differences. Used for painting by facing — "moss on the
    // top, soot under the arch" — and for nothing else, so its cost is paid only where a rule
    // asks for it.
    Vec3 normal_at(u32 at, Vec3 p, f64 step = 0.01) const;

    usize size() const { return nodes_.size(); }
    const Node& node(u32 index) const { return nodes_[index]; }

private:
    u32 push(const Node& n);
    u32 combine(Op op, const std::vector<u32>& parts, f64 blend);

    std::vector<Node> nodes_;
    std::vector<f64> parameters_;
    std::vector<std::string> names_;
    std::vector<Aabb> bounds_;   // empty until build_bounds(); never required for correctness
};

}  // namespace forge
}  // namespace ws
