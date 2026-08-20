// Evaluating one expression at MANY points in a single traversal.
//
// # Why this file exists, in one measurement
//
// `Field::eval` answers one point, and D722 counted what that costs on the estate: **632 field-node
// visits at 15.5 nanoseconds a visit**, about ten microseconds an answer. A node of the render tree
// is eight voxels a side, and the sampler's descent asks about **640 points inside a box a quarter
// of a metre across** to fill it. Every one of those 640 walks visits the same nodes, tests the same
// boxes and takes the same turns, because every one of them is about a point inside the same small
// box.
//
// So the traversal is paid 640 times for one shape's worth of information. This walks it once and
// carries all 640 points down it.
//
// # The one promise
//
// **The same answer as `Field::eval`, bit for bit, for every point.** Not nearly, not within an
// epsilon: the sampler settles boxes on these numbers and every world in this repository is gated on
// a content hash, so a last-bit difference is a different world. `tests/test_field_block.cpp` is
// that promise written down, and `clips/sampler.clip` building `d0d5f84c685be847` at 1,430,104 solid
// voxels is it measured end to end.
//
// That promise is what decides the shape of everything here. Every expression below is written in
// the same order and the same precision as the corresponding line of `Field::eval`, and where the
// two could be written differently they are not.
//
// ## The cull is replayed PER POINT, and that is deliberate
//
// It is tempting — and it is what the first sketch of this file said — to cull a union child once
// for the whole block, on the block's own bounds, on the grounds that a child rejected because the
// running answer already beats its box was never going to win the minimum anyway.
//
// **That is exactly the assumption D644 measured and found false.** `sd_ellipsoid`, `sd_cone`,
// `sd_prism` and `sd_platonic` are bounded approximations, not exact distances: asked outside their
// own box they can answer as little as 0.53 of the distance to it, and an intersection under a union
// can do the same. So a child `eval` rejected on its box may report LESS than the running answer,
// and evaluating it — which is what a more conservative block-level cull would do — gives a
// different number. Small, rare (D644 counted 58 points in 64,000 on the facility), and fatal to a
// content hash.
//
// So the block-level cull here is not conservative, it is IDENTICAL: every point gets its own box
// distances, its own sort, and its own break, and a child is evaluated over exactly the sub-block of
// points that `eval` would have evaluated it for. Points are gathered into a compacted sub-block
// before the recursion, so a child that only three points of five hundred still need is walked with
// three points and not with five hundred.
//
// # Where the saving actually is
//
// Not in vector instructions. The things paid once per BLOCK instead of once per point are the
// pointer chase into `nodes_`, the switch on `n.op`, the load of each child's `Aabb` out of
// `bounds_`, and every quantity a transform can hoist — a `rotate` costs six trigonometric calls per
// visit in `eval` and six per BLOCK here. D722 says that is where the cost is, because 15.5 ns a
// visit for a handful of flops is a memory-bound walk rather than an arithmetic-bound one.
//
// # What still goes one point at a time, and why
//
// **Most leaves.** `sd_prism`, `sd_platonic`, `sd_spiral`, `value_noise`, `cell_noise` and the rest
// live in the anonymous namespace of `field.cpp` and cannot be reached from here. Re-deriving one is
// the change that could break the promise silently — two copies of a distance function drift, and
// the drift is a last bit — so those are answered by `count` calls to `Field::eval`, which is the
// same function the promise is measured against.
//
// **Seven are copied anyway, and the line is a measurement.** The facility's field, reachable from
// its solid, holds 1,882 boxes, 573 cylinders, 245 capsules, 208 ellipsoids, 154 spheres, 95 planes
// and 53 tori — 3,210 of its 3,287 shape leaves, against seventy-seven of everything else. Leaving
// those seven to `eval` leaves the switch and the call being paid per point over most of the walk.
// So they are copied, in five short functions beside their originals, and `tests/test_field_block.cpp`
// checks each against `Field::eval` point for point — which is what turns "a copy might drift" from
// a silent failure into a failing test.
//
// **Three composite ops**, each for a reason of its own, and each named in the fallback list beside
// `block_walks`: a partial `revolve`, a `scatter`, and any union wide enough to have been given a
// bounding hierarchy. All three fall through to `Field::eval` per point, which keeps the promise by
// construction.

#include "forge/field.hpp"

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

namespace ws {
namespace forge {

// The visit counter defined in field.cpp, so a batched visit can be charged what a per-point one
// would have been: one visit per node per point, which is what D722's 632 is a count of.
//
// Declared here rather than in `field.hpp` because it is an instrument and not an interface —
// `field_visits()` is the way to read it and that IS in the header. This file is the only other
// thing that writes it, and it writes it for the one reason the counter exists: a block evaluator
// whose visits were counted differently from `eval`'s would make the two figures incomparable, which
// is the fault D722 was written to end.
extern thread_local u64 g_field_visits;

namespace {

// --- the same arithmetic, spelled the same way ------------------------------------------------
//
// Every function in this block is a character-for-character copy of the one in `field.cpp`'s
// anonymous namespace. They are copied rather than shared because sharing them means editing
// `field.cpp`, and they are small, total and covered point-for-point by `tests/test_field_block.cpp`
// against the originals. Anything longer than a few lines — the distance functions, the noise — is
// NOT copied and is answered by `Field::eval` instead. That is the line: a copy short enough to read
// beside its original in one screen, or no copy at all.

constexpr f64 kPi = 3.14159265358979323846;
constexpr f64 kTau = 2.0 * kPi;
constexpr f64 kInvRoot2 = 0.7071067811865476;

f64 axis_of(Vec3 p, u32 axis) {
    return (axis == 0) ? p.x : (axis == 1) ? p.y : p.z;
}

Vec3 with_axis(Vec3 p, u32 axis, f64 value) {
    if (axis == 0) p.x = value;
    else if (axis == 1) p.y = value;
    else p.z = value;
    return p;
}

void other_axes(u32 axis, u32& a, u32& b) {
    if (axis == 0) { a = 1; b = 2; }
    else if (axis == 1) { a = 0; b = 2; }
    else { a = 0; b = 1; }
}

f64 clamp(f64 v, f64 lo, f64 hi) { return (v < lo) ? lo : (v > hi) ? hi : v; }

f64 smooth_min(f64 a, f64 b, f64 k) {
    if (k <= 0.0) return std::min(a, b);
    const f64 h = clamp(0.5 + 0.5 * (b - a) / k, 0.0, 1.0);
    return b * (1.0 - h) + a * h - k * h * (1.0 - h);
}

f64 smooth_max(f64 a, f64 b, f64 k) { return -smooth_min(-a, -b, k); }

f64 chamfer_min(f64 a, f64 b, f64 k) {
    if (k <= 0.0) return std::min(a, b);
    const f64 plain = std::min(a, b);
    return std::min(plain, std::max((a + b - k) * kInvRoot2, plain - k * kInvRoot2));
}

f64 chamfer_max(f64 a, f64 b, f64 k) { return -chamfer_min(-a, -b, k); }

// No `infinite()` short cut, for the reason spelled out over the original: a box may be unbounded on
// one axis and bounded on another, and the arithmetic is already right for that case.
f64 squared_distance_to(const Field::Aabb& box, Vec3 p) {
    const f64 dx = std::max(std::max(box.low.x - p.x, p.x - box.high.x), 0.0);
    const f64 dy = std::max(std::max(box.low.y - p.y, p.y - box.high.y), 0.0);
    const f64 dz = std::max(std::max(box.low.z - p.z, p.z - box.high.z), 0.0);
    return dx * dx + dy * dy + dz * dz;
}

bool is_partial_sweep(f64 span) { return span > 0.0 && span < 1.0; }

f64 wrap_turn(f64 t) {
    const f64 w = t - std::floor(t);
    return (w < 1.0) ? w : 0.0;
}

f64 nearer_end(f64 rel, f64 span) {
    const f64 past_end = rel - span;
    const f64 before_start = 1.0 - rel;
    return (past_end <= before_start) ? past_end : -before_start;
}

// --- the seven distance functions that ARE copied ----------------------------------------------
//
// The static histogram of `clips/facility.clip` reachable from its solid: **1,882 boxes, 573
// cylinders, 245 capsules, 208 ellipsoids, 154 spheres, 95 planes and 53 tori** — 3,210 of its 3,287
// shape leaves. Everything else is a cone, a prism, a platonic solid, a spiral, a stair or a pattern,
// and those together are seventy-seven nodes.
//
// So these seven are copied and the rest are not, and the line is drawn on the measurement rather
// than on taste. Each is short enough to read beside its original in one screen, and each is checked
// against that original point for point by `tests/test_field_block.cpp` — which is the guard that
// makes a copy safe: if `field.cpp` changes one of them and this file does not, the test fails,
// because the test's whole content is "these two functions return the same double".
//
// `length` and `dot` are NOT copied. They are declared in `field.hpp` and defined once in
// `field.cpp`, so both evaluators call the same code and the arithmetic cannot drift.

f64 sd_box(Vec3 p, Vec3 half) {
    const Vec3 q{std::abs(p.x) - half.x, std::abs(p.y) - half.y, std::abs(p.z) - half.z};
    const Vec3 outside{std::max(q.x, 0.0), std::max(q.y, 0.0), std::max(q.z, 0.0)};
    const f64 inside = std::min(std::max(q.x, std::max(q.y, q.z)), 0.0);
    return length(outside) + inside;
}

f64 sd_cylinder(Vec3 p, f64 r, f64 half_height, u32 axis) {
    u32 a = 0, b = 0;
    other_axes(axis, a, b);
    const f64 radial = std::hypot(axis_of(p, a), axis_of(p, b)) - r;
    const f64 along = std::abs(axis_of(p, axis)) - half_height;
    const f64 outside = std::hypot(std::max(radial, 0.0), std::max(along, 0.0));
    return std::min(std::max(radial, along), 0.0) + outside;
}

f64 sd_capsule(Vec3 p, Vec3 a, Vec3 b, f64 r) {
    const Vec3 pa = p - a;
    const Vec3 ba = b - a;
    const f64 denom = dot(ba, ba);
    const f64 h = (denom > 0.0) ? clamp(dot(pa, ba) / denom, 0.0, 1.0) : 0.0;
    return length(pa - ba * h) - r;
}

f64 sd_torus(Vec3 p, f64 ring, f64 tube, u32 axis) {
    u32 a = 0, b = 0;
    other_axes(axis, a, b);
    const f64 radial = std::hypot(axis_of(p, a), axis_of(p, b)) - ring;
    return std::hypot(radial, axis_of(p, axis)) - tube;
}

f64 sd_ellipsoid(Vec3 p, Vec3 r) {
    const Vec3 safe{r.x > 0 ? r.x : 1e-9, r.y > 0 ? r.y : 1e-9, r.z > 0 ? r.z : 1e-9};
    const f64 k0 = length({p.x / safe.x, p.y / safe.y, p.z / safe.z});
    const f64 k1 = length({p.x / (safe.x * safe.x), p.y / (safe.y * safe.y), p.z / (safe.z * safe.z)});
    if (k1 == 0.0) return -std::min(safe.x, std::min(safe.y, safe.z));
    return k0 * (k0 - 1.0) / k1;
}

f64 polar_copy_turn(f64 turn, f64 from, f64 span, u32 count) {
    if (count <= 1) return from;
    const f64 step = span / static_cast<f64>(count - 1);
    const f64 rel = wrap_turn(turn - from);
    if (rel <= span) {
        const f64 k = clamp(std::round(rel / step), 0.0, static_cast<f64>(count - 1));
        return from + k * step;
    }
    return (nearer_end(rel, span) > 0.0) ? from + span : from;
}

// --- the scratch the traversal carries its blocks in ------------------------------------------
//
// A recursion that hands each node a buffer per child needs somewhere to put them, and it must not
// be `new` — `eval_block` is called from every sampler thread at once and a malloc per node visit
// would cost more than the traversal it is saving.
//
// So: a per-thread stack allocator over chunks that are allocated once and reused for the life of
// the thread. `here()` and `back_to()` are the whole of the discipline — a node takes what it needs,
// recurses, and gives it all back before it returns, so the high-water mark is the deepest path and
// not the whole tree.
//
// **Chunks are never reallocated**, which is the property the whole thing rests on: an outer node
// holds raw pointers into its own chunk while its children allocate, so a `std::vector<f64>` that
// grew would leave every one of those pointers dangling. New chunks are inserted after the one in
// use, which cannot move a chunk an outer frame is already pointing into.
class Scratch {
public:
    struct Mark {
        usize chunk = 0;
        usize used = 0;
    };

    Mark here() const { return Mark{current_, used_}; }
    void back_to(Mark m) { current_ = m.chunk; used_ = m.used; }

    f64* doubles(usize n) { return static_cast<f64*>(take(n * sizeof(f64))); }
    Vec3* points(usize n) { return static_cast<Vec3*>(take(n * sizeof(Vec3))); }
    u32* indices(usize n) { return static_cast<u32*>(take(n * sizeof(u32))); }
    u8* flags(usize n) { return static_cast<u8*>(take(n * sizeof(u8))); }

private:
    struct Chunk {
        std::unique_ptr<unsigned char[]> data;
        usize size = 0;
    };
    static constexpr usize kChunk = usize(1) << 18;   // 256 KB, which holds ten blocks of 640

    void* take(usize bytes) {
        bytes = (bytes + 15u) & ~usize(15u);
        if (bytes == 0) bytes = 16u;
        if (!chunks_.empty() && used_ + bytes <= chunks_[current_].size) {
            unsigned char* p = chunks_[current_].data.get() + used_;
            used_ += bytes;
            return p;
        }
        const usize next = chunks_.empty() ? 0u : current_ + 1u;
        if (next >= chunks_.size() || chunks_[next].size < bytes) {
            Chunk made;
            made.size = (bytes > kChunk) ? bytes : kChunk;
            made.data.reset(new unsigned char[made.size]);
            chunks_.insert(chunks_.begin() + static_cast<isize>(next), std::move(made));
        }
        current_ = next;
        used_ = bytes;
        return chunks_[current_].data.get();
    }

    std::vector<Chunk> chunks_;
    usize current_ = 0;
    usize used_ = 0;
};

Scratch& scratch() {
    static thread_local Scratch s;
    return s;
}

// How many points one node is allowed to ask its child about at once.
//
// `repeat` asks up to eight times, `occlusion` fourteen, `curvature` seven — so a block of 640 walks
// into a child block of 8,960 unless something says otherwise, and two of those nested is a hundred
// thousand points of scratch for one node visit. The fan-out ops slice their input instead: the
// batching is kept, the memory is bounded, and nothing about the answer changes because a slice is
// just a shorter block.
constexpr usize kMaxFan = 4096;

}  // namespace

void Field::eval_block(u32 root, const Vec3* points, usize count, f64* out) const {
    if (count == 0) return;

    // A local struct rather than a free function, and it is not a stylistic choice: the traversal
    // needs `nodes_`, `bounds_`, `accelerator_of_` and `eval_accelerated`, all of which are private,
    // and a local class of a member function has exactly the access rights of that member function.
    // The alternative is a declaration in `field.hpp`, and this file is meant to be replaceable
    // without touching the header the rest of the engine reads.
    struct Walk {
        const Field& f;
        Scratch& s;

        // Which ops carry a whole block down in one visit. Everything not on this list is answered
        // by `Field::eval`, one point at a time, which keeps the promise by construction.
        //
        // The three composites that are deliberately absent:
        //
        //   a PARTIAL `revolve`   its branch decides, per point, whether the answer comes from the
        //                         profile at the point's own radius or from an end cap at one of
        //                         two angles — three different recursions with three different
        //                         transformed points, chosen per point. Sliceable in principle and
        //                         not worth being wrong about.
        //   `scatter`             needs `scatter_point`, which needs the cell hash, which is forty
        //                         lines of `field.cpp`'s anonymous namespace. Its child is a pebble
        //                         — one node — so the walk it saves is one deep.
        //   an ACCELERATED union  the bounding hierarchy has a stack per point. Off by default
        //                         (`kAccelerateFromDefault` is `kAccelerateNever`, D637), so this
        //                         costs nothing unless somebody passes `--accelerate-from`.
        static bool block_walks(const Node& n) {
            switch (n.op) {
                case Op::Constant:
                case Op::Parameter:
                case Op::Coordinate:
                case Op::Radius:
                case Op::Sphere:
                case Op::Box:
                case Op::Cylinder:
                case Op::Capsule:
                case Op::Torus:
                case Op::Plane:
                case Op::Ellipsoid:
                case Op::Union:
                case Op::Intersection:
                case Op::Difference:
                case Op::SmoothUnion:
                case Op::SmoothIntersection:
                case Op::SmoothDifference:
                case Op::ChamferUnion:
                case Op::ChamferIntersection:
                case Op::ChamferDifference:
                case Op::Translate:
                case Op::Rotate:
                case Op::Scale:
                case Op::Mirror:
                case Op::Repeat:
                case Op::PolarRepeat:
                case Op::Shell:
                case Op::Round:
                case Op::Offset:
                case Op::Displace:
                case Op::Twist:
                case Op::Bend:
                case Op::Curvature:
                case Op::Occlusion:
                case Op::Facing:
                case Op::Add:
                case Op::Multiply:
                case Op::Min:
                case Op::Max:
                case Op::Blend:
                case Op::Remap:
                case Op::Abs:
                case Op::Negate:
                case Op::Step:
                case Op::Smoothstep:
                case Op::Clamp:
                case Op::Power:
                    return true;
                case Op::Revolve:
                    return !is_partial_sweep(n.a[5]);
                default:
                    return false;
            }
        }

        void one_at_a_time(u32 at, const Vec3* pts, usize count, f64* out) {
            for (usize i = 0; i < count; ++i) out[i] = f.eval(at, pts[i]);
        }

        // The child of a transform: fill `q` with the moved points, then recurse into it.
        void go(u32 at, const Vec3* pts, usize count, f64* out) {
            if (count == 0) return;
            if (at >= f.nodes_.size()) { one_at_a_time(at, pts, count, out); return; }
            const Node& n = f.nodes_[at];
            if (!block_walks(n)) { one_at_a_time(at, pts, count, out); return; }

            g_field_visits += count;
            const f64* a = n.a;
            const Scratch::Mark mark = s.here();

            switch (n.op) {
                // --- constants, coordinates and the seven copied primitives ----------------
                //
                // The switch and the `Node` load are paid once here where `eval` pays them per
                // point, and everything a node's `a[]` decides — the centre, the axis, the pair of
                // cross-axes, the parameter's own slot — is worked out once for the block.
                case Op::Constant: {
                    for (usize i = 0; i < count; ++i) out[i] = a[0];
                    break;
                }
                case Op::Parameter: {
                    const usize slot = static_cast<usize>(a[0]);
                    const f64 value = (slot < f.parameters_.size()) ? f.parameters_[slot] : 0.0;
                    for (usize i = 0; i < count; ++i) out[i] = value;
                    break;
                }
                case Op::Coordinate: {
                    const u32 axis = static_cast<u32>(a[0]);
                    for (usize i = 0; i < count; ++i) out[i] = axis_of(pts[i], axis);
                    break;
                }
                case Op::Radius: {
                    const Vec3 centre{a[0], a[1], a[2]};
                    for (usize i = 0; i < count; ++i) out[i] = length(pts[i] - centre);
                    break;
                }
                case Op::Sphere: {
                    const Vec3 centre{a[0], a[1], a[2]};
                    for (usize i = 0; i < count; ++i) out[i] = length(pts[i] - centre) - a[3];
                    break;
                }
                case Op::Box: {
                    const Vec3 centre{a[0], a[1], a[2]};
                    const Vec3 half{a[3] - a[6], a[4] - a[6], a[5] - a[6]};
                    for (usize i = 0; i < count; ++i) {
                        out[i] = sd_box(pts[i] - centre, half) - a[6];
                    }
                    break;
                }
                case Op::Cylinder: {
                    const Vec3 centre{a[0], a[1], a[2]};
                    const u32 axis = static_cast<u32>(a[5]);
                    for (usize i = 0; i < count; ++i) {
                        out[i] = sd_cylinder(pts[i] - centre, a[3], a[4], axis);
                    }
                    break;
                }
                case Op::Capsule: {
                    const Vec3 from{a[0], a[1], a[2]};
                    const Vec3 to{a[3], a[4], a[5]};
                    for (usize i = 0; i < count; ++i) out[i] = sd_capsule(pts[i], from, to, a[6]);
                    break;
                }
                case Op::Torus: {
                    const Vec3 centre{a[0], a[1], a[2]};
                    const u32 axis = static_cast<u32>(a[5]);
                    for (usize i = 0; i < count; ++i) {
                        out[i] = sd_torus(pts[i] - centre, a[3], a[4], axis);
                    }
                    break;
                }
                case Op::Plane: {
                    const Vec3 normal{a[0], a[1], a[2]};
                    for (usize i = 0; i < count; ++i) out[i] = dot(pts[i], normal) - a[3];
                    break;
                }
                case Op::Ellipsoid: {
                    const Vec3 centre{a[0], a[1], a[2]};
                    const Vec3 radii{a[3], a[4], a[5]};
                    for (usize i = 0; i < count; ++i) {
                        out[i] = sd_ellipsoid(pts[i] - centre, radii);
                    }
                    break;
                }

                // --- combining ------------------------------------------------------------
                case Op::Union: {
                    // The accelerated arm first, because it is the one that cannot be batched.
                    if (!f.accelerator_of_.empty() && f.accelerator_of_[at] != Field::kNoAccelerator) {
                        const Field::Accelerator& bvh = f.accelerators_[f.accelerator_of_[at]];
                        for (usize i = 0; i < count; ++i) out[i] = f.eval_accelerated(bvh, pts[i]);
                        break;
                    }

                    const u32 kids = n.children;
                    // Exactly `eval`'s condition. With `bounds_` empty every box is `everywhere()`,
                    // every distance is nought, the insertion sort is stable and the rejection test
                    // is `away > 0.0` — so the unsorted arm and a sorted arm over infinite boxes
                    // already agree, and this only has to match on the one that matters.
                    if (f.bounds_.empty() || kids <= 1) {
                        // Nothing to cull with: every child, in declaration order, over the whole
                        // block. Kept as its own arm rather than as a flag through the machinery
                        // below because it is the arm every field takes before `build_bounds()`, and
                        // paying for a sort and a bucket to do nothing measured.
                        go(n.child[0], pts, count, out);
                        if (kids > 1) {
                            f64* v = s.doubles(count);
                            for (u32 i = 1; i < kids; ++i) {
                                go(n.child[i], pts, count, v);
                                for (usize j = 0; j < count; ++j) out[j] = std::min(out[j], v[j]);
                            }
                        }
                        break;
                    }

                    // A BLOCK-LEVEL CONTAINMENT TEST WAS TRIED HERE AND MEASURED WORSE.
                    //
                    // A point inside a box is nought away from it exactly, so a child whose box
                    // swallows the whole block has all `count` of its box distances known without
                    // any of them being computed — one pass for the block's own bounds and six
                    // comparisons a child, against twelve arithmetic operations a point a child.
                    // D637's "the parts of a building are LAYERS and not regions" says that case
                    // should be the common one.
                    //
                    // It is not, on the compiled facility, and the arm is written down rather than
                    // left to be tried again: `swallows == kids` — the shortcut that skips the whole
                    // machinery — needs ALL four children to swallow the block, and a quarter of a
                    // metre inside a building sits inside some part boxes and outside most. The
                    // per-child half saves twelve operations a point for perhaps one child in four
                    // and spends six on every point to find out. Measured across all six places, in
                    // three interleaved rounds, it was **1.70x against 1.78x** — consistently worse
                    // at every one of them, so it came out.
                    f64* away = s.doubles(count * 4);
                    u8* order = s.flags(count * 4);
                    // Where each point is in its own order, with 0xFF for "finished" — one array
                    // rather than a position and a flag, because the whole of the union's cost that
                    // `eval` does not also pay is passes over arrays this size.
                    u8* step = s.flags(count);
                    u32* bucket = s.indices(count * 4);
                    Vec3* gathered = s.points(count);
                    f64* got = s.doubles(count);
                    static constexpr u8 kFinished = 0xFFu;

                    // ONE pass for the box distances, the sort AND the first round's buckets. Four
                    // passes over `count` was what the first version did, and out in open air —
                    // where the cull throws everything away and there is no real work to amortise
                    // them against — those passes were the whole of the difference between this and
                    // `eval`. The four boxes are hoisted into locals here, which is the load per
                    // child per BLOCK that D638's 31% of instructions was per child per point.
                    const Field::Aabb* boxes[4] = {nullptr, nullptr, nullptr, nullptr};
                    for (u32 c = 0; c < kids; ++c) boxes[c] = &f.bounds_[n.child[c]];
                    usize filled[4] = {0, 0, 0, 0};
                    for (usize i = 0; i < count; ++i) {
                        const Vec3 p = pts[i];
                        f64 aw[4];
                        u8 o[4] = {0, 1, 2, 3};
                        for (u32 c = 0; c < kids; ++c) aw[c] = squared_distance_to(*boxes[c], p);
                        // Four children at most, so an insertion sort is the whole of it — and it is
                        // `eval`'s own, stable, so equal distances keep declaration order.
                        for (u32 j = 1; j < kids; ++j) {
                            const u8 key = o[j];
                            const f64 key_away = aw[key];
                            u32 t = j;
                            while (t > 0 && aw[o[t - 1]] > key_away) { o[t] = o[t - 1]; --t; }
                            o[t] = key;
                        }
                        f64* keep_away = away + i * 4;
                        u8* keep_order = order + i * 4;
                        for (u32 c = 0; c < kids; ++c) {
                            keep_away[c] = aw[c];
                            keep_order[c] = o[c];
                        }
                        step[i] = 0;
                        bucket[static_cast<usize>(o[0]) * count + filled[o[0]]] =
                            static_cast<u32>(i);
                        ++filled[o[0]];
                    }

                    // One round per position in the order. Every point that is still going advances
                    // by one child, so there are at most four rounds; within a round the points are
                    // bucketed by WHICH child they want next, and each child is walked once with the
                    // points that want it. When the whole block agrees — which is what a block a
                    // quarter of a metre across nearly always does — that is one recursion a round,
                    // the bucket IS the block, and no gather happens at all.
                    for (;;) {
                        for (u32 c = 0; c < 4; ++c) {
                            if (filled[c] == 0) continue;
                            const u32* mine = bucket + static_cast<usize>(c) * count;
                            const Vec3* sub = pts;
                            if (filled[c] != count) {
                                for (usize j = 0; j < filled[c]; ++j) gathered[j] = pts[mine[j]];
                                sub = gathered;
                            }
                            go(n.child[c], sub, filled[c], got);
                            for (usize j = 0; j < filled[c]; ++j) {
                                const usize i = mine[j];
                                out[i] = (step[i] == 0) ? got[j] : std::min(out[i], got[j]);
                                ++step[i];
                                if (static_cast<u32>(step[i]) >= kids) step[i] = kFinished;
                            }
                        }
                        filled[0] = 0; filled[1] = 0; filled[2] = 0; filled[3] = 0;
                        bool any = false;
                        for (usize i = 0; i < count; ++i) {
                            if (step[i] == kFinished) continue;
                            const u32 c = order[i * 4 + step[i]];
                            const f64 aw = away[i * 4 + c];
                            const f64 d = out[i];
                            // `eval`'s `break`: the children are in ascending box distance and the
                            // running answer only shrinks, so a child rejected here proves every
                            // child after it is rejected too.
                            if (aw > 0.0 && (d < 0.0 || d * d <= aw)) { step[i] = kFinished; continue; }
                            bucket[static_cast<usize>(c) * count + filled[c]] = static_cast<u32>(i);
                            ++filled[c];
                            any = true;
                        }
                        if (!any) break;
                    }
                    break;
                }
                case Op::Intersection: {
                    f64* v = s.doubles(count);
                    go(n.child[0], pts, count, out);
                    for (u32 i = 1; i < n.children; ++i) {
                        go(n.child[i], pts, count, v);
                        for (usize j = 0; j < count; ++j) out[j] = std::max(out[j], v[j]);
                    }
                    break;
                }
                case Op::Difference: {
                    go(n.child[0], pts, count, out);
                    if (n.children <= 1) break;

                    u32* idx = s.indices(count);
                    Vec3* gathered = s.points(count);
                    f64* got = s.doubles(count);
                    for (u32 i = 1; i < n.children; ++i) {
                        usize m = 0;
                        if (f.bounds_.empty()) {
                            m = count;
                        } else {
                            const Field::Aabb& box = f.bounds_[n.child[i]];
                            for (usize j = 0; j < count; ++j) {
                                const f64 away = squared_distance_to(box, pts[j]);
                                const f64 d = out[j];
                                if (away > 0.0 && (d >= 0.0 || d * d <= away)) continue;
                                idx[m] = static_cast<u32>(j);
                                gathered[m] = pts[j];
                                ++m;
                            }
                        }
                        if (m == 0) continue;
                        if (m == count) {
                            go(n.child[i], pts, count, got);
                            for (usize j = 0; j < count; ++j) out[j] = std::max(out[j], -got[j]);
                            continue;
                        }
                        go(n.child[i], gathered, m, got);
                        for (usize j = 0; j < m; ++j) {
                            const usize t = idx[j];
                            out[t] = std::max(out[t], -got[j]);
                        }
                    }
                    break;
                }
                case Op::SmoothUnion: {
                    f64* v = s.doubles(count);
                    go(n.child[0], pts, count, out);
                    for (u32 i = 1; i < n.children; ++i) {
                        go(n.child[i], pts, count, v);
                        for (usize j = 0; j < count; ++j) out[j] = smooth_min(out[j], v[j], a[0]);
                    }
                    break;
                }
                case Op::SmoothIntersection: {
                    f64* v = s.doubles(count);
                    go(n.child[0], pts, count, out);
                    for (u32 i = 1; i < n.children; ++i) {
                        go(n.child[i], pts, count, v);
                        for (usize j = 0; j < count; ++j) out[j] = smooth_max(out[j], v[j], a[0]);
                    }
                    break;
                }
                case Op::SmoothDifference: {
                    f64* v = s.doubles(count);
                    go(n.child[0], pts, count, out);
                    for (u32 i = 1; i < n.children; ++i) {
                        go(n.child[i], pts, count, v);
                        for (usize j = 0; j < count; ++j) out[j] = smooth_max(out[j], -v[j], a[0]);
                    }
                    break;
                }
                case Op::ChamferUnion: {
                    f64* v = s.doubles(count);
                    go(n.child[0], pts, count, out);
                    for (u32 i = 1; i < n.children; ++i) {
                        go(n.child[i], pts, count, v);
                        for (usize j = 0; j < count; ++j) out[j] = chamfer_min(out[j], v[j], a[0]);
                    }
                    break;
                }
                case Op::ChamferIntersection: {
                    f64* v = s.doubles(count);
                    go(n.child[0], pts, count, out);
                    for (u32 i = 1; i < n.children; ++i) {
                        go(n.child[i], pts, count, v);
                        for (usize j = 0; j < count; ++j) out[j] = chamfer_max(out[j], v[j], a[0]);
                    }
                    break;
                }
                case Op::ChamferDifference: {
                    f64* v = s.doubles(count);
                    go(n.child[0], pts, count, out);
                    for (u32 i = 1; i < n.children; ++i) {
                        go(n.child[i], pts, count, v);
                        for (usize j = 0; j < count; ++j) out[j] = chamfer_max(out[j], -v[j], a[0]);
                    }
                    break;
                }

                // --- moving the point ----------------------------------------------------
                case Op::Translate: {
                    Vec3* q = s.points(count);
                    const Vec3 by{a[0], a[1], a[2]};
                    for (usize i = 0; i < count; ++i) q[i] = pts[i] - by;
                    go(n.child[0], q, count, out);
                    break;
                }
                case Op::Rotate: {
                    // Six trigonometric calls for the whole block, where `eval` pays six a point.
                    const f64 cx = std::cos(-a[0] * kTau), sx = std::sin(-a[0] * kTau);
                    const f64 cy = std::cos(-a[1] * kTau), sy = std::sin(-a[1] * kTau);
                    const f64 cz = std::cos(-a[2] * kTau), sz = std::sin(-a[2] * kTau);
                    Vec3* out_pts = s.points(count);
                    for (usize i = 0; i < count; ++i) {
                        Vec3 q = pts[i];
                        q = {q.x, q.y * cx - q.z * sx, q.y * sx + q.z * cx};
                        q = {q.x * cy + q.z * sy, q.y, -q.x * sy + q.z * cy};
                        q = {q.x * cz - q.y * sz, q.x * sz + q.y * cz, q.z};
                        out_pts[i] = q;
                    }
                    go(n.child[0], out_pts, count, out);
                    break;
                }
                case Op::Scale: {
                    const Vec3 sc{a[0] != 0.0 ? a[0] : 1.0, a[1] != 0.0 ? a[1] : 1.0,
                                  a[2] != 0.0 ? a[2] : 1.0};
                    const f64 smallest =
                        std::min(std::abs(sc.x), std::min(std::abs(sc.y), std::abs(sc.z)));
                    Vec3* q = s.points(count);
                    for (usize i = 0; i < count; ++i) {
                        q[i] = {pts[i].x / sc.x, pts[i].y / sc.y, pts[i].z / sc.z};
                    }
                    go(n.child[0], q, count, out);
                    for (usize i = 0; i < count; ++i) out[i] = out[i] * smallest;
                    break;
                }
                case Op::Mirror: {
                    const u32 axis = static_cast<u32>(a[0]);
                    Vec3* q = s.points(count);
                    for (usize i = 0; i < count; ++i) {
                        q[i] = with_axis(pts[i], axis, std::abs(axis_of(pts[i], axis)));
                    }
                    go(n.child[0], q, count, out);
                    break;
                }
                case Op::Repeat: {
                    // Up to eight queries a point — the fold, and every combination of the
                    // neighbours it leans toward — so the block is sliced to keep the child's block
                    // under the fan-out ceiling.
                    const usize slice = kMaxFan / 8;
                    for (usize base = 0; base < count; base += slice) {
                        const usize m = std::min(slice, count - base);
                        const Scratch::Mark inner = s.here();
                        Vec3* q = s.points(m * 8);
                        u32* first = s.indices(m + 1);
                        usize total = 0;
                        for (usize i = 0; i < m; ++i) {
                            const Vec3 p = pts[base + i];
                            first[i] = static_cast<u32>(total);
                            Vec3 folded = p;
                            u32 axes[3];
                            f64 leaning[3];
                            u32 neighbours = 0;
                            for (u32 axis = 0; axis < 3; ++axis) {
                                const f64 period = a[axis];
                                if (period <= 0.0) continue;
                                const f64 limit = a[3 + axis];
                                const f64 value = axis_of(p, axis);
                                f64 cell = std::round(value / period);
                                if (limit > 0.0) cell = clamp(cell, -limit, limit);
                                const f64 at_cell = value - period * cell;
                                folded = with_axis(folded, axis, at_cell);

                                f64 other = cell + ((at_cell >= 0.0) ? 1.0 : -1.0);
                                if (limit > 0.0) other = clamp(other, -limit, limit);
                                if (other != cell) {
                                    axes[neighbours] = axis;
                                    leaning[neighbours] = value - period * other;
                                    ++neighbours;
                                }
                            }
                            q[total++] = folded;
                            for (u32 mask = 1; mask < (1u << neighbours); ++mask) {
                                Vec3 shifted = folded;
                                for (u32 k = 0; k < neighbours; ++k) {
                                    if ((mask >> k) & 1u) {
                                        shifted = with_axis(shifted, axes[k], leaning[k]);
                                    }
                                }
                                q[total++] = shifted;
                            }
                        }
                        first[m] = static_cast<u32>(total);
                        f64* vals = s.doubles(total);
                        go(n.child[0], q, total, vals);
                        for (usize i = 0; i < m; ++i) {
                            f64 best = vals[first[i]];
                            for (u32 k = first[i] + 1; k < first[i + 1]; ++k) {
                                best = std::min(best, vals[k]);
                            }
                            out[base + i] = best;
                        }
                        s.back_to(inner);
                    }
                    break;
                }
                case Op::PolarRepeat: {
                    const u32 copies = std::max(1u, static_cast<u32>(a[0]));
                    const u32 axis = static_cast<u32>(a[1]);
                    u32 u = 0, v = 0;
                    other_axes(axis, u, v);
                    const bool partial = is_partial_sweep(a[3]);
                    const f64 sector = kTau / static_cast<f64>(copies);
                    Vec3* q = s.points(count);
                    for (usize i = 0; i < count; ++i) {
                        const Vec3 p = pts[i];
                        const f64 x = axis_of(p, u), y = axis_of(p, v);
                        f64 angle = std::atan2(y, x);
                        if (!partial) {
                            angle -= sector * std::round(angle / sector);
                        } else {
                            angle -= kTau * polar_copy_turn(angle / kTau, a[2], a[3], copies);
                        }
                        const f64 r = std::hypot(x, y);
                        Vec3 moved = p;
                        moved = with_axis(moved, u, std::cos(angle) * r);
                        moved = with_axis(moved, v, std::sin(angle) * r);
                        q[i] = moved;
                    }
                    go(n.child[0], q, count, out);
                    break;
                }

                // --- swept ---------------------------------------------------------------
                case Op::Revolve: {
                    // The whole-turn arm only; `block_walks` sends a partial sweep to `eval`.
                    const u32 axis = static_cast<u32>(a[3]);
                    u32 u = 0, v = 0;
                    other_axes(axis, u, v);
                    const Vec3 centre{a[0], a[1], a[2]};
                    Vec3* q = s.points(count);
                    for (usize i = 0; i < count; ++i) {
                        const Vec3 d = pts[i] - centre;
                        const f64 r = std::hypot(axis_of(d, u), axis_of(d, v));
                        Vec3 flat{0, 0, 0};
                        flat = with_axis(flat, u, r);
                        flat = with_axis(flat, axis, axis_of(d, axis));
                        q[i] = flat;
                    }
                    go(n.child[0], q, count, out);
                    break;
                }

                // --- changing the answer -------------------------------------------------
                case Op::Shell: {
                    go(n.child[0], pts, count, out);
                    for (usize i = 0; i < count; ++i) out[i] = std::abs(out[i]) - a[0];
                    break;
                }
                case Op::Round: {
                    go(n.child[0], pts, count, out);
                    for (usize i = 0; i < count; ++i) out[i] = out[i] - a[0];
                    break;
                }
                case Op::Offset: {
                    go(n.child[0], pts, count, out);
                    for (usize i = 0; i < count; ++i) out[i] = out[i] + a[0];
                    break;
                }
                case Op::Displace: {
                    f64* v = s.doubles(count);
                    go(n.child[0], pts, count, out);
                    go(n.child[1], pts, count, v);
                    for (usize i = 0; i < count; ++i) out[i] = out[i] + a[0] * v[i];
                    break;
                }
                case Op::Twist: {
                    const u32 axis = static_cast<u32>(a[1]);
                    u32 u = 0, v = 0;
                    other_axes(axis, u, v);
                    Vec3* q = s.points(count);
                    for (usize i = 0; i < count; ++i) {
                        const Vec3 p = pts[i];
                        const f64 angle = -a[0] * kTau * axis_of(p, axis);
                        const f64 c = std::cos(angle), sn = std::sin(angle);
                        const f64 x = axis_of(p, u), y = axis_of(p, v);
                        Vec3 moved = p;
                        moved = with_axis(moved, u, x * c - y * sn);
                        moved = with_axis(moved, v, x * sn + y * c);
                        q[i] = moved;
                    }
                    go(n.child[0], q, count, out);
                    break;
                }
                case Op::Bend: {
                    const u32 axis = static_cast<u32>(a[1]);
                    u32 u = 0, v = 0;
                    other_axes(axis, u, v);
                    Vec3* q = s.points(count);
                    for (usize i = 0; i < count; ++i) {
                        const Vec3 p = pts[i];
                        const f64 angle = -a[0] * kTau * axis_of(p, u);
                        const f64 c = std::cos(angle), sn = std::sin(angle);
                        const f64 x = axis_of(p, u), y = axis_of(p, v);
                        Vec3 moved = p;
                        moved = with_axis(moved, u, x * c - y * sn);
                        moved = with_axis(moved, v, x * sn + y * c);
                        q[i] = moved;
                    }
                    go(n.child[0], q, count, out);
                    break;
                }

                // --- what the shape is doing here ----------------------------------------
                case Op::Curvature: {
                    const f64 r = (a[0] > 0.0) ? a[0] : 0.05;
                    const usize slice = kMaxFan / 7;
                    for (usize base = 0; base < count; base += slice) {
                        const usize m = std::min(slice, count - base);
                        const Scratch::Mark inner = s.here();
                        Vec3* q = s.points(m * 7);
                        for (usize i = 0; i < m; ++i) {
                            const Vec3 p = pts[base + i];
                            q[i * 7 + 0] = p;
                            q[i * 7 + 1] = {p.x + r, p.y, p.z};
                            q[i * 7 + 2] = {p.x - r, p.y, p.z};
                            q[i * 7 + 3] = {p.x, p.y + r, p.z};
                            q[i * 7 + 4] = {p.x, p.y - r, p.z};
                            q[i * 7 + 5] = {p.x, p.y, p.z + r};
                            q[i * 7 + 6] = {p.x, p.y, p.z - r};
                        }
                        f64* vals = s.doubles(m * 7);
                        go(n.child[0], q, m * 7, vals);
                        for (usize i = 0; i < m; ++i) {
                            const f64* w = vals + i * 7;
                            const f64 centre = w[0];
                            f64 sum = 0.0;
                            sum += w[1];
                            sum += w[2];
                            sum += w[3];
                            sum += w[4];
                            sum += w[5];
                            sum += w[6];
                            out[base + i] = (sum / 6.0 - centre) / r;
                        }
                        s.back_to(inner);
                    }
                    break;
                }
                case Op::Occlusion: {
                    const f64 r = (a[0] > 0.0) ? a[0] : 0.15;
                    const f64 k = 0.5773502691896258;
                    const Vec3 dirs[14] = {{1, 0, 0},   {-1, 0, 0}, {0, 1, 0},   {0, -1, 0},
                                           {0, 0, 1},   {0, 0, -1}, {k, k, k},   {k, k, -k},
                                           {k, -k, k},  {k, -k, -k}, {-k, k, k}, {-k, k, -k},
                                           {-k, -k, k}, {-k, -k, -k}};
                    const usize slice = kMaxFan / 14;
                    for (usize base = 0; base < count; base += slice) {
                        const usize m = std::min(slice, count - base);
                        const Scratch::Mark inner = s.here();
                        Vec3* q = s.points(m * 14);
                        for (usize i = 0; i < m; ++i) {
                            const Vec3 p = pts[base + i];
                            for (u32 d = 0; d < 14; ++d) q[i * 14 + d] = p + dirs[d] * r;
                        }
                        f64* vals = s.doubles(m * 14);
                        go(n.child[0], q, m * 14, vals);
                        for (usize i = 0; i < m; ++i) {
                            u32 inside = 0;
                            for (u32 d = 0; d < 14; ++d) {
                                if (vals[i * 14 + d] < 0.0) ++inside;
                            }
                            out[base + i] = static_cast<f64>(inside) / 14.0;
                        }
                        s.back_to(inner);
                    }
                    break;
                }
                case Op::Facing: {
                    // `normal_at`'s six central differences, batched. The step is `eval`'s.
                    const f64 h = (a[1] > 0.0) ? a[1] : 0.02;
                    const u32 axis = static_cast<u32>(a[0]);
                    const usize slice = kMaxFan / 6;
                    for (usize base = 0; base < count; base += slice) {
                        const usize m = std::min(slice, count - base);
                        const Scratch::Mark inner = s.here();
                        Vec3* q = s.points(m * 6);
                        for (usize i = 0; i < m; ++i) {
                            const Vec3 p = pts[base + i];
                            q[i * 6 + 0] = {p.x + h, p.y, p.z};
                            q[i * 6 + 1] = {p.x - h, p.y, p.z};
                            q[i * 6 + 2] = {p.x, p.y + h, p.z};
                            q[i * 6 + 3] = {p.x, p.y - h, p.z};
                            q[i * 6 + 4] = {p.x, p.y, p.z + h};
                            q[i * 6 + 5] = {p.x, p.y, p.z - h};
                        }
                        f64* vals = s.doubles(m * 6);
                        go(n.child[0], q, m * 6, vals);
                        for (usize i = 0; i < m; ++i) {
                            const f64* w = vals + i * 6;
                            const Vec3 normal = normalise({w[0] - w[1], w[2] - w[3], w[4] - w[5]});
                            out[base + i] =
                                (axis == 0) ? normal.x : (axis == 1) ? normal.y : normal.z;
                        }
                        s.back_to(inner);
                    }
                    break;
                }

                // --- arithmetic ----------------------------------------------------------
                case Op::Add: {
                    f64* v = s.doubles(count);
                    go(n.child[0], pts, count, out);
                    for (u32 i = 1; i < n.children; ++i) {
                        go(n.child[i], pts, count, v);
                        for (usize j = 0; j < count; ++j) out[j] += v[j];
                    }
                    break;
                }
                case Op::Multiply: {
                    // `eval` stops a point at its first nought factor, and so does this: a point
                    // whose product is already nought is simply not gathered into the next child's
                    // block, and when no point is left the remaining children are never walked.
                    go(n.child[0], pts, count, out);
                    u32* idx = s.indices(count);
                    Vec3* gathered = s.points(count);
                    f64* got = s.doubles(count);
                    for (u32 i = 1; i < n.children; ++i) {
                        usize m = 0;
                        for (usize j = 0; j < count; ++j) {
                            if (out[j] == 0.0) continue;
                            idx[m] = static_cast<u32>(j);
                            gathered[m] = pts[j];
                            ++m;
                        }
                        if (m == 0) break;
                        go(n.child[i], (m == count) ? pts : gathered, m, got);
                        for (usize j = 0; j < m; ++j) out[idx[j]] *= got[j];
                    }
                    break;
                }
                case Op::Min: {
                    f64* v = s.doubles(count);
                    go(n.child[0], pts, count, out);
                    for (u32 i = 1; i < n.children; ++i) {
                        go(n.child[i], pts, count, v);
                        for (usize j = 0; j < count; ++j) out[j] = std::min(out[j], v[j]);
                    }
                    break;
                }
                case Op::Max: {
                    f64* v = s.doubles(count);
                    go(n.child[0], pts, count, out);
                    for (u32 i = 1; i < n.children; ++i) {
                        go(n.child[i], pts, count, v);
                        for (usize j = 0; j < count; ++j) out[j] = std::max(out[j], v[j]);
                    }
                    break;
                }
                case Op::Blend: {
                    const f64 t = clamp(a[0], 0.0, 1.0);
                    f64* v = s.doubles(count);
                    go(n.child[0], pts, count, out);
                    go(n.child[1], pts, count, v);
                    for (usize i = 0; i < count; ++i) out[i] = out[i] * (1.0 - t) + v[i] * t;
                    break;
                }
                case Op::Remap: {
                    go(n.child[0], pts, count, out);
                    const f64 span = a[1] - a[0];
                    for (usize i = 0; i < count; ++i) {
                        const f64 t = (span != 0.0) ? clamp((out[i] - a[0]) / span, 0.0, 1.0) : 0.0;
                        out[i] = a[2] + (a[3] - a[2]) * t;
                    }
                    break;
                }
                case Op::Abs: {
                    go(n.child[0], pts, count, out);
                    for (usize i = 0; i < count; ++i) out[i] = std::abs(out[i]);
                    break;
                }
                case Op::Negate: {
                    go(n.child[0], pts, count, out);
                    for (usize i = 0; i < count; ++i) out[i] = -out[i];
                    break;
                }
                case Op::Step: {
                    go(n.child[0], pts, count, out);
                    for (usize i = 0; i < count; ++i) out[i] = (out[i] > a[0]) ? 1.0 : 0.0;
                    break;
                }
                case Op::Smoothstep: {
                    go(n.child[0], pts, count, out);
                    const f64 span = a[1] - a[0];
                    if (span == 0.0) {
                        for (usize i = 0; i < count; ++i) out[i] = (out[i] > a[0]) ? 1.0 : 0.0;
                    } else {
                        for (usize i = 0; i < count; ++i) {
                            const f64 t = clamp((out[i] - a[0]) / span, 0.0, 1.0);
                            out[i] = t * t * (3.0 - 2.0 * t);
                        }
                    }
                    break;
                }
                case Op::Clamp: {
                    go(n.child[0], pts, count, out);
                    for (usize i = 0; i < count; ++i) out[i] = clamp(out[i], a[0], a[1]);
                    break;
                }
                case Op::Power: {
                    go(n.child[0], pts, count, out);
                    for (usize i = 0; i < count; ++i) {
                        const f64 v = out[i];
                        const f64 mag = std::pow(std::abs(v), a[0]);
                        out[i] = (v < 0.0) ? -mag : mag;
                    }
                    break;
                }

                // Unreachable — `block_walks` is the gate and this switch is its other half — and
                // kept correct rather than kept out, because the two lists disagreeing should cost
                // a slow answer and not a wrong one.
                default:
                    one_at_a_time(at, pts, count, out);
                    break;
            }

            s.back_to(mark);
        }
    };

    Scratch& s = scratch();
    const Scratch::Mark mark = s.here();
    Walk walk{*this, s};
    walk.go(root, points, count, out);
    s.back_to(mark);
}

}  // namespace forge
}  // namespace ws
