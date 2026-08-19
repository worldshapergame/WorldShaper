#include "forge/field.hpp"

#include <algorithm>
#include <cmath>

namespace ws {
namespace forge {

namespace {

constexpr f64 kPi = 3.14159265358979323846;
constexpr f64 kTau = 2.0 * kPi;

f64 axis_of(Vec3 p, u32 axis) {
    return (axis == 0) ? p.x : (axis == 1) ? p.y : p.z;
}

Vec3 with_axis(Vec3 p, u32 axis, f64 value) {
    if (axis == 0) p.x = value;
    else if (axis == 1) p.y = value;
    else p.z = value;
    return p;
}

// The two axes that are not this one, in ascending order, so a cylinder's cross-section, a
// prism's first face and a brick pattern's courses all agree about which way is "across".
//
// Ascending rather than cyclic, and the difference is not cosmetic. Cyclic order makes the
// first axis of a y-prism's cross-section *z*, so a hexagon with no turn points along z while
// the same hexagon about x or z points along y or x — three different conventions wearing one
// name. An author writing `prism sides=6 turn=0` means "a flat face toward the first axis", and
// with ascending order that is what they get, whichever way the prism stands.
void other_axes(u32 axis, u32& a, u32& b) {
    if (axis == 0) { a = 1; b = 2; }
    else if (axis == 1) { a = 0; b = 2; }
    else { a = 0; b = 1; }
}

f64 clamp(f64 v, f64 lo, f64 hi) { return (v < lo) ? lo : (v > hi) ? hi : v; }

// The polynomial smooth minimum. The blend is a real distance in metres, which is what makes it
// usable from a file: "join these two with a five centimetre fillet" is a thing an author knows
// they want, where a unitless k is a thing they have to discover by trying.
f64 smooth_min(f64 a, f64 b, f64 k) {
    if (k <= 0.0) return std::min(a, b);
    const f64 h = clamp(0.5 + 0.5 * (b - a) / k, 0.0, 1.0);
    return b * (1.0 - h) + a * h - k * h * (1.0 - h);
}

f64 smooth_max(f64 a, f64 b, f64 k) { return -smooth_min(-a, -b, k); }

// The FLAT version of the same two, which is what a chamfer is — see Op::ChamferUnion.
//
// The extra term is the forty-five degree plane through the seam: for two perpendicular faces
// `a` and `b`, `(a + b - k)/root2` is exactly the signed distance to the plane `a + b = k`, so a
// chamfer between two walls is not an approximation of a chamfer, it is one.
//
// # The clamp, which is not decoration
//
// Without it the field is not one-Lipschitz. Deep inside two overlapping shapes `a` and `b` are
// both about -t, the extra term is -root2*t - k/root2, and a field that falls by root2 metres per
// metre breaks the one promise `metric_slack` makes on this engine's behalf: that a reading at a
// block's centre bounds what the field can be anywhere in the block. Settling reads that promise,
// and a block wrongly settled is matter that is silently not there.
//
// So the term is floored (and, for the max, ceilinged) at one chamfer's worth away from the plain
// min. That bounds the whole op's deviation from `min(a, b)` — which IS one-Lipschitz — by
// k/root2 everywhere, which is an additive slack of root2*k and exactly the shape of allowance
// `metric_slack` is built to carry. It cannot move the SURFACE, because the clamp only bites
// where `min(a, b)` is already negative and the chamfer face lies where it is positive.
constexpr f64 kInvRoot2 = 0.7071067811865476;

f64 chamfer_min(f64 a, f64 b, f64 k) {
    if (k <= 0.0) return std::min(a, b);
    const f64 plain = std::min(a, b);
    return std::min(plain, std::max((a + b - k) * kInvRoot2, plain - k * kInvRoot2));
}

f64 chamfer_max(f64 a, f64 b, f64 k) { return -chamfer_min(-a, -b, k); }

// The point a stretched grain is really asked at — see the block above Op::Sine in field.hpp.
//
// A stored zero means one, so a node built before the stretch existed reads exactly as it did, and
// the all-ones case returns the point untouched rather than dividing by one three times. That is
// what makes the claim "a clip that never writes `stretch=` pays nothing for it" true rather than
// nearly true: six comparisons the branch predictor gets right every time, against three divides
// on the hottest path in the sampler.
Vec3 stretched(Vec3 p, f64 sx, f64 sy, f64 sz) {
    if ((sx == 0.0 || sx == 1.0) && (sy == 0.0 || sy == 1.0) && (sz == 0.0 || sz == 1.0)) {
        return p;
    }
    return {p.x / ((sx != 0.0) ? sx : 1.0), p.y / ((sy != 0.0) ? sy : 1.0),
            p.z / ((sz != 0.0) ? sz : 1.0)};
}

// How far a point is from a box, SQUARED, for the cull test — which compares against a distance it
// already has and so never needs the root. One square root per child per evaluation is not much; a
// hundred million evaluations with thirty children each is a hundred million square roots too many.
//
// The un-squared form stood beside this one until D638 moved the last caller across, and then sat
// here uncalled: MSVC does not diagnose an unused function in an anonymous namespace and GCC does,
// which is why it survived to be found by the first build off Windows.
f64 squared_distance_to(const Field::Aabb& box, Vec3 p) {
    if (box.infinite()) return 0.0;
    const f64 dx = std::max(std::max(box.low.x - p.x, p.x - box.high.x), 0.0);
    const f64 dy = std::max(std::max(box.low.y - p.y, p.y - box.high.y), 0.0);
    const f64 dz = std::max(std::max(box.low.z - p.z, p.z - box.high.z), 0.0);
    return dx * dx + dy * dy + dz * dz;
}

// --- deterministic value noise ----------------------------------------------------------
//
// Hash-based rather than table-based, so it needs no setup and no memory, and two machines
// agree exactly. The same clip file must build the same clip everywhere — a procedural clip
// whose grain differs between players is a clip that cannot be shared.
u32 hash_u32(u32 x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

f64 hash_to_unit(i64 xi, i64 yi, i64 zi, u32 seed) {
    u32 h = hash_u32(static_cast<u32>(xi) * 0x8da6b343u ^
                     static_cast<u32>(yi) * 0xd8163841u ^
                     static_cast<u32>(zi) * 0xcb1ab31fu ^ seed);
    return static_cast<f64>(h) * (1.0 / 4294967296.0);
}

f64 smootherstep(f64 t) { return t * t * t * (t * (t * 6.0 - 15.0) + 10.0); }

// Value noise in [-1, 1], with a feature size in metres.
f64 value_noise(Vec3 p, f64 size, u32 seed) {
    if (size <= 0.0) size = 1.0;
    const Vec3 q{p.x / size, p.y / size, p.z / size};
    const f64 fx = std::floor(q.x), fy = std::floor(q.y), fz = std::floor(q.z);
    const i64 xi = static_cast<i64>(fx), yi = static_cast<i64>(fy), zi = static_cast<i64>(fz);
    const f64 tx = smootherstep(q.x - fx);
    const f64 ty = smootherstep(q.y - fy);
    const f64 tz = smootherstep(q.z - fz);

    f64 accum = 0.0;
    for (i32 dz = 0; dz < 2; ++dz) {
        for (i32 dy = 0; dy < 2; ++dy) {
            for (i32 dx = 0; dx < 2; ++dx) {
                const f64 wx = dx ? tx : (1.0 - tx);
                const f64 wy = dy ? ty : (1.0 - ty);
                const f64 wz = dz ? tz : (1.0 - tz);
                accum += wx * wy * wz * hash_to_unit(xi + dx, yi + dy, zi + dz, seed);
            }
        }
    }
    return accum * 2.0 - 1.0;
}

f64 fbm_noise(Vec3 p, f64 size, u32 octaves, f64 gain, f64 lacunarity, u32 seed) {
    if (octaves == 0) return 0.0;
    f64 sum = 0.0;
    f64 amplitude = 1.0;
    f64 total = 0.0;
    f64 current = size;
    for (u32 i = 0; i < octaves; ++i) {
        sum += amplitude * value_noise(p, current, seed + i * 131u);
        total += amplitude;
        amplitude *= gain;
        current /= (lacunarity > 0.0) ? lacunarity : 2.0;
    }
    return (total > 0.0) ? sum / total : 0.0;
}

// Distance to the nearest of one scattered point per cell, and to the second nearest.
//
// The nearest alone gives grains, cobbles and crystals. The *difference* between the two gives
// the seams between them, and a seam is what a crack is: a thin branching network that meets
// itself at junctions and never simply stops. Drawing cracks as lines needs a line-drawing
// algorithm; getting them as the boundary between two nearest points needs only this.
void cell_noise(Vec3 p, f64 size, u32 seed, f64& nearest, f64& second) {
    if (size <= 0.0) size = 1.0;
    const Vec3 q{p.x / size, p.y / size, p.z / size};
    const f64 fx = std::floor(q.x), fy = std::floor(q.y), fz = std::floor(q.z);
    f64 best = 1e30;
    f64 next = 1e30;
    for (i32 dz = -1; dz <= 1; ++dz) {
        for (i32 dy = -1; dy <= 1; ++dy) {
            for (i32 dx = -1; dx <= 1; ++dx) {
                const i64 cx = static_cast<i64>(fx) + dx;
                const i64 cy = static_cast<i64>(fy) + dy;
                const i64 cz = static_cast<i64>(fz) + dz;
                const Vec3 jitter{hash_to_unit(cx, cy, cz, seed),
                                  hash_to_unit(cx, cy, cz, seed + 7919u),
                                  hash_to_unit(cx, cy, cz, seed + 104729u)};
                const Vec3 site{static_cast<f64>(cx) + jitter.x,
                                static_cast<f64>(cy) + jitter.y,
                                static_cast<f64>(cz) + jitter.z};
                const f64 d = length(q - site);
                if (d < best) {
                    next = best;
                    best = d;
                } else if (d < next) {
                    next = d;
                }
            }
        }
    }
    nearest = best * size;
    second = next * size;
}

// --- scatter: the numbers a tiled cell draws for its own copy ---------------------------------
//
// See Op::Scatter in field.hpp for what this is for. Three hashes of the cell's index, and the
// only thing worth being careful about is that they are hashes of the INDEX and of nothing else:
// a scatter whose numbers came from the point would shimmer as the sampler moved across a copy,
// and one whose numbers came from a counter would depend on the order the sampler happened to
// visit cells in.

// Which axis a scattered copy spins about: the first with no period, and y when all three repeat.
// A gravel bed repeats in x and z, so its pebbles turn about y; ivy on a wall repeats in x and y,
// so its leaves turn about z. Both are what the author meant and neither is written down.
u32 scatter_spin_axis(const f64* a) {
    for (u32 axis = 0; axis < 3; ++axis) {
        if (a[axis] <= 0.0) return axis;
    }
    return 1u;
}

// Where a scattered copy's cell asks its child, and by how much the answer must be multiplied on
// the way back out.
//
// The forward transform on the copy is scale, then spin, then shift, then tile; this is its
// inverse applied to the point in the opposite order. `scale` comes back as a multiplier because
// a UNIFORM scale of s reports s * d(p / s) exactly — no approximation and nothing to allow for.
Vec3 scatter_point(const f64* a, Vec3 p, const f64 cell[3], f64& scale) {
    const f64 jitter = clamp(a[6], 0.0, 1.0);
    const i64 cx = static_cast<i64>(cell[0]);
    const i64 cy = static_cast<i64>(cell[1]);
    const i64 cz = static_cast<i64>(cell[2]);

    Vec3 q = p;
    for (u32 axis = 0; axis < 3; ++axis) {
        const f64 period = a[axis];
        if (period <= 0.0) continue;   // no period, no cell, and so nothing to move it within
        const f64 shift = (jitter > 0.0)
                              ? period * jitter * (hash_to_unit(cx, cy, cz, 0x51ed270bu + axis) -
                                                   0.5)
                              : 0.0;
        q = with_axis(q, axis, axis_of(p, axis) - period * cell[axis] - shift);
    }

    const f64 turn = (a[7] != 0.0)
                         ? a[7] * (hash_to_unit(cx, cy, cz, 0x9e3779b9u) - 0.5) * 2.0
                         : 0.0;
    if (turn != 0.0) {
        const u32 spin = scatter_spin_axis(a);
        u32 u = 0, v = 0;
        other_axes(spin, u, v);
        const f64 angle = -turn * kTau;
        const f64 c = std::cos(angle), s = std::sin(angle);
        const f64 x = axis_of(q, u), y = axis_of(q, v);
        q = with_axis(q, u, x * c - y * s);
        q = with_axis(q, v, x * s + y * c);
    }

    // The same dial as the position: "how irregular", once, rather than three keys an author has
    // to balance against one another. Never larger than one, so a copy never outgrows the box its
    // own bounds were worked out from.
    scale = (jitter > 0.0) ? 1.0 - jitter * hash_to_unit(cx, cy, cz, 0x2545f491u) : 1.0;
    if (scale <= 1e-6) scale = 1e-6;
    if (scale != 1.0) q = {q.x / scale, q.y / scale, q.z / scale};
    return q;
}

// --- the exact distance functions ---------------------------------------------------------

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

// --- part of the way round ---------------------------------------------------------------------
//
// The three sweeps — `revolve`, `around` and `arc` — all carry a first turn and a width in turns,
// and all three measure the angle the way `atan2(second cross-axis, first cross-axis)` does. See
// the block above Op::Revolve in field.hpp for why that sense and not another.

// An author's `from` and `to` reduced to the pair the nodes actually store: where the sweep starts,
// and how wide it is.
//
// A width of exactly one means the whole turn, and every evaluator below tests for it before doing
// anything else — so a clip that never writes `from=` pays not one extra multiply. Ends that
// coincide are a whole turn rather than an empty shape, because `from=0.25 to=0.25` is a person
// saying "all the way round from a quarter" and not "nothing".
//
// The sweep always runs the increasing way, which is what makes `from=0.75 to=0.25` the half turn
// through the seam at zero instead of a hole where a shape was meant to be.
void sweep_range(f64 from, f64 to, f64& out_from, f64& out_span) {
    const f64 raw = to - from;
    // Not `>= 1.0`, so that a NaN — which compares false against everything — also lands here.
    if (!(std::abs(raw) < 1.0)) { out_from = 0.0; out_span = 1.0; return; }
    const f64 span = raw - std::floor(raw);
    if (!(span > 0.0)) { out_from = 0.0; out_span = 1.0; return; }
    out_from = from - std::floor(from);
    out_span = span;
}

// Whether a stored width is a real arc rather than the whole turn.
inline bool is_partial_sweep(f64 span) { return span > 0.0 && span < 1.0; }

// A turn folded into [0, 1). `std::floor` rather than `fmod` because fmod keeps the sign.
inline f64 wrap_turn(f64 t) {
    const f64 w = t - std::floor(t);
    return (w < 1.0) ? w : 0.0;   // a tiny negative t can round up to exactly 1.0
}

// Which end of an arc a point outside it is nearer to, as a signed offset in turns: positive when
// the point lies past `to`, negative when it lies short of `from`.
//
// The nearer end by ANGLE is the nearer end by DISTANCE, and that is what makes clamping exact
// rather than plausible. Two points sharing a radius and a height are |p−q|² = r₁² + r₂² −
// 2r₁r₂cos Δ + Δh² apart, which grows with |Δ| the whole way from nought to half a turn — so for
// every point of the swept shape, rotating it to the angularly nearest end brings it nearer.
inline f64 nearer_end(f64 rel, f64 span) {
    const f64 past_end = rel - span;      // beyond `to`, going the way the sweep runs
    const f64 before_start = 1.0 - rel;   // short of `from`, going the same way round the seam
    return (past_end <= before_start) ? past_end : -before_start;
}

// Which of a partial `around`'s copies a point belongs to, as the turn that copy stands at.
//
// n copies and n-1 gaps, first on `from` and last on `to` — see Op::PolarRepeat in field.hpp for
// why that and not n gaps. A point in the gap outside the arc is folded into whichever END copy
// is nearer, which does not invent matter: that copy really does stand there, and asking it about
// a point behind itself is the same question the whole-turn fold has always asked.
f64 polar_copy_turn(f64 turn, f64 from, f64 span, u32 count) {
    if (count <= 1) return from;   // one copy, and it sits on `from`
    const f64 step = span / static_cast<f64>(count - 1);
    const f64 rel = wrap_turn(turn - from);
    if (rel <= span) {
        const f64 k = clamp(std::round(rel / step), 0.0, static_cast<f64>(count - 1));
        return from + k * step;
    }
    return (nearer_end(rel, span) > 0.0) ? from + span : from;
}

f64 sd_cone(Vec3 p, f64 base_r, f64 height, u32 axis) {
    // Measured from the base, growing along the axis. Written as the intersection of the side
    // and the two caps, which is exact and needs no special case for a degenerate tip.
    u32 a = 0, b = 0;
    other_axes(axis, a, b);
    const f64 radial = std::hypot(axis_of(p, a), axis_of(p, b));
    const f64 along = axis_of(p, axis);
    const f64 slope = std::hypot(height, base_r);
    const f64 side = (radial * height + along * base_r - base_r * height) / (slope > 0.0 ? slope : 1.0);
    const f64 bottom = -along;
    const f64 top = along - height;
    return std::max(side, std::max(bottom, top));
}

f64 sd_ellipsoid(Vec3 p, Vec3 r) {
    // The standard bounded approximation. Exact distance to an ellipsoid has no closed form;
    // this is correct in sign everywhere and accurate near the surface, which is what a voxel
    // sampler and a displacement need.
    const Vec3 safe{r.x > 0 ? r.x : 1e-9, r.y > 0 ? r.y : 1e-9, r.z > 0 ? r.z : 1e-9};
    const f64 k0 = length({p.x / safe.x, p.y / safe.y, p.z / safe.z});
    const f64 k1 = length({p.x / (safe.x * safe.x), p.y / (safe.y * safe.y), p.z / (safe.z * safe.z)});
    if (k1 == 0.0) return -std::min(safe.x, std::min(safe.y, safe.z));
    return k0 * (k0 - 1.0) / k1;
}

// A regular n-gon prism, as the intersection of n half planes. Exact, and it gives every
// polygonal cross-section from a triangle upwards from one node.
f64 sd_prism(Vec3 p, f64 circumradius, f64 half_height, u32 sides, u32 axis, f64 turn) {
    if (sides < 3) sides = 3;
    u32 a = 0, b = 0;
    other_axes(axis, a, b);
    const f64 u = axis_of(p, a);
    const f64 v = axis_of(p, b);
    // The apothem: a circumradius is the distance to a corner, which is what an author means by
    // "a hexagon this big", so the faces sit closer in by cos(pi/n).
    const f64 apothem = circumradius * std::cos(kPi / static_cast<f64>(sides));
    f64 cross = -1e30;
    for (u32 i = 0; i < sides; ++i) {
        const f64 angle = kTau * (static_cast<f64>(i) / static_cast<f64>(sides) + turn);
        cross = std::max(cross, u * std::cos(angle) + v * std::sin(angle) - apothem);
    }
    const f64 along = std::abs(axis_of(p, axis)) - half_height;
    const f64 outside = std::hypot(std::max(cross, 0.0), std::max(along, 0.0));
    return std::min(std::max(cross, along), 0.0) + outside;
}

// The five Platonic solids, each as the intersection of its face planes.
//
// One mechanism for all five, because that is what they are: a set of unit normals and a
// distance. The normals below are the face normals of each solid, and the distance is scaled so
// the solid's circumradius — the distance to a *vertex*, which is the size an author means — is
// the number asked for.
//
// An intersection of half spaces is exact inside the solid and an under-estimate outside it,
// because near a corner the nearest face plane is further away than the corner itself. That is
// the safe direction — a distance that is too small never lets anything step through a surface —
// and it is invisible to the sampler, which only reads the sign. It is visible to `round` and
// `shell`, so those are slightly generous on the corners of a dodecahedron and exact on a cube,
// which is special-cased to a box for precisely that reason.
f64 sd_platonic(Vec3 p, f64 circumradius, u32 which) {
    const f64 phi = (1.0 + std::sqrt(5.0)) * 0.5;
    // Twenty, because that is how many faces an icosahedron has and it is the largest of the
    // five. Sized at sixteen first, which is the count for none of them and overran the stack
    // the moment an icosahedron was asked for.
    Vec3 normals[20];
    u32 count = 0;
    f64 face_over_circum = 1.0;   // inradius / circumradius, so the size means the vertex

    switch (which) {
        case 0: {   // tetrahedron
            const Vec3 n[4] = {{1, 1, 1}, {1, -1, -1}, {-1, 1, -1}, {-1, -1, 1}};
            for (u32 i = 0; i < 4; ++i) normals[count++] = normalise({-n[i].x, -n[i].y, -n[i].z});
            face_over_circum = 1.0 / 3.0;
            break;
        }
        case 1:   // cube — handled exactly below rather than as six planes
            return sd_box(p, {circumradius / std::sqrt(3.0), circumradius / std::sqrt(3.0),
                              circumradius / std::sqrt(3.0)});
        case 2: {   // octahedron
            for (i32 sx = -1; sx <= 1; sx += 2)
                for (i32 sy = -1; sy <= 1; sy += 2)
                    for (i32 sz = -1; sz <= 1; sz += 2)
                        normals[count++] = normalise({static_cast<f64>(sx), static_cast<f64>(sy),
                                                      static_cast<f64>(sz)});
            face_over_circum = 1.0 / std::sqrt(3.0);
            break;
        }
        case 3: {   // dodecahedron: twelve faces, normals along the icosahedron's vertices
            const Vec3 base[6] = {{0, 1, phi},  {0, 1, -phi}, {1, phi, 0},
                                  {1, -phi, 0}, {phi, 0, 1},  {-phi, 0, 1}};
            for (u32 i = 0; i < 6; ++i) {
                normals[count++] = normalise(base[i]);
                normals[count++] = normalise({-base[i].x, -base[i].y, -base[i].z});
            }
            face_over_circum = 0.7947;
            break;
        }
        default: {   // icosahedron: twenty faces, normals along the dodecahedron's vertices
            const Vec3 base[10] = {{1, 1, 1},     {1, 1, -1},    {1, -1, 1},    {1, -1, -1},
                                   {0, 1 / phi, phi}, {0, 1 / phi, -phi},
                                   {1 / phi, phi, 0}, {-1 / phi, phi, 0},
                                   {phi, 0, 1 / phi}, {phi, 0, -1 / phi}};
            for (u32 i = 0; i < 10; ++i) {
                normals[count++] = normalise(base[i]);
                normals[count++] = normalise({-base[i].x, -base[i].y, -base[i].z});
            }
            face_over_circum = 0.7947;
            break;
        }
    }

    const f64 inradius = circumradius * face_over_circum;
    f64 d = -1e30;
    for (u32 i = 0; i < count; ++i) d = std::max(d, dot(p, normals[i]) - inradius);
    return d;
}

// A ramp: a box cut by the diagonal plane that rises along one axis as another advances.
f64 sd_wedge(Vec3 p, Vec3 half, u32 rise_axis, u32 run_axis) {
    const f64 box = sd_box(p, half);
    const f64 rise = axis_of(half, rise_axis);
    const f64 run = axis_of(half, run_axis);
    if (rise <= 0.0 || run <= 0.0) return box;
    // The plane through (-run, -rise) and (+run, +rise), normalised so the result stays a
    // distance rather than merely having the right sign.
    const f64 u = axis_of(p, run_axis);
    const f64 v = axis_of(p, rise_axis);
    const f64 nx = rise, ny = -run;
    const f64 inv = 1.0 / std::hypot(nx, ny);
    return std::max(box, (u * nx + v * ny) * inv);
}

// A flight of steps, as the ramp it approximates minus the treads' overhang. Written by folding
// the run into one step, which makes it O(1) rather than one node per step and means a staircase
// of any length costs the same.
f64 sd_stairs(Vec3 p, Vec3 half, f64 run, f64 rise, u32 run_axis, u32 rise_axis) {
    if (run <= 0.0 || rise <= 0.0) return sd_box(p, half);
    const f64 u = axis_of(p, run_axis) + axis_of(half, run_axis);   // from the bottom of the flight
    const f64 v = axis_of(p, rise_axis) + axis_of(half, rise_axis);

    const f64 tread = std::floor(u / run);
    const f64 top = (tread + 1.0) * rise;
    // Inside the flight's box, and below the top of the step this point stands on.
    const f64 box = sd_box(p, half);
    return std::max(box, v - top);
}

// How many straight pieces a spiral of this many turns is cut into.
//
// Twenty-four to the turn, which puts the chord of a piece at about a fortieth of the radius, so
// the gap between the chord and the arc is under a thousandth of the radius — a tenth of a voxel
// on a volute the size of a dinner plate, at the finest resolution this engine samples. Capped,
// because the cost is linear in it and a spiral of forty turns is a decoration nobody is looking
// at closely.
u32 spiral_pieces(f64 turns) {
    const f64 wanted = std::abs(turns) * 24.0;
    if (wanted < 4.0) return 4u;
    if (wanted > 320.0) return 320u;
    return static_cast<u32>(wanted + 0.5);
}

// The distance to a logarithmic spiral swept as a round tube.
//
// Walked as a chain of straight pieces rather than solved: the spiral r = a·k^θ has no closed
// form for the nearest point, and every approximation of one either over-states the distance
// somewhere — which lets a sampler skip over the shape — or is slower than this. A chain of
// capsules has an exact distance, so what is returned is the true distance to the thing that
// actually gets built, and the field can be trusted to settle boxes near it.
//
// The pieces are stepped round by one complex multiply each rather than by a fresh cosine, so a
// spiral of a hundred pieces costs one pair of trigonometric calls and a hundred multiplies.
f64 sd_spiral(Vec3 p, f64 start_r, f64 per_turn, f64 tube, f64 turns, u32 axis) {
    if (start_r <= 0.0 || turns <= 0.0) return 1e30;
    u32 u = 0, v = 0;
    other_axes(axis, u, v);
    const f64 px = axis_of(p, u);
    const f64 py = axis_of(p, v);
    const f64 along = axis_of(p, axis);

    const u32 pieces = spiral_pieces(turns);
    const f64 step = kTau * turns / static_cast<f64>(pieces);
    const f64 c = std::cos(step), s = std::sin(step);
    // The radius is multiplied by `per_turn` over a whole turn, so by this much over one piece.
    const f64 grow = std::pow((per_turn > 0.0) ? per_turn : 1.0, turns / static_cast<f64>(pieces));

    f64 dirx = 1.0, diry = 0.0;
    f64 radius = start_r;
    f64 ax = radius, ay = 0.0;
    f64 best = 1e30;
    for (u32 i = 0; i < pieces; ++i) {
        const f64 ndirx = dirx * c - diry * s;
        const f64 ndiry = dirx * s + diry * c;
        radius *= grow;
        const f64 bx = ndirx * radius, by = ndiry * radius;

        // Point to segment, in the plane; the height off the plane is carried through as the
        // third component, which is what makes the sweep a tube rather than a ribbon.
        const f64 ex = bx - ax, ey = by - ay;
        const f64 wx = px - ax, wy = py - ay;
        const f64 denom = ex * ex + ey * ey;
        const f64 t = (denom > 0.0) ? clamp((wx * ex + wy * ey) / denom, 0.0, 1.0) : 0.0;
        const f64 dx = wx - ex * t, dy = wy - ey * t;
        const f64 d = std::sqrt(dx * dx + dy * dy + along * along);
        if (d < best) best = d;

        dirx = ndirx; diry = ndiry;
        ax = bx; ay = by;
    }
    return best - tube;
}

}  // namespace

f64 length(Vec3 v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }
f64 dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

Vec3 normalise(Vec3 v) {
    const f64 len = length(v);
    return (len > 0.0) ? Vec3{v.x / len, v.y / len, v.z / len} : Vec3{0.0, 1.0, 0.0};
}

u32 Field::push(const Node& n) {
    nodes_.push_back(n);
    return static_cast<u32>(nodes_.size() - 1);
}

// --- parameters ------------------------------------------------------------------------

u32 Field::parameter(const char* name, f64 initial) {
    const std::string key(name);
    for (usize i = 0; i < names_.size(); ++i) {
        if (names_[i] == key) {
            Node n;
            n.op = Op::Parameter;
            n.a[0] = static_cast<f64>(i);
            return push(n);
        }
    }
    parameters_.push_back(initial);
    names_.push_back(key);
    Node n;
    n.op = Op::Parameter;
    n.a[0] = static_cast<f64>(parameters_.size() - 1);
    return push(n);
}

bool Field::set_parameter(const char* name, f64 value) {
    const std::string key(name);
    for (usize i = 0; i < names_.size(); ++i) {
        if (names_[i] == key) {
            parameters_[i] = value;
            return true;
        }
    }
    return false;
}

f64 Field::get_parameter(const char* name, f64 fallback) const {
    const std::string key(name);
    for (usize i = 0; i < names_.size(); ++i) {
        if (names_[i] == key) return parameters_[i];
    }
    return fallback;
}

// --- builders ---------------------------------------------------------------------------

u32 Field::constant(f64 value) {
    Node n;
    n.op = Op::Constant;
    n.a[0] = value;
    return push(n);
}

u32 Field::coordinate(u32 axis) {
    Node n;
    n.op = Op::Coordinate;
    n.a[0] = static_cast<f64>(axis);
    return push(n);
}

u32 Field::radius(Vec3 centre) {
    Node n;
    n.op = Op::Radius;
    n.a[0] = centre.x; n.a[1] = centre.y; n.a[2] = centre.z;
    return push(n);
}

u32 Field::sphere(Vec3 centre, f64 r) {
    Node n;
    n.op = Op::Sphere;
    n.a[0] = centre.x; n.a[1] = centre.y; n.a[2] = centre.z; n.a[3] = r;
    return push(n);
}

u32 Field::box(Vec3 centre, Vec3 half, f64 corner) {
    Node n;
    n.op = Op::Box;
    n.a[0] = centre.x; n.a[1] = centre.y; n.a[2] = centre.z;
    n.a[3] = half.x;   n.a[4] = half.y;   n.a[5] = half.z;
    n.a[6] = corner;
    return push(n);
}

u32 Field::cylinder(Vec3 centre, f64 r, f64 half_height, u32 axis) {
    Node n;
    n.op = Op::Cylinder;
    n.a[0] = centre.x; n.a[1] = centre.y; n.a[2] = centre.z;
    n.a[3] = r; n.a[4] = half_height; n.a[5] = static_cast<f64>(axis);
    return push(n);
}

u32 Field::capsule(Vec3 a, Vec3 b, f64 r) {
    Node n;
    n.op = Op::Capsule;
    n.a[0] = a.x; n.a[1] = a.y; n.a[2] = a.z;
    n.a[3] = b.x; n.a[4] = b.y; n.a[5] = b.z;
    n.a[6] = r;
    return push(n);
}

u32 Field::torus(Vec3 centre, f64 ring, f64 tube, u32 axis) {
    Node n;
    n.op = Op::Torus;
    n.a[0] = centre.x; n.a[1] = centre.y; n.a[2] = centre.z;
    n.a[3] = ring; n.a[4] = tube; n.a[5] = static_cast<f64>(axis);
    return push(n);
}

u32 Field::arc(Vec3 centre, f64 ring, f64 tube, u32 axis, f64 from, f64 to) {
    Node n;
    n.op = Op::Arc;
    n.a[0] = centre.x; n.a[1] = centre.y; n.a[2] = centre.z;
    n.a[3] = ring; n.a[4] = tube; n.a[5] = static_cast<f64>(axis);
    sweep_range(from, to, n.a[6], n.a[7]);
    return push(n);
}

u32 Field::cone(Vec3 base_centre, f64 base_r, f64 height, u32 axis) {
    Node n;
    n.op = Op::Cone;
    n.a[0] = base_centre.x; n.a[1] = base_centre.y; n.a[2] = base_centre.z;
    n.a[3] = base_r; n.a[4] = height; n.a[5] = static_cast<f64>(axis);
    return push(n);
}

u32 Field::plane(Vec3 normal, f64 offset) {
    const Vec3 unit = normalise(normal);
    Node n;
    n.op = Op::Plane;
    n.a[0] = unit.x; n.a[1] = unit.y; n.a[2] = unit.z; n.a[3] = offset;
    return push(n);
}

u32 Field::ellipsoid(Vec3 centre, Vec3 radii) {
    Node n;
    n.op = Op::Ellipsoid;
    n.a[0] = centre.x; n.a[1] = centre.y; n.a[2] = centre.z;
    n.a[3] = radii.x;  n.a[4] = radii.y;  n.a[5] = radii.z;
    return push(n);
}

u32 Field::prism(Vec3 centre, f64 circumradius, f64 half_height, u32 sides, u32 axis, f64 turn) {
    Node n;
    n.op = Op::Prism;
    n.a[0] = centre.x; n.a[1] = centre.y; n.a[2] = centre.z;
    n.a[3] = circumradius; n.a[4] = half_height;
    n.a[5] = static_cast<f64>(sides); n.a[6] = static_cast<f64>(axis); n.a[7] = turn;
    return push(n);
}

u32 Field::platonic(Vec3 centre, f64 circumradius, u32 which) {
    Node n;
    n.op = Op::Platonic;
    n.a[0] = centre.x; n.a[1] = centre.y; n.a[2] = centre.z;
    n.a[3] = circumradius; n.a[4] = static_cast<f64>(which);
    return push(n);
}

u32 Field::wedge(Vec3 centre, Vec3 half, u32 rise_axis, u32 run_axis) {
    Node n;
    n.op = Op::Wedge;
    n.a[0] = centre.x; n.a[1] = centre.y; n.a[2] = centre.z;
    n.a[3] = half.x;   n.a[4] = half.y;   n.a[5] = half.z;
    n.a[6] = static_cast<f64>(rise_axis); n.a[7] = static_cast<f64>(run_axis);
    return push(n);
}

u32 Field::stairs(Vec3 centre, Vec3 half, f64 run, f64 rise) {
    Node n;
    n.op = Op::Stairs;
    n.a[0] = centre.x; n.a[1] = centre.y; n.a[2] = centre.z;
    n.a[3] = half.x;   n.a[4] = half.y;   n.a[5] = half.z;
    n.a[6] = run;      n.a[7] = rise;
    return push(n);
}

u32 Field::revolve(u32 profile, Vec3 centre, u32 axis, f64 from, f64 to) {
    Node n;
    n.op = Op::Revolve;
    n.child[0] = profile;
    n.children = 1;
    n.a[0] = centre.x; n.a[1] = centre.y; n.a[2] = centre.z;
    n.a[3] = static_cast<f64>(axis);
    sweep_range(from, to, n.a[4], n.a[5]);
    return push(n);
}

u32 Field::spiral(Vec3 centre, f64 radius, f64 tighten, f64 tube, f64 turns, u32 axis) {
    Node n;
    n.op = Op::Spiral;
    n.a[0] = centre.x; n.a[1] = centre.y; n.a[2] = centre.z;
    n.a[3] = radius; n.a[4] = tighten; n.a[5] = tube; n.a[6] = turns;
    n.a[7] = static_cast<f64>(axis);
    return push(n);
}

u32 Field::combine(Op op, const std::vector<u32>& parts, f64 blend) {
    if (parts.empty()) return constant(1e30);
    if (parts.size() == 1 && blend <= 0.0) return parts[0];
    // More than four parts fold into a chain, so an author can write `union { a b c d e f }`
    // without knowing that a node holds four children.
    u32 current = parts[0];
    usize i = 1;
    while (i < parts.size()) {
        Node n;
        n.op = op;
        n.a[0] = blend;
        n.child[0] = current;
        n.children = 1;
        while (i < parts.size() && n.children < 4) {
            n.child[n.children++] = parts[i++];
        }
        current = push(n);
    }
    return current;
}

u32 Field::unite(const std::vector<u32>& parts) { return combine(Op::Union, parts, 0.0); }
u32 Field::intersect(const std::vector<u32>& parts) { return combine(Op::Intersection, parts, 0.0); }
u32 Field::subtract(const std::vector<u32>& parts) { return combine(Op::Difference, parts, 0.0); }

u32 Field::smooth_unite(const std::vector<u32>& parts, f64 blend) {
    return combine(Op::SmoothUnion, parts, blend);
}
u32 Field::smooth_subtract(const std::vector<u32>& parts, f64 blend) {
    return combine(Op::SmoothDifference, parts, blend);
}
u32 Field::smooth_intersect(const std::vector<u32>& parts, f64 blend) {
    return combine(Op::SmoothIntersection, parts, blend);
}

u32 Field::chamfer_unite(const std::vector<u32>& parts, f64 width) {
    return combine(Op::ChamferUnion, parts, width);
}
u32 Field::chamfer_subtract(const std::vector<u32>& parts, f64 width) {
    return combine(Op::ChamferDifference, parts, width);
}
u32 Field::chamfer_intersect(const std::vector<u32>& parts, f64 width) {
    return combine(Op::ChamferIntersection, parts, width);
}

namespace {
Node unary(Op op, u32 child) {
    Node n;
    n.op = op;
    n.child[0] = child;
    n.children = 1;
    return n;
}
}  // namespace

u32 Field::translate(u32 child, Vec3 by) {
    Node n = unary(Op::Translate, child);
    n.a[0] = by.x; n.a[1] = by.y; n.a[2] = by.z;
    return push(n);
}

u32 Field::rotate(u32 child, Vec3 turns) {
    Node n = unary(Op::Rotate, child);
    n.a[0] = turns.x; n.a[1] = turns.y; n.a[2] = turns.z;
    return push(n);
}

u32 Field::scale(u32 child, Vec3 by) {
    Node n = unary(Op::Scale, child);
    n.a[0] = by.x; n.a[1] = by.y; n.a[2] = by.z;
    return push(n);
}

u32 Field::mirror(u32 child, u32 axis) {
    Node n = unary(Op::Mirror, child);
    n.a[0] = static_cast<f64>(axis);
    return push(n);
}

u32 Field::repeat(u32 child, Vec3 period, Vec3 limit) {
    Node n = unary(Op::Repeat, child);
    n.a[0] = period.x; n.a[1] = period.y; n.a[2] = period.z;
    n.a[3] = limit.x;  n.a[4] = limit.y;  n.a[5] = limit.z;
    return push(n);
}

u32 Field::scatter(u32 child, Vec3 period, Vec3 limit, f64 jitter, f64 turn) {
    // With nothing drawn per cell this IS a repeat, so it becomes one: an author sweeping `jitter`
    // and `turn` down to nought should land back on the shape they started from, in the same
    // number of nodes and at the same cost, rather than on a scatter that happens to draw zero.
    if (jitter <= 0.0 && turn == 0.0) return repeat(child, period, limit);

    Node n = unary(Op::Scatter, child);
    n.a[0] = period.x; n.a[1] = period.y; n.a[2] = period.z;
    n.a[3] = limit.x;  n.a[4] = limit.y;  n.a[5] = limit.z;
    n.a[6] = clamp(jitter, 0.0, 1.0);
    n.a[7] = turn;
    return push(n);
}

u32 Field::polar_repeat(u32 child, u32 count, u32 axis, f64 from, f64 to) {
    Node n = unary(Op::PolarRepeat, child);
    n.a[0] = static_cast<f64>(count);
    n.a[1] = static_cast<f64>(axis);
    sweep_range(from, to, n.a[2], n.a[3]);
    return push(n);
}

u32 Field::shell(u32 child, f64 thickness) {
    Node n = unary(Op::Shell, child);
    n.a[0] = thickness;
    return push(n);
}

u32 Field::round_off(u32 child, f64 r) {
    Node n = unary(Op::Round, child);
    n.a[0] = r;
    return push(n);
}

u32 Field::offset(u32 child, f64 by) {
    Node n = unary(Op::Offset, child);
    n.a[0] = by;
    return push(n);
}

u32 Field::displace(u32 child, u32 pattern, f64 amount) {
    // A displacement of nothing is the child, and saying so here removes the PATTERN from the tree.
    //
    // Op::Displace evaluates its pattern and multiplies by the amount, so an amount of zero still
    // pays for the noise in full at every sample and then throws it away. Not hypothetical: the
    // sub-voxel guard in clip_script.cpp sets exactly this amount to zero, so the facility's
    // `grain_fine` — several octaves of fbm — was being evaluated at every one of a hundred million
    // voxels to be multiplied by nought.
    if (amount == 0.0) return child;

    Node n;
    n.op = Op::Displace;
    n.child[0] = child;
    n.child[1] = pattern;
    n.children = 2;
    n.a[0] = amount;
    return push(n);
}

u32 Field::twist(u32 child, f64 turns_per_metre, u32 axis) {
    Node n = unary(Op::Twist, child);
    n.a[0] = turns_per_metre;
    n.a[1] = static_cast<f64>(axis);
    return push(n);
}

u32 Field::bend(u32 child, f64 turns_per_metre, u32 axis) {
    Node n = unary(Op::Bend, child);
    n.a[0] = turns_per_metre;
    n.a[1] = static_cast<f64>(axis);
    return push(n);
}

u32 Field::sine(u32 axis, f64 period, f64 phase) {
    Node n;
    n.op = Op::Sine;
    n.a[0] = static_cast<f64>(axis); n.a[1] = period; n.a[2] = phase;
    return push(n);
}

u32 Field::waves(u32 axis, f64 period_a, f64 period_b, f64 phase) {
    Node n;
    n.op = Op::Waves;
    n.a[0] = static_cast<f64>(axis); n.a[1] = period_a; n.a[2] = period_b; n.a[3] = phase;
    return push(n);
}

u32 Field::noise(f64 size, u32 seed, Vec3 stretch) {
    Node n;
    n.op = Op::Noise;
    n.a[0] = size; n.a[1] = static_cast<f64>(seed);
    n.a[2] = stretch.x; n.a[3] = stretch.y; n.a[4] = stretch.z;
    return push(n);
}

u32 Field::fbm(f64 size, u32 octaves, f64 gain, f64 lacunarity, u32 seed, Vec3 stretch) {
    Node n;
    n.op = Op::Fbm;
    n.a[0] = size; n.a[1] = static_cast<f64>(octaves); n.a[2] = gain;
    n.a[3] = lacunarity; n.a[4] = static_cast<f64>(seed);
    n.a[5] = stretch.x; n.a[6] = stretch.y; n.a[7] = stretch.z;
    return push(n);
}

u32 Field::ridged(f64 size, u32 octaves, f64 gain, f64 lacunarity, u32 seed, Vec3 stretch) {
    Node n;
    n.op = Op::Ridged;
    n.a[0] = size; n.a[1] = static_cast<f64>(octaves); n.a[2] = gain;
    n.a[3] = lacunarity; n.a[4] = static_cast<f64>(seed);
    n.a[5] = stretch.x; n.a[6] = stretch.y; n.a[7] = stretch.z;
    return push(n);
}

u32 Field::rasp(f64 size, f64 depth, u32 seed, Vec3 stretch) {
    Node n;
    n.op = Op::Rasp;
    n.a[0] = size; n.a[1] = depth; n.a[2] = static_cast<f64>(seed);
    n.a[3] = stretch.x; n.a[4] = stretch.y; n.a[5] = stretch.z;
    return push(n);
}

u32 Field::cells(f64 size, u32 seed, Vec3 stretch) {
    Node n;
    n.op = Op::Cells;
    n.a[0] = size; n.a[1] = static_cast<f64>(seed);
    n.a[2] = stretch.x; n.a[3] = stretch.y; n.a[4] = stretch.z;
    return push(n);
}

u32 Field::cell_edge(f64 size, u32 seed, Vec3 stretch) {
    Node n;
    n.op = Op::CellEdge;
    n.a[0] = size; n.a[1] = static_cast<f64>(seed);
    n.a[2] = stretch.x; n.a[3] = stretch.y; n.a[4] = stretch.z;
    return push(n);
}

u32 Field::curvature(u32 child, f64 radius) {
    Node n = unary(Op::Curvature, child);
    n.a[0] = radius;
    return push(n);
}

u32 Field::occlusion(u32 child, f64 radius) {
    Node n = unary(Op::Occlusion, child);
    n.a[0] = radius;
    return push(n);
}

u32 Field::facing(u32 child, u32 axis) {
    Node n = unary(Op::Facing, child);
    n.a[0] = static_cast<f64>(axis);
    return push(n);
}

u32 Field::checker(Vec3 cell) {
    Node n;
    n.op = Op::Checker;
    n.a[0] = cell.x; n.a[1] = cell.y; n.a[2] = cell.z;
    return push(n);
}

u32 Field::stripes(u32 axis, f64 period, f64 duty) {
    Node n;
    n.op = Op::Stripes;
    n.a[0] = static_cast<f64>(axis); n.a[1] = period; n.a[2] = duty;
    return push(n);
}

u32 Field::bricks(Vec3 brick, f64 mortar, u32 face_axis) {
    Node n;
    n.op = Op::Bricks;
    n.a[0] = brick.x; n.a[1] = brick.y; n.a[2] = brick.z;
    n.a[3] = mortar;  n.a[4] = static_cast<f64>(face_axis);
    return push(n);
}

u32 Field::add(const std::vector<u32>& parts) { return combine(Op::Add, parts, 0.0); }
u32 Field::multiply(const std::vector<u32>& parts) { return combine(Op::Multiply, parts, 0.0); }
u32 Field::minimum(const std::vector<u32>& parts) { return combine(Op::Min, parts, 0.0); }
u32 Field::maximum(const std::vector<u32>& parts) { return combine(Op::Max, parts, 0.0); }

u32 Field::blend(u32 a, u32 b, f64 t) {
    Node n;
    n.op = Op::Blend;
    n.child[0] = a; n.child[1] = b; n.children = 2;
    n.a[0] = t;
    return push(n);
}

u32 Field::remap(u32 child, f64 from_lo, f64 from_hi, f64 to_lo, f64 to_hi) {
    Node n = unary(Op::Remap, child);
    n.a[0] = from_lo; n.a[1] = from_hi; n.a[2] = to_lo; n.a[3] = to_hi;
    return push(n);
}

u32 Field::absolute(u32 child) { return push(unary(Op::Abs, child)); }
u32 Field::negate(u32 child) { return push(unary(Op::Negate, child)); }

u32 Field::step(u32 child, f64 edge) {
    Node n = unary(Op::Step, child);
    n.a[0] = edge;
    return push(n);
}

u32 Field::smoothstep(u32 child, f64 lo, f64 hi) {
    Node n = unary(Op::Smoothstep, child);
    n.a[0] = lo; n.a[1] = hi;
    return push(n);
}

u32 Field::clamp_to(u32 child, f64 lo, f64 hi) {
    Node n = unary(Op::Clamp, child);
    n.a[0] = lo; n.a[1] = hi;
    return push(n);
}

u32 Field::power(u32 child, f64 exponent) {
    Node n = unary(Op::Power, child);
    n.a[0] = exponent;
    return push(n);
}

// --- evaluation ---------------------------------------------------------------------------

f64 Field::eval(u32 at, Vec3 p) const {
    if (at >= nodes_.size()) return 1e30;
    const Node& n = nodes_[at];
    const f64* a = n.a;

    switch (n.op) {
        case Op::Constant: return a[0];
        case Op::Parameter: {
            const usize slot = static_cast<usize>(a[0]);
            return (slot < parameters_.size()) ? parameters_[slot] : 0.0;
        }
        case Op::Coordinate: return axis_of(p, static_cast<u32>(a[0]));
        case Op::Radius: return length(p - Vec3{a[0], a[1], a[2]});

        case Op::Sphere: return length(p - Vec3{a[0], a[1], a[2]}) - a[3];
        case Op::Box: {
            const f64 d = sd_box(p - Vec3{a[0], a[1], a[2]},
                                 {a[3] - a[6], a[4] - a[6], a[5] - a[6]});
            return d - a[6];
        }
        case Op::Cylinder:
            return sd_cylinder(p - Vec3{a[0], a[1], a[2]}, a[3], a[4], static_cast<u32>(a[5]));
        case Op::Capsule:
            return sd_capsule(p, {a[0], a[1], a[2]}, {a[3], a[4], a[5]}, a[6]);
        case Op::Torus:
            return sd_torus(p - Vec3{a[0], a[1], a[2]}, a[3], a[4], static_cast<u32>(a[5]));
        case Op::Cone:
            return sd_cone(p - Vec3{a[0], a[1], a[2]}, a[3], a[4], static_cast<u32>(a[5]));
        case Op::Plane: return dot(p, {a[0], a[1], a[2]}) - a[3];
        case Op::Ellipsoid:
            return sd_ellipsoid(p - Vec3{a[0], a[1], a[2]}, {a[3], a[4], a[5]});
        case Op::Prism:
            return sd_prism(p - Vec3{a[0], a[1], a[2]}, a[3], a[4], static_cast<u32>(a[5]),
                            static_cast<u32>(a[6]), a[7]);
        case Op::Platonic:
            return sd_platonic(p - Vec3{a[0], a[1], a[2]}, a[3], static_cast<u32>(a[4]));
        case Op::Wedge:
            return sd_wedge(p - Vec3{a[0], a[1], a[2]}, {a[3], a[4], a[5]},
                            static_cast<u32>(a[6]), static_cast<u32>(a[7]));
        case Op::Stairs:
            return sd_stairs(p - Vec3{a[0], a[1], a[2]}, {a[3], a[4], a[5]}, a[6], a[7],
                             /*run*/ 2u, /*rise*/ 1u);

        case Op::Revolve: {
            // The point folded into the profile's half plane: how far it is from the axis, how
            // far along the axis it is, and nothing else. That fold is exactly what revolving
            // means, and it is why the answer is a real distance and not a bound — the nearest
            // point of a surface of revolution is always at the asking point's own angle, so the
            // three-dimensional distance and the profile's own two-dimensional distance are the
            // same number.
            const u32 axis = static_cast<u32>(a[3]);
            u32 u = 0, v = 0;
            other_axes(axis, u, v);
            const Vec3 q = p - Vec3{a[0], a[1], a[2]};
            const f64 r = std::hypot(axis_of(q, u), axis_of(q, v));
            if (!is_partial_sweep(a[5])) {
                Vec3 flat{0, 0, 0};
                flat = with_axis(flat, u, r);
                flat = with_axis(flat, axis, axis_of(q, axis));
                return eval(n.child[0], flat);
            }

            // --- part of the way round ------------------------------------------------------
            //
            // The profile in its own plane, at any radius — including a negative one, which is
            // what the far side of an end cap asks for.
            const auto profile_at = [&](f64 radius) {
                Vec3 flat{0, 0, 0};
                flat = with_axis(flat, u, radius);
                flat = with_axis(flat, axis, axis_of(q, axis));
                return eval(n.child[0], flat);
            };

            // How far the point is from the end cap standing `delta` turns away from it. The cap
            // is the profile's own region, flat, at that angle; rotating the point into the cap's
            // plane splits it into a distance measured IN the plane and one perpendicular to it,
            // and the distance to a flat region is the hypotenuse of the two. Exact.
            const auto cap_away = [&](f64 delta) {
                const f64 turn = delta * kTau;
                const f64 across = r * std::cos(turn);    // toward the cap's own radius
                const f64 off = r * std::sin(turn);       // out of the cap's plane
                return std::hypot(std::max(profile_at(across), 0.0), off);
            };

            const f64 rel = wrap_turn(std::atan2(axis_of(q, v), axis_of(q, u)) / kTau - a[4]);
            if (rel > a[5]) {
                // Outside the wedge, and this is the whole trap. The honest distance here is to
                // the END CAP, not to the surface of the full revolution — return the latter and
                // every normal near the cut is wrong while a slice through the shape still looks
                // exactly right. D-note in field.hpp.
                return cap_away(nearer_end(rel, a[5]));
            }

            const f64 d = profile_at(r);
            // Outside the profile and inside the wedge: the nearest matter is at the point's own
            // angle, so the profile's own answer is already the true distance.
            if (d >= 0.0) return d;
            // Inside the solid the caps are surface too, and either may be nearer than the swept
            // face. Magnitude is what normals are made of, so this is not optional.
            return std::max(d, -std::min(cap_away(rel), cap_away(rel - a[5])));
        }
        case Op::Arc: {
            const Vec3 q = p - Vec3{a[0], a[1], a[2]};
            const u32 axis = static_cast<u32>(a[5]);
            if (!is_partial_sweep(a[7])) return sd_torus(q, a[3], a[4], axis);
            u32 u = 0, v = 0;
            other_axes(axis, u, v);
            const f64 x = axis_of(q, u), y = axis_of(q, v);
            const f64 rel = wrap_turn(std::atan2(y, x) / kTau - a[6]);
            // Within the arc the nearest point of the centre-line is at the asking point's own
            // angle, which is exactly what the torus already computes — so the same call answers
            // it and the two can never drift apart.
            if (rel <= a[7]) return sd_torus(q, a[3], a[4], axis);
            // Past an end, the nearest point of the centre-line is that end, and the cap is round
            // because the segment is a swept sphere. So: the distance to one point, less the tube.
            const f64 turn = (a[6] + ((nearer_end(rel, a[7]) > 0.0) ? a[7] : 0.0)) * kTau;
            const f64 ex = a[3] * std::cos(turn), ey = a[3] * std::sin(turn);
            return std::hypot(std::hypot(x - ex, y - ey), axis_of(q, axis)) - a[4];
        }
        case Op::Spiral:
            return sd_spiral(p - Vec3{a[0], a[1], a[2]}, a[3], a[4], a[5], a[6],
                             static_cast<u32>(a[7]));

        case Op::Union: {
            // A union wide enough to have earned a hierarchy is answered by it. See the comment
            // on Accelerator: what is being replaced is not the union's arithmetic but the
            // assumption that the way an author grouped a building is a useful way to search it.
            if (!accelerator_of_.empty() && accelerator_of_[at] != kNoAccelerator) {
                return eval_accelerated(accelerators_[accelerator_of_[at]], p);
            }

            // Nearest box first, and this is the difference between a cull that works and one
            // that is merely correct.
            //
            // The test below can only reject a child once the running answer is already small,
            // and the running answer starts at whatever the FIRST child happens to say. In
            // declaration order that is the first part the author wrote, which for a building is
            // the ground — so a point up in the dome evaluates the entire site before it has a
            // number small enough to reject anything with, and then rejects everything.
            //
            // Sorting by how far the point is from each child's box costs a handful of
            // subtractions per child and no recursion at all, and it means the first child
            // evaluated is the one most likely to give the smallest answer. Everything else is
            // then rejected on its box.
            //
            // Measured on the facility, which is 3474 nodes where the old one was 126: an
            // evaluation cost 3.4 microseconds against the old building's 312 nanoseconds, and
            // almost all of the difference was walking subtrees that the very next comparison
            // would have thrown away.
            // The distances are KEPT, and that is worth a third of a box test per node visited.
            //
            // They were worked out here to sort by, the array went out of scope, and the rejection
            // below asked for the very same number again — so a union of four children paid seven
            // box distances where four would do. Measured on the facility under callgrind:
            // `squared_distance_to` was called **1.93 times per node visited** and was **31% of
            // every instruction in the sample**, which is more than the shapes themselves. D638.
            u32 order[4] = {0, 1, 2, 3};
            f64 away[4] = {0.0, 0.0, 0.0, 0.0};
            const bool sorted = !bounds_.empty() && n.children > 1;
            if (sorted) {
                for (u32 i = 0; i < n.children; ++i) {
                    away[i] = squared_distance_to(bounds_[n.child[i]], p);
                }
                // Four children at most, so an insertion sort is the whole of it.
                for (u32 i = 1; i < n.children; ++i) {
                    const u32 key = order[i];
                    const f64 key_away = away[key];
                    u32 j = i;
                    while (j > 0 && away[order[j - 1]] > key_away) {
                        order[j] = order[j - 1];
                        --j;
                    }
                    order[j] = key;
                }
            }

            f64 d = eval(n.child[order[0]], p);
            for (u32 k = 1; k < n.children; ++k) {
                const u32 i = order[k];
                // If the point is already nearer to something than it could possibly be to
                // anything inside this child's box, the child cannot change the answer and does
                // not need evaluating. On a clip made of many separate parts this is nearly all
                // of them, at nearly every voxel.
                //
                // The condition to get right is *strictly outside the box*. A point inside a
                // child's box has a box distance of zero, and zero is not a lower bound on what
                // that child will say — a shape you are inside reports a negative distance. The
                // first version of this guarded on the running answer being positive instead,
                // which is sound but throws the optimisation away exactly where the work is:
                // inside a solid, which is where every voxel that becomes matter lives.
                //
                // Getting it wrong is not loud. The sign never changed, so nothing appeared or
                // vanished; the magnitude did, which moved the surface normals, which moved the
                // paint rule that follows them. Four hundred voxels of moss in the wrong place.
                // A child that under-reports may not be skipped on its box, however far away that
                // box is: it is free to answer less than the distance to it, so "the running
                // answer already beats your box" says nothing about what it would have said. D644.
                if (sorted) {
                    // Outside the box at all, and either already inside something (so nothing
                    // outside can be nearer) or nearer than the box can possibly be.
                    //
                    // And `break` rather than `continue`, which is the sort paying a second time.
                    // The children are in ascending box distance and `d` only ever shrinks, so a
                    // child rejected here is a proof that every child after it is rejected too:
                    // its box is at least as far, against a running answer that is at most as
                    // large. Reaching for the next one was asking a question whose answer was
                    // already known.
                    if (away[i] > 0.0 && (d < 0.0 || d * d <= away[i])) break;
                }
                d = std::min(d, eval(n.child[i], p));
            }
            return d;
        }
        case Op::Intersection: {
            // No cull here, and there cannot be one of this kind: an intersection takes the
            // largest answer, and a child the point is far outside is exactly the child most
            // likely to be it.
            f64 d = eval(n.child[0], p);
            for (u32 i = 1; i < n.children; ++i) d = std::max(d, eval(n.child[i], p));
            return d;
        }
        case Op::Difference: {
            f64 d = eval(n.child[0], p);
            for (u32 i = 1; i < n.children; ++i) {
                // Carving with something the point is nowhere near. Outside that child's box its
                // distance is at least `away`, so the term it contributes is at most −away, and
                // if the running answer already beats that the cut cannot reach here.
                if (!bounds_.empty()) {
                    const f64 away = squared_distance_to(bounds_[n.child[i]], p);
                    if (away > 0.0 && (d >= 0.0 || d * d <= away)) continue;
                }
                d = std::max(d, -eval(n.child[i], p));
            }
            return d;
        }
        case Op::SmoothUnion: {
            f64 d = eval(n.child[0], p);
            for (u32 i = 1; i < n.children; ++i) d = smooth_min(d, eval(n.child[i], p), a[0]);
            return d;
        }
        case Op::SmoothIntersection: {
            f64 d = eval(n.child[0], p);
            for (u32 i = 1; i < n.children; ++i) d = smooth_max(d, eval(n.child[i], p), a[0]);
            return d;
        }
        case Op::ChamferUnion: {
            f64 v = eval(n.child[0], p);
            for (u32 i = 1; i < n.children; ++i) v = chamfer_min(v, eval(n.child[i], p), a[0]);
            return v;
        }
        case Op::ChamferIntersection: {
            f64 v = eval(n.child[0], p);
            for (u32 i = 1; i < n.children; ++i) v = chamfer_max(v, eval(n.child[i], p), a[0]);
            return v;
        }
        case Op::ChamferDifference: {
            f64 v = eval(n.child[0], p);
            for (u32 i = 1; i < n.children; ++i) v = chamfer_max(v, -eval(n.child[i], p), a[0]);
            return v;
        }
        case Op::SmoothDifference: {
            f64 d = eval(n.child[0], p);
            for (u32 i = 1; i < n.children; ++i) d = smooth_max(d, -eval(n.child[i], p), a[0]);
            return d;
        }

        case Op::Translate: return eval(n.child[0], p - Vec3{a[0], a[1], a[2]});
        case Op::Rotate: {
            // Applied backwards, because moving the shape one way is asking about the point the
            // other. Euler xyz, in turns, because a quarter is a rounder thing to type than 90.
            const f64 cx = std::cos(-a[0] * kTau), sx = std::sin(-a[0] * kTau);
            const f64 cy = std::cos(-a[1] * kTau), sy = std::sin(-a[1] * kTau);
            const f64 cz = std::cos(-a[2] * kTau), sz = std::sin(-a[2] * kTau);
            Vec3 q = p;
            q = {q.x, q.y * cx - q.z * sx, q.y * sx + q.z * cx};
            q = {q.x * cy + q.z * sy, q.y, -q.x * sy + q.z * cy};
            q = {q.x * cz - q.y * sz, q.x * sz + q.y * cz, q.z};
            return eval(n.child[0], q);
        }
        case Op::Scale: {
            const Vec3 s{a[0] != 0.0 ? a[0] : 1.0, a[1] != 0.0 ? a[1] : 1.0,
                         a[2] != 0.0 ? a[2] : 1.0};
            const f64 smallest = std::min(std::abs(s.x), std::min(std::abs(s.y), std::abs(s.z)));
            // Scaled back by the smallest factor so the result never over-states the distance,
            // which would let a march step through the surface. Under-stating is always safe.
            return eval(n.child[0], {p.x / s.x, p.y / s.y, p.z / s.z}) * smallest;
        }
        case Op::Mirror: {
            const u32 axis = static_cast<u32>(a[0]);
            return eval(n.child[0], with_axis(p, axis, std::abs(axis_of(p, axis))));
        }
        case Op::Repeat: {
            // Folded into the nearest cell, and then checked against the neighbouring cell on the
            // side the point leans toward.
            //
            // The fold on its own answers "how far to the copy in *this* cell", which is not the
            // same question as "how far to the nearest copy" whenever a copy sits off-centre in
            // its cell. For a row of slats each hard against the left of its cell, a point just
            // past one slat is told the distance back to it rather than the shorter distance
            // forward to the next — an overstatement, and the dangerous direction, because a
            // sampler that believes there is nothing nearby skips over the slat that is.
            //
            // Checking the leaning neighbour makes it exact, and exact is worth far more than the
            // one extra evaluation costs: an honest distance can be trusted to settle whole boxes
            // at once, where a bound loose enough to be safe settles nothing. A copy lives inside
            // its own cell, so no cell beyond the immediate neighbour can hold anything nearer.
            //
            // A cell the limit has clamped away has no copy in it and must not be consulted:
            // taking the minimum against a copy that does not exist invents matter.
            Vec3 q = p;
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
                const f64 folded = value - period * cell;
                q = with_axis(q, axis, folded);

                f64 other = cell + ((folded >= 0.0) ? 1.0 : -1.0);
                if (limit > 0.0) other = clamp(other, -limit, limit);
                if (other != cell) {
                    axes[neighbours] = axis;
                    leaning[neighbours] = value - period * other;
                    ++neighbours;
                }
            }

            f64 best = eval(n.child[0], q);
            // Every combination of leaning neighbours, because with two axes repeating it is the
            // diagonal copy that can be nearest.
            for (u32 mask = 1; mask < (1u << neighbours); ++mask) {
                Vec3 shifted = q;
                for (u32 i = 0; i < neighbours; ++i) {
                    if ((mask >> i) & 1u) shifted = with_axis(shifted, axes[i], leaning[i]);
                }
                best = std::min(best, eval(n.child[0], shifted));
            }
            return best;
        }
        case Op::Scatter: {
            // `repeat`'s fold, with the cell INDEX carried instead of the folded point, because
            // the index is what the per-cell numbers are drawn from. The leaning-neighbour walk is
            // the same and is needed for the same reason — the copy in this cell is not
            // necessarily the nearest one, and with a jitter it is even less likely to be.
            f64 cell[3] = {0.0, 0.0, 0.0};
            f64 leaning[3];
            u32 axes[3];
            u32 neighbours = 0;
            for (u32 axis = 0; axis < 3; ++axis) {
                const f64 period = a[axis];
                if (period <= 0.0) continue;
                const f64 limit = a[3 + axis];
                const f64 value = axis_of(p, axis);
                f64 here = std::round(value / period);
                if (limit > 0.0) here = clamp(here, -limit, limit);
                cell[axis] = here;

                f64 other = here + ((value - period * here >= 0.0) ? 1.0 : -1.0);
                if (limit > 0.0) other = clamp(other, -limit, limit);
                if (other != here) {
                    axes[neighbours] = axis;
                    leaning[neighbours] = other;
                    ++neighbours;
                }
            }

            const auto ask = [&](const f64 which[3]) {
                f64 scale = 1.0;
                const Vec3 q = scatter_point(a, p, which, scale);
                return scale * eval(n.child[0], q);
            };

            f64 best = ask(cell);
            for (u32 mask = 1; mask < (1u << neighbours); ++mask) {
                f64 other[3] = {cell[0], cell[1], cell[2]};
                for (u32 i = 0; i < neighbours; ++i) {
                    if ((mask >> i) & 1u) other[axes[i]] = leaning[i];
                }
                best = std::min(best, ask(other));
            }
            return best;
        }
        case Op::PolarRepeat: {
            const u32 count = std::max(1u, static_cast<u32>(a[0]));
            const u32 axis = static_cast<u32>(a[1]);
            u32 u = 0, v = 0;
            other_axes(axis, u, v);
            const f64 x = axis_of(p, u), y = axis_of(p, v);
            f64 angle = std::atan2(y, x);
            if (!is_partial_sweep(a[3])) {
                const f64 sector = kTau / static_cast<f64>(count);
                angle -= sector * std::round(angle / sector);
            } else {
                angle -= kTau * polar_copy_turn(angle / kTau, a[2], a[3], count);
            }
            const f64 r = std::hypot(x, y);
            Vec3 q = p;
            q = with_axis(q, u, std::cos(angle) * r);
            q = with_axis(q, v, std::sin(angle) * r);
            return eval(n.child[0], q);
        }

        case Op::Shell: return std::abs(eval(n.child[0], p)) - a[0];
        case Op::Round: return eval(n.child[0], p) - a[0];
        case Op::Offset: return eval(n.child[0], p) + a[0];
        case Op::Displace: return eval(n.child[0], p) + a[0] * eval(n.child[1], p);
        case Op::Twist: {
            const u32 axis = static_cast<u32>(a[1]);
            u32 u = 0, v = 0;
            other_axes(axis, u, v);
            const f64 angle = -a[0] * kTau * axis_of(p, axis);
            const f64 c = std::cos(angle), s = std::sin(angle);
            const f64 x = axis_of(p, u), y = axis_of(p, v);
            Vec3 q = p;
            q = with_axis(q, u, x * c - y * s);
            q = with_axis(q, v, x * s + y * c);
            return eval(n.child[0], q);
        }
        case Op::Bend: {
            const u32 axis = static_cast<u32>(a[1]);
            u32 u = 0, v = 0;
            other_axes(axis, u, v);
            const f64 angle = -a[0] * kTau * axis_of(p, u);
            const f64 c = std::cos(angle), s = std::sin(angle);
            const f64 x = axis_of(p, u), y = axis_of(p, v);
            Vec3 q = p;
            q = with_axis(q, u, x * c - y * s);
            q = with_axis(q, v, x * s + y * c);
            return eval(n.child[0], q);
        }

        case Op::Sine: {
            const f64 period = (a[1] != 0.0) ? a[1] : 1.0;
            return std::sin(kTau * (axis_of(p, static_cast<u32>(a[0])) / period + a[2]));
        }
        case Op::Waves: {
            const u32 axis = static_cast<u32>(a[0]);
            u32 u = 0, v = 0;
            other_axes(axis, u, v);
            const f64 pa = (a[1] != 0.0) ? a[1] : 1.0;
            const f64 pb = (a[2] != 0.0) ? a[2] : 1.0;
            return std::sin(kTau * (axis_of(p, u) / pa + a[3])) *
                   std::sin(kTau * (axis_of(p, v) / pb + a[3]));
        }
        case Op::Noise:
            return value_noise(stretched(p, a[2], a[3], a[4]), a[0], static_cast<u32>(a[1]));
        case Op::Fbm:
            return fbm_noise(stretched(p, a[5], a[6], a[7]), a[0], static_cast<u32>(a[1]), a[2],
                             a[3], static_cast<u32>(a[4]));
        case Op::Ridged: {
            const f64 v = fbm_noise(stretched(p, a[5], a[6], a[7]), a[0], static_cast<u32>(a[1]),
                                    a[2], a[3], static_cast<u32>(a[4]));
            return 1.0 - 2.0 * std::abs(v);
        }
        case Op::Rasp: {
            // Ridges an order finer than the surface they sit on, which is what a filed or
            // scratched face looks like: many shallow parallel gouges rather than lumps. Stretched
            // they are what a file, a claw chisel or a saw actually leaves, which is parallel
            // gouges all running one way.
            const f64 v = fbm_noise(stretched(p, a[3], a[4], a[5]), a[0], 3u, 0.5, 2.7,
                                    static_cast<u32>(a[2]));
            return -std::abs(v) * a[1];
        }
        case Op::Cells: {
            f64 nearest = 0.0, second = 0.0;
            cell_noise(stretched(p, a[2], a[3], a[4]), a[0], static_cast<u32>(a[1]), nearest,
                       second);
            return nearest;
        }
        case Op::CellEdge: {
            f64 nearest = 0.0, second = 0.0;
            cell_noise(stretched(p, a[2], a[3], a[4]), a[0], static_cast<u32>(a[1]), nearest,
                       second);
            return second - nearest;   // zero on a seam, growing towards a cell's middle
        }

        case Op::Curvature: {
            // The mean of the field around a point against the field at it. For a distance
            // field that is its Laplacian, and the Laplacian of a distance field is curvature:
            // outside a convex shape the neighbourhood is further away than the centre, inside
            // a concave one it is nearer. Six samples, on the axes, which is enough to tell an
            // arris from a hollow and cheap enough to ask per voxel.
            const f64 r = (a[0] > 0.0) ? a[0] : 0.05;
            const f64 centre = eval(n.child[0], p);
            f64 sum = 0.0;
            sum += eval(n.child[0], {p.x + r, p.y, p.z});
            sum += eval(n.child[0], {p.x - r, p.y, p.z});
            sum += eval(n.child[0], {p.x, p.y + r, p.z});
            sum += eval(n.child[0], {p.x, p.y - r, p.z});
            sum += eval(n.child[0], {p.x, p.y, p.z + r});
            sum += eval(n.child[0], {p.x, p.y, p.z - r});
            return (sum / 6.0 - centre) / r;
        }
        case Op::Occlusion: {
            // How much of a small sphere around this point is solid. Fourteen fixed directions —
            // the six axes and the eight diagonals — rather than a random spray, because a
            // weathering pattern that shimmers when the clip is re-sampled is not a pattern.
            const f64 r = (a[0] > 0.0) ? a[0] : 0.15;
            const f64 k = 0.5773502691896258;   // one over root three, for the diagonals
            const Vec3 dirs[14] = {{1, 0, 0},  {-1, 0, 0}, {0, 1, 0},  {0, -1, 0}, {0, 0, 1},
                                   {0, 0, -1}, {k, k, k},  {k, k, -k}, {k, -k, k}, {k, -k, -k},
                                   {-k, k, k}, {-k, k, -k}, {-k, -k, k}, {-k, -k, -k}};
            u32 inside = 0;
            for (u32 i = 0; i < 14; ++i) {
                if (eval(n.child[0], p + dirs[i] * r) < 0.0) ++inside;
            }
            return static_cast<f64>(inside) / 14.0;
        }
        case Op::Facing: {
            const Vec3 normal = normal_at(n.child[0], p, (a[1] > 0.0) ? a[1] : 0.02);
            const u32 axis = static_cast<u32>(a[0]);
            return (axis == 0) ? normal.x : (axis == 1) ? normal.y : normal.z;
        }
        case Op::Checker: {
            f64 sum = 0.0;
            for (u32 axis = 0; axis < 3; ++axis) {
                const f64 cell = a[axis];
                if (cell <= 0.0) continue;
                sum += std::floor(axis_of(p, axis) / cell);
            }
            return (std::fmod(std::abs(sum), 2.0) < 1.0) ? -1.0 : 1.0;
        }
        case Op::Stripes: {
            const f64 period = (a[1] != 0.0) ? a[1] : 1.0;
            f64 t = axis_of(p, static_cast<u32>(a[0])) / period;
            t -= std::floor(t);
            return (t < a[2]) ? -1.0 : 1.0;
        }
        case Op::Bricks: {
            // Running bond: every other course offset by half a brick, and the value is how deep
            // into the mortar this point is — negative on a brick face, positive in a joint. So
            // it can carve the joints or colour them without any further work.
            const u32 face = static_cast<u32>(a[4]);
            u32 u = 0, v = 0;
            other_axes(face, u, v);
            const f64 course_height = (a[1] != 0.0) ? a[1] : 1.0;
            const f64 length_ = (a[0] != 0.0) ? a[0] : 1.0;
            const f64 course = std::floor(axis_of(p, v) / course_height);
            const f64 shift = (std::fmod(std::abs(course), 2.0) < 1.0) ? 0.0 : 0.5;
            f64 along = axis_of(p, u) / length_ + shift;
            along -= std::floor(along);
            f64 up = axis_of(p, v) / course_height;
            up -= std::floor(up);
            const f64 joint_u = a[3] / (2.0 * length_);
            const f64 joint_v = a[3] / (2.0 * course_height);
            // Distance into the brick from the nearest joint, on both axes.
            const f64 du = std::min(along, 1.0 - along) - joint_u;
            const f64 dv = std::min(up, 1.0 - up) - joint_v;
            return -std::min(du, dv);
        }

        case Op::Add: {
            f64 v = eval(n.child[0], p);
            for (u32 i = 1; i < n.children; ++i) v += eval(n.child[i], p);
            return v;
        }
        case Op::Multiply: {
            // Stops at the first factor that is nought.
            //
            // A product of patterns is how every mask in this language is written, and a mask
            // exists to be zero nearly everywhere. The factor that says so is usually cheap — a
            // smoothstep of a box, a coordinate — and the factors it is multiplying are usually
            // not: an occlusion or a curvature samples the field several times over, and a
            // weathering mask is built from both.
            //
            // Eagerly, a product costs the sum of its factors everywhere in the clip, including
            // everywhere the answer is nought. That is not a small constant: the facility's
            // weathering multiplied an occlusion of the whole building by a scope mask that was
            // zero across ninety-nine per cent of it, and sampling went from 2.4 seconds to 623.
            //
            // So write the cheap discriminating factor first, and this pays for it. It is exact —
            // zero times anything finite is zero — and the one thing it changes is that a factor
            // after a zero is not evaluated, which nothing can observe because nothing in this
            // language has side effects.
            f64 v = eval(n.child[0], p);
            for (u32 i = 1; i < n.children && v != 0.0; ++i) v *= eval(n.child[i], p);
            return v;
        }
        case Op::Min: {
            f64 v = eval(n.child[0], p);
            for (u32 i = 1; i < n.children; ++i) v = std::min(v, eval(n.child[i], p));
            return v;
        }
        case Op::Max: {
            f64 v = eval(n.child[0], p);
            for (u32 i = 1; i < n.children; ++i) v = std::max(v, eval(n.child[i], p));
            return v;
        }
        case Op::Blend: {
            const f64 t = clamp(a[0], 0.0, 1.0);
            return eval(n.child[0], p) * (1.0 - t) + eval(n.child[1], p) * t;
        }
        case Op::Remap: {
            const f64 v = eval(n.child[0], p);
            const f64 span = a[1] - a[0];
            const f64 t = (span != 0.0) ? clamp((v - a[0]) / span, 0.0, 1.0) : 0.0;
            return a[2] + (a[3] - a[2]) * t;
        }
        case Op::Abs: return std::abs(eval(n.child[0], p));
        case Op::Negate: return -eval(n.child[0], p);
        case Op::Step: return (eval(n.child[0], p) > a[0]) ? 1.0 : 0.0;
        case Op::Smoothstep: {
            const f64 span = a[1] - a[0];
            if (span == 0.0) return (eval(n.child[0], p) > a[0]) ? 1.0 : 0.0;
            const f64 t = clamp((eval(n.child[0], p) - a[0]) / span, 0.0, 1.0);
            return t * t * (3.0 - 2.0 * t);
        }
        case Op::Clamp: return clamp(eval(n.child[0], p), a[0], a[1]);
        case Op::Power: {
            const f64 v = eval(n.child[0], p);
            const f64 m = std::pow(std::abs(v), a[0]);
            return (v < 0.0) ? -m : m;
        }
    }
    return 1e30;
}

namespace {

Field::Aabb everywhere() { return Field::Aabb{}; }

// Whether a node's answer can be trusted as at least the distance to its own box.
//
// **This is the property every box cull rests on and four primitives do not have.** The union's
// rejection skips a child when the running answer is already nearer than the child's box; that is
// only sound if the child, asked at a point outside its box, answers at least the distance to it.
// An exact signed distance does, because the shape is inside the box. A BOUNDED one need not:
// `sd_ellipsoid` is "the standard bounded approximation" by its own comment, and a cone, a prism
// and a platonic solid are built as intersections of half planes, which under-state the distance
// out past a corner by up to a factor of root three.
//
// Measured rather than reasoned: sampling each primitive outside its own box, the worst ratio of
// answer to box distance is 1.0000 for the sphere, box, cylinder, capsule, torus, wedge, stairs
// and spiral, and **0.5877 for the ellipsoid, 0.5300 for the cone, 0.8660 for the prism and 0.5774
// for the platonic**. Every composite is sound (smooth union 1.0412, union, difference, round,
// translate, repeat and uniform scale all 1.0000). D644.
//
// Under-stating a distance is otherwise SAFE and is the direction this whole engine prefers -- a
// march that steps short is slow, one that steps long goes through the wall. The cull is the one
// place where the other direction is assumed, and `build_bounds` already refuses a box to a
// non-uniform scale for exactly this reason, in a comment that describes this bug precisely
// without knowing four primitives had it too.
}  // namespace

bool op_reports_true_distance(Op op) {
    switch (op) {
        case Op::Ellipsoid: case Op::Cone: case Op::Prism: case Op::Platonic:
            return false;
        // **An INTERSECTION belongs on this list and is deliberately NOT on it, because the cure
        // measured worse than the disease.** Its shape sits in the OVERLAP of its children's boxes
        // and `bounds_` says so rightly, but its answer is `max` over the children and is only
        // ever as large as one child's own distance -- and a child's box is bigger than the
        // overlap. `cull_bounds_` below gives the node a box its answer CAN vouch for, which fixes
        // a parent culling this node. What that cannot fix is a grandparent culling the union
        // above it, because the union's box is built from the shape boxes.
        //
        // Refusing the cull outright is sound and unusable: the facility's solid holds 217
        // intersections, refusing them takes the whole tree with them through the honesty AND, and
        // one sample of the facility at metre 8 went from **12.45 s to over nine minutes** before
        // it was killed. The right fix is to propagate a second set of boxes through every op --
        // the same switch, reading children's cull boxes instead of their shape boxes -- and it is
        // a bigger change than this one. Until then this is a known and bounded hole: 58 points in
        // 64,000 over the facility, worst 0.139 m, always in the safe-for-marching direction for
        // the shapes themselves and in the dangerous one for a union above an intersection. D644.
        default:
            return true;
    }
}

namespace {

Field::Aabb around(Vec3 centre, Vec3 half) {
    Field::Aabb box;
    box.low = {centre.x - half.x, centre.y - half.y, centre.z - half.z};
    box.high = {centre.x + half.x, centre.y + half.y, centre.z + half.z};
    return box;
}

Field::Aabb grown(Field::Aabb box, f64 by) {
    if (box.infinite()) return box;
    box.low = {box.low.x - by, box.low.y - by, box.low.z - by};
    box.high = {box.high.x + by, box.high.y + by, box.high.z + by};
    return box;
}

Field::Aabb merged(Field::Aabb a, Field::Aabb b) {
    if (a.infinite() || b.infinite()) return everywhere();
    Field::Aabb box;
    box.low = {std::min(a.low.x, b.low.x), std::min(a.low.y, b.low.y),
               std::min(a.low.z, b.low.z)};
    box.high = {std::max(a.high.x, b.high.x), std::max(a.high.y, b.high.y),
                std::max(a.high.z, b.high.z)};
    return box;
}

Field::Aabb overlapped(Field::Aabb a, Field::Aabb b) {
    if (a.infinite()) return b;
    if (b.infinite()) return a;
    Field::Aabb box;
    box.low = {std::max(a.low.x, b.low.x), std::max(a.low.y, b.low.y), std::max(a.low.z, b.low.z)};
    box.high = {std::min(a.high.x, b.high.x), std::min(a.high.y, b.high.y),
                std::min(a.high.z, b.high.z)};
    return box;
}

// The room ONE scattered copy needs, before it is tiled: the child's own box widened for
// everything its cell may draw at it.
//
// Written once because two readers want it and they want it for opposite reasons. `build_bounds`
// asks so it can put a box round the whole bed; `metric_slack` asks so it can decide whether a
// copy still fits inside its own cell, which is what makes a gravel bed settleable. Derived twice,
// those two drift, and the way they drift is silent: a footprint that is right for the box and
// wrong for the fit test promises the sampler an honest distance it is not getting.
//
// **The shrink is the one that looks harmless and is not.** A copy scaled about its own origin
// moves TOWARD that origin, so a child modelled at arm's length — a pebble at x = 0.06, say —
// sweeps out MORE room as it shrinks, not less: [0.06, 0.08] scaled by a half is [0.03, 0.04], and
// the pair of them span [0.03, 0.08], which is wider than either. Taking the child's box as the
// footprint was wrong in exactly that case and only in that case, which is why it is spelled out.
Field::Aabb scatter_footprint(const f64* a, Field::Aabb child) {
    if (child.infinite()) return child;
    const f64 jitter = clamp(a[6], 0.0, 1.0);

    // Every size a cell can draw, from the smallest to one, is covered by the union of the box and
    // the box at the smallest.
    const f64 smallest = std::max(1.0 - jitter, 1e-6);
    child.low = {std::min(child.low.x, child.low.x * smallest),
                 std::min(child.low.y, child.low.y * smallest),
                 std::min(child.low.z, child.low.z * smallest)};
    child.high = {std::max(child.high.x, child.high.x * smallest),
                  std::max(child.high.y, child.high.y * smallest),
                  std::max(child.high.z, child.high.z * smallest)};

    // Spun through any angle it may be spun through, which is the circle its own corners reach.
    if (a[7] != 0.0) {
        const u32 spin = scatter_spin_axis(a);
        u32 u = 0, v = 0;
        other_axes(spin, u, v);
        const f64 reach =
            std::hypot(std::max(std::abs(axis_of(child.low, u)), std::abs(axis_of(child.high, u))),
                       std::max(std::abs(axis_of(child.low, v)), std::abs(axis_of(child.high, v))));
        child.low = with_axis(with_axis(child.low, u, -reach), v, -reach);
        child.high = with_axis(with_axis(child.high, u, reach), v, reach);
    }
    return child;
}

}  // namespace

Field::Aabb Field::bounds_of(u32 node) const {
    if (node < bounds_.size()) return bounds_[node];
    return everywhere();
}

namespace {

// The walk behind `containing_bounds_of`. See the declaration for what separates it from
// `bounds_of` and why the separation is the whole point.
//
// `budget` is a node count and not a depth, because the thing being walked is a DAG: a shared `let`
// binding is one node with many parents, and a recursion with no memo can visit it once per path.
// Falling straight through wherever `bounds_of` already has an answer keeps that from mattering in
// practice — only the spine of unbounded nodes is descended, and on the estate that is 15% of the
// field — but a budget costs one decrement and makes "in practice" not have to be true.
Field::Aabb containing_walk(const Field& f, u32 at, u32& budget) {
    const Field::Aabb known = f.bounds_of(at);
    if (!known.infinite()) return known;
    if (budget == 0) return known;
    --budget;

    const Node& n = f.node(at);
    const f64* a = n.a;
    switch (n.op) {
        // A scale puts the shape where the child's box goes when its corners are scaled. That is
        // true whether or not the factors match; what does not survive an uneven scale is the
        // DISTANCE the node reports, and nothing here reads one.
        case Op::Scale: {
            if (n.children < 1) break;
            const Field::Aabb child = containing_walk(f, n.child[0], budget);
            if (child.infinite()) break;
            const Vec3 s{a[0] != 0.0 ? a[0] : 1.0, a[1] != 0.0 ? a[1] : 1.0,
                         a[2] != 0.0 ? a[2] : 1.0};
            const Vec3 one{child.low.x * s.x, child.low.y * s.y, child.low.z * s.z};
            const Vec3 two{child.high.x * s.x, child.high.y * s.y, child.high.z * s.z};
            return Field::Aabb{
                {std::min(one.x, two.x), std::min(one.y, two.y), std::min(one.z, two.z)},
                {std::max(one.x, two.x), std::max(one.y, two.y), std::max(one.z, two.z)}};
        }

        // And the structure above it, so that one unbounded leaf does not take the box off the
        // assembly it was carried into. Every one of these is the same rule `build_bounds` uses;
        // the only difference is which child boxes they are asked to combine.
        //
        // **Every case here is growth-free, and that is a condition rather than a coincidence.**
        // The ops `build_bounds` widens by a distance — `shell`, `round`, `offset`, a smooth or
        // chamfered union — are all sound only if the child reports an honest distance, and the
        // whole reason this walk exists is that it descends into shapes that do not. An uneven
        // scale UNDER-states distance, so `{ d <= t }` reaches further than the scaled box grown by
        // `t`, and a box grown that way would be smaller than the shape it claims to hold. Those
        // ops are left to fall through to the infinite box they already had. What is here is
        // exact set arithmetic and assumes nothing about what the children report.
        case Op::Translate: {
            if (n.children < 1) break;
            Field::Aabb child = containing_walk(f, n.child[0], budget);
            if (child.infinite()) break;
            const Vec3 by{a[0], a[1], a[2]};
            child.low = child.low + by;
            child.high = child.high + by;
            return child;
        }
        // `min(a, b) <= 0` exactly where either child is, and `max(a, b) <= 0` exactly where both
        // are. Two set identities, true of any two functions whatever either of them reports.
        case Op::Union:
        case Op::Min: {
            if (n.children < 1) break;
            Field::Aabb box = containing_walk(f, n.child[0], budget);
            for (u32 c = 1; c < n.children; ++c) {
                box = merged(box, containing_walk(f, n.child[c], budget));
            }
            if (box.infinite()) break;
            return box;
        }
        case Op::Intersection:
        case Op::Max: {
            if (n.children < 1) break;
            Field::Aabb box = containing_walk(f, n.child[0], budget);
            for (u32 c = 1; c < n.children; ++c) {
                box = overlapped(box, containing_walk(f, n.child[c], budget));
            }
            if (box.infinite()) break;
            return box;
        }
        // Carving only removes, however it is carved, so what is left is inside what it started as.
        case Op::Difference:
        case Op::SmoothDifference:
        case Op::ChamferDifference: {
            if (n.children < 1) break;
            const Field::Aabb box = containing_walk(f, n.child[0], budget);
            if (box.infinite()) break;
            return box;
        }
        // A rotation puts the shape inside the box its child's corners land in, and a mirror puts
        // it inside the child's box and the child's box reflected. Both are where the shape is and
        // neither is a distance.
        case Op::Rotate: {
            if (n.children < 1) break;
            const Field::Aabb child = containing_walk(f, n.child[0], budget);
            if (child.infinite()) break;
            const f64 cx = std::cos(a[0] * kTau), sx = std::sin(a[0] * kTau);
            const f64 cy = std::cos(a[1] * kTau), sy = std::sin(a[1] * kTau);
            const f64 cz = std::cos(a[2] * kTau), sz = std::sin(a[2] * kTau);
            Vec3 lo{1e30, 1e30, 1e30};
            Vec3 hi{-1e30, -1e30, -1e30};
            for (u32 corner = 0; corner < 8; ++corner) {
                Vec3 q{(corner & 1u) ? child.high.x : child.low.x,
                       (corner & 2u) ? child.high.y : child.low.y,
                       (corner & 4u) ? child.high.z : child.low.z};
                q = {q.x * cz - q.y * sz, q.x * sz + q.y * cz, q.z};
                q = {q.x * cy + q.z * sy, q.y, -q.x * sy + q.z * cy};
                q = {q.x, q.y * cx - q.z * sx, q.y * sx + q.z * cx};
                lo = {std::min(lo.x, q.x), std::min(lo.y, q.y), std::min(lo.z, q.z)};
                hi = {std::max(hi.x, q.x), std::max(hi.y, q.y), std::max(hi.z, q.z)};
            }
            return Field::Aabb{lo, hi};
        }
        case Op::Mirror: {
            if (n.children < 1) break;
            Field::Aabb child = containing_walk(f, n.child[0], budget);
            if (child.infinite()) break;
            const u32 axis = static_cast<u32>(a[0]);
            const f64 reach = std::max(std::abs(axis_of(child.low, axis)),
                                       std::abs(axis_of(child.high, axis)));
            child.low = with_axis(child.low, axis, -reach);
            child.high = with_axis(child.high, axis, reach);
            return child;
        }
        default: break;
    }
    return known;
}

}  // namespace

Field::Aabb Field::containing_bounds_of(u32 node) const {
    if (node >= nodes_.size()) return everywhere();
    // Enough to walk the whole field once over, so the only thing the budget can stop is a DAG
    // being re-walked, never an honest expression being cut short.
    u32 budget = static_cast<u32>(nodes_.size()) + 64u;
    return containing_walk(*this, node, budget);
}

// Everything a plain union finally reaches. A nested union is walked through; anything else is a
// leaf, however large — a carved wall is one leaf, and the right one, because its box is tight
// around the wall rather than around the layer the wall was filed in.
//
// SmoothUnion is deliberately not walked through. Its answer depends on every child at once, so
// it cannot be reordered or skipped, and it is a leaf like any other shape.
void Field::flatten_union(u32 at, std::vector<u32>& leaves) const {
    if (at >= nodes_.size()) return;
    const Node& n = nodes_[at];
    if (n.op != Op::Union) {
        leaves.push_back(at);
        return;
    }
    for (u32 i = 0; i < n.children; ++i) flatten_union(n.child[i], leaves);
}

// Split on the longest axis of the range's own bounds, at the median. Not the best heuristic
// there is, and the best heuristic is not what this needed: the win is going from "every layer
// that overlaps this point" to "the few things actually near it", and a median split gets that.
u32 Field::build_bvh(Accelerator& bvh, std::vector<u32>& work, usize begin, usize end) const {
    const u32 self = static_cast<u32>(bvh.nodes.size());
    bvh.nodes.push_back(BvhNode{});

    Aabb box = bounds_[work[begin]];
    for (usize i = begin + 1; i < end; ++i) box = merged(box, bounds_[work[i]]);

    const usize count = end - begin;
    // A handful is cheaper to test one by one than to descend into, and an infinite box cannot be
    // split usefully — everything in it would sort the same way.
    if (count <= 4 || box.infinite()) {
        bvh.nodes[self].box = box;
        bvh.nodes[self].left = static_cast<u32>(bvh.order.size());
        bvh.nodes[self].count = static_cast<u32>(count);
        for (usize i = begin; i < end; ++i) bvh.order.push_back(work[i]);
        return self;
    }

    const f64 span[3] = {box.high.x - box.low.x, box.high.y - box.low.y, box.high.z - box.low.z};
    const u32 axis = (span[0] > span[1] && span[0] > span[2]) ? 0u : ((span[1] > span[2]) ? 1u : 2u);
    const usize middle = begin + count / 2;
    std::nth_element(work.begin() + static_cast<isize>(begin),
                     work.begin() + static_cast<isize>(middle),
                     work.begin() + static_cast<isize>(end), [&](u32 a, u32 b) {
                         const Aabb& ba = bounds_[a];
                         const Aabb& bb = bounds_[b];
                         return axis_of(ba.low, axis) + axis_of(ba.high, axis) <
                                axis_of(bb.low, axis) + axis_of(bb.high, axis);
                     });

    const u32 left = build_bvh(bvh, work, begin, middle);
    const u32 right = build_bvh(bvh, work, middle, end);
    bvh.nodes[self].box = box;
    bvh.nodes[self].left = left;
    bvh.nodes[self].right = right;
    bvh.nodes[self].count = 0;
    return self;
}

// The minimum over everything in the hierarchy, visiting the nearer box first so the running
// answer gets small early and the rest is thrown away on its box.
//
// The rejection test is the one the union already used, and it has to be exactly as careful: a
// point INSIDE a box has a box distance of zero, and zero is not a lower bound on what that
// subtree will say, because a shape you are inside reports a negative distance.
f64 Field::eval_accelerated(const Accelerator& bvh, Vec3 p) const {
    f64 d = 1e30;
    // The distance travels WITH the node, for the reason the union path above now does the same:
    // a child's box was already measured to decide which of the two to visit first, and measuring
    // it again on the way out is the same arithmetic twice. It cannot have changed — a box and a
    // point are both fixed for the whole of this call — so this is a saving and not a shortcut.
    // D638.
    struct Pending {
        u32 node;
        f64 away;
    };
    Pending stack[64];
    u32 top = 0;
    stack[top++] = Pending{0, squared_distance_to(bvh.nodes[0].box, p)};
    while (top > 0) {
        const Pending taken = stack[--top];
        const BvhNode& node = bvh.nodes[taken.node];
        const f64 away = taken.away;
        if (away > 0.0 && (d < 0.0 || d * d <= away)) continue;

        if (node.count > 0) {
            for (u32 i = 0; i < node.count; ++i) {
                const u32 leaf = bvh.order[node.left + i];
                const f64 leaf_away = squared_distance_to(bounds_[leaf], p);
                if (leaf_away > 0.0 && (d < 0.0 || d * d <= leaf_away)) continue;
                d = std::min(d, eval(leaf, p));
            }
            continue;
        }

        // The farther child goes on the stack first, so the nearer one is popped and evaluated
        // first and gives the other something to be rejected against.
        const f64 la = squared_distance_to(bvh.nodes[node.left].box, p);
        const f64 ra = squared_distance_to(bvh.nodes[node.right].box, p);
        if (top + 2 <= 64) {
            if (la <= ra) {
                stack[top++] = Pending{node.right, ra};
                stack[top++] = Pending{node.left, la};
            } else {
                stack[top++] = Pending{node.left, la};
                stack[top++] = Pending{node.right, ra};
            }
        }
    }
    return d;
}

void Field::build_bounds() {
    bounds_.assign(nodes_.size(), everywhere());
    // Children always have a lower index than their parent — every builder pushes after its
    // children — so one forward pass suffices and no recursion is needed.
    for (usize i = 0; i < nodes_.size(); ++i) {
        const Node& n = nodes_[i];
        const f64* a = n.a;
        Aabb box = everywhere();
        switch (n.op) {
            case Op::Sphere: box = around({a[0], a[1], a[2]}, {a[3], a[3], a[3]}); break;
            case Op::Box: box = around({a[0], a[1], a[2]}, {a[3], a[4], a[5]}); break;
            case Op::Ellipsoid: box = around({a[0], a[1], a[2]}, {a[3], a[4], a[5]}); break;
            case Op::Platonic:
                box = around({a[0], a[1], a[2]}, {a[3], a[3], a[3]});
                break;
            case Op::Cylinder:
            case Op::Prism: {
                const u32 axis = static_cast<u32>((n.op == Op::Cylinder) ? a[5] : a[6]);
                const f64 across = a[3];
                const f64 along = (n.op == Op::Cylinder) ? a[4] : a[4];
                Vec3 half{across, across, across};
                if (axis == 0) half.x = along;
                else if (axis == 1) half.y = along;
                else half.z = along;
                box = around({a[0], a[1], a[2]}, half);
                break;
            }
            case Op::Torus:
            case Op::Arc: {
                // **The WHOLE ring's box, even for a segment of one, and that is deliberate.**
                //
                // A segment's true extent is tighter — a quarter of a ring occupies a quarter of
                // the box — and a tighter box would cull better. It would also be a box worked out
                // from an arc's endpoints and its extreme angles, which is four cases and a seam,
                // and a box that is tighter than the truth by any amount at all is a piece of the
                // clip quietly missing. Conservative is always correct here: the segment is inside
                // the full ring, and the segment's answer is at least the full torus's answer, so
                // a point outside this box is told at least the distance to it and the cull that
                // reads it stays sound.
                const u32 axis = static_cast<u32>(a[5]);
                const f64 across = a[3] + a[4];
                Vec3 half{across, across, across};
                if (axis == 0) half.x = a[4];
                else if (axis == 1) half.y = a[4];
                else half.z = a[4];
                box = around({a[0], a[1], a[2]}, half);
                break;
            }
            case Op::Cone: {
                const u32 axis = static_cast<u32>(a[5]);
                Vec3 half{a[3], a[3], a[3]};
                Vec3 centre{a[0], a[1], a[2]};
                if (axis == 0) { half.x = a[4] * 0.5; centre.x += a[4] * 0.5; }
                else if (axis == 1) { half.y = a[4] * 0.5; centre.y += a[4] * 0.5; }
                else { half.z = a[4] * 0.5; centre.z += a[4] * 0.5; }
                box = around(centre, half);
                break;
            }
            case Op::Capsule: {
                const Vec3 p0{a[0], a[1], a[2]};
                const Vec3 p1{a[3], a[4], a[5]};
                Aabb b;
                b.low = {std::min(p0.x, p1.x), std::min(p0.y, p1.y), std::min(p0.z, p1.z)};
                b.high = {std::max(p0.x, p1.x), std::max(p0.y, p1.y), std::max(p0.z, p1.z)};
                box = grown(b, a[6]);
                break;
            }
            case Op::Wedge:
            case Op::Stairs: box = around({a[0], a[1], a[2]}, {a[3], a[4], a[5]}); break;

            case Op::Revolve: {
                // As far out as the profile reaches from the axis, all the way round, and as far
                // along the axis as the profile is tall. The profile's third dimension says
                // nothing: it is only ever asked at zero.
                //
                // A PARTIAL revolve keeps the whole turn's box, unchanged and on purpose. The
                // truth is tighter and the tighter one would have to be derived from an arc's
                // extremes, and this is not the place to be clever: a box larger than the shape
                // culls a little less and is always right, a box smaller than the shape deletes
                // whatever falls outside it and says nothing while it does. The soundness the cull
                // needs also still holds — the segment is inside the full revolution, so its
                // answer at a point outside this box is at least the distance to the box.
                const Aabb child = bounds_of(n.child[0]);
                if (child.infinite()) { box = everywhere(); break; }
                const u32 axis = static_cast<u32>(a[3]);
                u32 u = 0, v = 0;
                other_axes(axis, u, v);
                const f64 reach = std::max(std::abs(axis_of(child.low, u)),
                                           std::abs(axis_of(child.high, u)));
                Vec3 half{reach, reach, reach};
                const f64 lo = axis_of(child.low, axis);
                const f64 hi = axis_of(child.high, axis);
                half = with_axis(half, axis, (hi - lo) * 0.5);
                Vec3 centre{a[0], a[1], a[2]};
                centre = with_axis(centre, axis, axis_of(centre, axis) + (hi + lo) * 0.5);
                box = around(centre, half);
                break;
            }
            case Op::Spiral: {
                // The widest the curve ever gets, which is its first turn when it tightens and
                // its last when it opens out.
                const f64 ends = std::max(1.0, std::pow((a[4] > 0.0) ? a[4] : 1.0, a[6]));
                const f64 reach = a[3] * ends + a[5];
                Vec3 half{reach, reach, reach};
                half = with_axis(half, static_cast<u32>(a[7]), a[5]);
                box = around({a[0], a[1], a[2]}, half);
                break;
            }

            case Op::Union:
            case Op::SmoothUnion:
            // A chamfer fills the valley between two shapes, so it can put matter a little outside
            // both of them — as far as the forty-five degree plane reaches, which is inside the
            // box grown by the chamfer's own width. Growing by that keeps the cull sound as well
            // as the box honest: a point `t` outside the grown box is at least `t + width` from
            // either child, so the chamfer term is at least (2t + width)/root2 > t and the plain
            // minimum is at least t, and the cull is told no less than the truth.
            case Op::ChamferUnion: {
                box = bounds_of(n.child[0]);
                for (u32 c = 1; c < n.children; ++c) box = merged(box, bounds_of(n.child[c]));
                if (n.op == Op::SmoothUnion || n.op == Op::ChamferUnion) box = grown(box, a[0]);
                break;
            }
            case Op::Intersection:
            case Op::ChamferIntersection:
            case Op::SmoothIntersection: {
                box = bounds_of(n.child[0]);
                for (u32 c = 1; c < n.children; ++c) box = overlapped(box, bounds_of(n.child[c]));
                break;
            }

            // `min` and `max` are union and intersection written arithmetically, and a box is owed
            // to them for exactly the same reason.
            //
            // A box here means "the solid is inside this". `max(a, b)` is at most nought only
            // where BOTH are, so it is inside either child's box and the tighter one may be taken;
            // `min(a, b)` is at most nought where EITHER is, so it needs both. That holds whatever
            // the children are, which matters because these two are the ops an author reaches for
            // when a child is not a shape at all — `max { ashlar_band  add { constant 0.02
            // negate { bond } } }` is how the facility cuts its rustication joints, and the
            // arithmetic half is unbounded while the band is a box round one wall.
            //
            // Falling through to "everywhere" cost more than anything else in the building. Five
            // paint rules were written this way, and with no box the sampler had to ask each of
            // them at every solid voxel in the facility: one point nine million evaluations each,
            // three quarters of all the paint work in a cold build, for rules naming a course of
            // masonry and a vestibule floor.
            case Op::Max: {
                box = bounds_of(n.child[0]);
                for (u32 c = 1; c < n.children; ++c) box = overlapped(box, bounds_of(n.child[c]));
                break;
            }
            case Op::Min: {
                box = bounds_of(n.child[0]);
                for (u32 c = 1; c < n.children; ++c) box = merged(box, bounds_of(n.child[c]));
                break;
            }
            // Carving can only remove, so what is left is inside what it started as.
            // A chamfered cut only ever removes more, so what is left is still inside the first
            // child.
            case Op::Difference:
            case Op::ChamferDifference:
            case Op::SmoothDifference: box = bounds_of(n.child[0]); break;

            case Op::Translate: {
                Aabb child = bounds_of(n.child[0]);
                if (!child.infinite()) {
                    const Vec3 by{a[0], a[1], a[2]};
                    child.low = child.low + by;
                    child.high = child.high + by;
                }
                box = child;
                break;
            }
            // Turning a bounded shape leaves it bounded, and this is where the facility's time
            // went.
            //
            // A rotation moves the box's eight corners and the box round WHERE THEY LAND contains
            // the shape. That is looser than the shape's own extent — a long thin thing turned
            // forty-five degrees gets a box half again as wide as it needs — and it is exact
            // enough for the only thing a box is for, which is knowing when not to look.
            //
            // Leaving this unbounded was the single most expensive thing in the building. There
            // are a hundred and eighty-three rotations in the facility, and an unbounded child
            // makes its parent union unbounded and that one's parent too, so a colonnade turned
            // to face a courtyard took the box off everything above it all the way to the site.
            // Thirty-eight per cent of the field carried no box, and a node with no box is a node
            // no cull can skip: every evaluation anywhere walked into every one of them.
            //
            // The direction matters and is easy to get backwards, so `bounds contain a rotated
            // shape` in tests/test_field.cpp samples the surface and checks it is inside.
            case Op::Rotate: {
                Aabb child = bounds_of(n.child[0]);
                if (child.infinite()) { box = everywhere(); break; }
                // `eval` turns the POINT by the negated angles, so the shape itself turns by the
                // positive ones — and the inverse of that composition is these three applied in
                // the opposite order.
                const f64 cx = std::cos(a[0] * kTau), sx = std::sin(a[0] * kTau);
                const f64 cy = std::cos(a[1] * kTau), sy = std::sin(a[1] * kTau);
                const f64 cz = std::cos(a[2] * kTau), sz = std::sin(a[2] * kTau);
                Vec3 lo{1e30, 1e30, 1e30};
                Vec3 hi{-1e30, -1e30, -1e30};
                for (u32 corner = 0; corner < 8; ++corner) {
                    Vec3 q{(corner & 1u) ? child.high.x : child.low.x,
                           (corner & 2u) ? child.high.y : child.low.y,
                           (corner & 4u) ? child.high.z : child.low.z};
                    q = {q.x * cz - q.y * sz, q.x * sz + q.y * cz, q.z};
                    q = {q.x * cy + q.z * sy, q.y, -q.x * sy + q.z * cy};
                    q = {q.x, q.y * cx - q.z * sx, q.y * sx + q.z * cx};
                    lo = {std::min(lo.x, q.x), std::min(lo.y, q.y), std::min(lo.z, q.z)};
                    hi = {std::max(hi.x, q.x), std::max(hi.y, q.y), std::max(hi.z, q.z)};
                }
                box = Aabb{lo, hi};
                break;
            }

            // Scaling is the same argument with one condition on it, and the condition is not
            // about where the shape is — it is about what the node REPORTS.
            //
            // A box is not only a claim that the shape is inside it. Every cull that reads one
            // also assumes that a point `away` outside the box gets an answer of at least `away`,
            // because that is what lets it skip a child without asking. A non-uniform scale breaks
            // that: it evaluates the child at `p / s` and multiplies by the SMALLEST factor, so a
            // shape stretched twice along x reports half the true distance out there. The box
            // would be right and the cull reading it would still drop a child that could have been
            // nearest — which is a piece of the clip quietly missing.
            //
            // A uniform scale reports the distance exactly, so it is bounded and the rest are
            // left alone. `the boxes round a revolve, a spiral and a scaled shape cull nothing
            // they should keep` in tests/test_field.cpp is what says so: it takes every answer
            // before the boxes exist and demands the same ones after, and it caught this.
            case Op::Scale: {
                Aabb child = bounds_of(n.child[0]);
                if (child.infinite()) { box = everywhere(); break; }
                const Vec3 s{a[0] != 0.0 ? a[0] : 1.0, a[1] != 0.0 ? a[1] : 1.0,
                             a[2] != 0.0 ? a[2] : 1.0};
                const f64 least = std::min(std::abs(s.x), std::min(std::abs(s.y), std::abs(s.z)));
                const f64 most = std::max(std::abs(s.x), std::max(std::abs(s.y), std::abs(s.z)));
                if (most - least > 1e-12) { box = everywhere(); break; }
                const Vec3 one{child.low.x * s.x, child.low.y * s.y, child.low.z * s.z};
                const Vec3 two{child.high.x * s.x, child.high.y * s.y, child.high.z * s.z};
                box = Aabb{{std::min(one.x, two.x), std::min(one.y, two.y), std::min(one.z, two.z)},
                           {std::max(one.x, two.x), std::max(one.y, two.y), std::max(one.z, two.z)}};
                break;
            }

            // A shell reaches out as far as it reaches in; rounding and offsetting move the
            // surface by a known amount. What is left unbounded — twisting, bending — is left that
            // way rather than approximated, because a bound that is wrong by a little produces a
            // clip with pieces missing.
            // A limited repeat is a shape with a known extent: the child's box, plus as far
            // either side as the count allows. Leaving this unbounded was expensive in a way
            // nothing pointed at — a colonnade and a screen of slats are repeats, so every
            // evaluation anywhere in the clip walked into them and did the modulo arithmetic to
            // find out it was nowhere near. An unlimited repeat still gets an infinite box on
            // that axis, because it genuinely is infinite.
            case Op::Mirror:
            case Op::Repeat: {
                Aabb child = bounds_of(n.child[0]);
                if (child.infinite()) {
                    box = child;
                    break;
                }
                if (n.op == Op::Mirror) {
                    // Folding about the plane means the shape also exists at the mirrored
                    // coordinate, so the box is the union of the two.
                    const u32 axis = static_cast<u32>(a[0]);
                    const f64 low = axis_of(child.low, axis);
                    const f64 high = axis_of(child.high, axis);
                    const f64 reach = std::max(std::abs(low), std::abs(high));
                    child.low = with_axis(child.low, axis, -reach);
                    child.high = with_axis(child.high, axis, reach);
                    box = child;
                    break;
                }
                bool unlimited = false;
                for (u32 axis = 0; axis < 3; ++axis) {
                    const f64 period = a[axis];
                    const f64 limit = a[3 + axis];
                    if (period <= 0.0) continue;         // this axis does not repeat
                    if (limit <= 0.0) { unlimited = true; break; }
                    const f64 reach = period * limit;
                    child.low = with_axis(child.low, axis, axis_of(child.low, axis) - reach);
                    child.high = with_axis(child.high, axis, axis_of(child.high, axis) + reach);
                }
                box = unlimited ? everywhere() : child;
                break;
            }

            // The same, with the three things a cell draws allowed for in the order the copy is
            // built: scaled about its own origin, spun about its own axis, moved within its cell,
            // and only then tiled. The first two are `scatter_footprint`, which the settling test
            // reads as well so the two cannot disagree about how much room a copy takes.
            case Op::Scatter: {
                Aabb child = scatter_footprint(a, bounds_of(n.child[0]));
                if (child.infinite()) { box = child; break; }
                const f64 jitter = clamp(a[6], 0.0, 1.0);

                bool unlimited = false;
                for (u32 axis = 0; axis < 3; ++axis) {
                    const f64 period = a[axis];
                    const f64 limit = a[3 + axis];
                    if (period <= 0.0) continue;
                    if (limit <= 0.0) { unlimited = true; break; }
                    const f64 reach = period * limit + period * jitter * 0.5;
                    child.low = with_axis(child.low, axis, axis_of(child.low, axis) - reach);
                    child.high = with_axis(child.high, axis, axis_of(child.high, axis) + reach);
                }
                box = unlimited ? everywhere() : child;
                break;
            }

            // Scaling gets no box, and it is worth saying why, because the box is easy to work out
            // and wrong to use.
            //
            // A union culls a child when the point is further from the child's box than the
            // running answer — which is only sound if the child never *reports* less than its own
            // box distance. Uneven scaling reports the child's distance times the smallest factor,
            // so a cylinder stretched to twice its width along x answers half of what the truth is
            // out along x, while its box is honestly twice as wide. The cull then believes the
            // shape is further away than the shape itself says it is, and the surface moves.
            //
            // It cost an afternoon and was found only because the check that boxes change no
            // answer was run over a scaled shape. An infinite box culls nothing and hides nothing;
            // a scaled shape is nearly always inside an intersection with something square, and
            // that intersection has a box of its own that *is* sound.

            case Op::Shell: box = grown(bounds_of(n.child[0]), a[0]); break;
            case Op::Round: box = grown(bounds_of(n.child[0]), a[0]); break;
            case Op::Offset: box = grown(bounds_of(n.child[0]), std::abs(a[0])); break;
            case Op::Displace: {
                // Grown by as far as the displacement can actually push the surface, which is the
                // amount times the pattern's own reach. Assuming a pattern of unit size was wrong
                // in both directions: too tight for a pattern that swings wider than one, which
                // clips the shape's own bounding box and deletes what falls outside, and too
                // generous for the ordinary `multiply { mask amount }`, which then charged the
                // whole clip for a displacement of a few millimetres.
                f64 lo = 0.0, hi = 0.0;
                if (n.children < 2 || !value_range(n.child[1], lo, hi)) {
                    // Nothing is known about how far it moves, so nothing can be said about where
                    // the shape ends up. An honest infinity culls nothing and hides nothing.
                    box = everywhere();
                    break;
                }
                box = grown(bounds_of(n.child[0]),
                            std::abs(a[0]) * std::max(std::abs(lo), std::abs(hi)));
                break;
            }
            default: box = everywhere(); break;
        }
        bounds_[i] = box;

    }

    // Now the boxes exist, so the hierarchy over them can be built.
    //
    // Only the OUTERMOST union of a chain gets one: flattening reaches everything below it, so an
    // accelerator on a nested union would be built, stored, and never consulted. A union is
    // outermost when nothing that would flatten through it points at it, which — since every
    // child has a lower index than its parent — one backward pass settles.
    accelerator_of_.assign(nodes_.size(), kNoAccelerator);
    accelerators_.clear();

    std::vector<u8> swallowed(nodes_.size(), 0);
    for (usize i = 0; i < nodes_.size(); ++i) {
        const Node& n = nodes_[i];
        if (n.op != Op::Union) continue;
        for (u32 c = 0; c < n.children; ++c) {
            if (n.child[c] < nodes_.size() && nodes_[n.child[c]].op == Op::Union) {
                swallowed[n.child[c]] = 1;
            }
        }
    }

    std::vector<u32> leaves;
    for (usize i = 0; i < nodes_.size(); ++i) {
        if (nodes_[i].op != Op::Union || swallowed[i] != 0) continue;
        leaves.clear();
        flatten_union(static_cast<u32>(i), leaves);
        if (leaves.size() < accelerate_from_) continue;

        Accelerator bvh;
        bvh.nodes.reserve(leaves.size() * 2);
        bvh.order.reserve(leaves.size());
        build_bvh(bvh, leaves, 0, leaves.size());
        accelerator_of_[i] = static_cast<u32>(accelerators_.size());
        accelerators_.push_back(std::move(bvh));
    }
}

std::vector<Field::UnboundedCause> Field::unbounded_by_op() const {
    // Indexed by the op's own value, which is what makes this one pass rather than a search.
    std::vector<UnboundedCause> by_op(static_cast<usize>(Op::Power) + 1);
    for (usize i = 0; i < by_op.size(); ++i) by_op[i].op = static_cast<Op>(i);

    for (usize i = 0; i < nodes_.size() && i < bounds_.size(); ++i) {
        if (!bounds_[i].infinite()) continue;
        const Node& n = nodes_[i];

        // Downstream is decided by the CHILDREN's boxes and not by the op, because the same op is
        // both things in different places: a union of bounded parts bounds, and the one above a
        // twist does not. Asking the boxes is asking build_bounds what it actually did.
        bool from_below = false;
        for (u32 c = 0; c < n.children && c < 4; ++c) {
            if (n.child[c] < bounds_.size() && bounds_[n.child[c]].infinite()) from_below = true;
        }
        UnboundedCause& row = by_op[static_cast<usize>(n.op)];
        if (from_below) ++row.downstream;
        else ++row.source;
    }

    std::vector<UnboundedCause> out;
    for (const UnboundedCause& row : by_op) {
        if (row.source != 0 || row.downstream != 0) out.push_back(row);
    }
    std::sort(out.begin(), out.end(), [](const UnboundedCause& a, const UnboundedCause& b) {
        if (a.source != b.source) return a.source > b.source;
        return a.downstream > b.downstream;
    });
    return out;
}

const char* op_name(Op op) {
    switch (op) {
        case Op::Constant: return "constant";
        case Op::Parameter: return "parameter";
        case Op::Coordinate: return "coordinate";
        case Op::Radius: return "radius";
        case Op::Sphere: return "sphere";
        case Op::Box: return "box";
        case Op::Cylinder: return "cylinder";
        case Op::Capsule: return "capsule";
        case Op::Torus: return "torus";
        case Op::Arc: return "arc";
        case Op::Cone: return "cone";
        case Op::Plane: return "plane";
        case Op::Ellipsoid: return "ellipsoid";
        case Op::Prism: return "prism";
        case Op::Platonic: return "platonic";
        case Op::Wedge: return "wedge";
        case Op::Stairs: return "stairs";
        case Op::Revolve: return "revolve";
        case Op::Spiral: return "spiral";
        case Op::Union: return "union";
        case Op::Intersection: return "intersection";
        case Op::Difference: return "difference";
        case Op::SmoothUnion: return "smooth-union";
        case Op::SmoothDifference: return "smooth-difference";
        case Op::SmoothIntersection: return "smooth-intersection";
        case Op::ChamferUnion: return "chamfer-union";
        case Op::ChamferDifference: return "chamfer-difference";
        case Op::ChamferIntersection: return "chamfer-intersection";
        case Op::Translate: return "translate";
        case Op::Rotate: return "rotate";
        case Op::Scale: return "scale";
        case Op::Mirror: return "mirror";
        case Op::Repeat: return "repeat";
        case Op::PolarRepeat: return "polar-repeat";
        case Op::Scatter: return "scatter";
        case Op::Shell: return "shell";
        case Op::Round: return "round";
        case Op::Offset: return "offset";
        case Op::Displace: return "displace";
        case Op::Twist: return "twist";
        case Op::Bend: return "bend";
        case Op::Sine: return "sine";
        case Op::Waves: return "waves";
        case Op::Noise: return "noise";
        case Op::Fbm: return "fbm";
        case Op::Ridged: return "ridged";
        case Op::Rasp: return "rasp";
        case Op::Cells: return "cells";
        case Op::CellEdge: return "cell-edge";
        case Op::Curvature: return "curvature";
        case Op::Occlusion: return "occlusion";
        case Op::Facing: return "facing";
        case Op::Checker: return "checker";
        case Op::Stripes: return "stripes";
        case Op::Bricks: return "bricks";
        case Op::Add: return "add";
        case Op::Multiply: return "multiply";
        case Op::Min: return "min";
        case Op::Max: return "max";
        case Op::Blend: return "blend";
        case Op::Remap: return "remap";
        case Op::Abs: return "abs";
        case Op::Negate: return "negate";
        case Op::Step: return "step";
        case Op::Smoothstep: return "smoothstep";
        case Op::Clamp: return "clamp";
        case Op::Power: return "power";
    }
    return "?";
}

// See the declaration in field.hpp. One `bool` for a whole feature's control arm, because the
// thing it switches is read from three functions and none of them has a caller that could pass it.
bool g_rule_bounds = true;

void use_rule_bounds(bool on) { g_rule_bounds = on; }
bool rule_bounds_used() { return g_rule_bounds; }

bool Field::value_range(u32 at, f64& low, f64& high) const {
    if (at >= nodes_.size()) return false;
    const Node& n = nodes_[at];
    const f64* a = n.a;

    // Every child's range, or a refusal. Written once because almost every case wants it.
    f64 lo[4];
    f64 hi[4];
    const auto children = [&]() {
        for (u32 i = 0; i < n.children && i < 4; ++i) {
            if (!value_range(n.child[i], lo[i], hi[i])) return false;
        }
        return n.children > 0;
    };
    const auto span = [&](f64 l, f64 h) {
        low = std::min(l, h);
        high = std::max(l, h);
        return true;
    };

    switch (n.op) {
        case Op::Constant: return span(a[0], a[0]);
        case Op::Parameter: {
            const usize slot = static_cast<usize>(a[0]);
            const f64 v = (slot < parameters_.size()) ? parameters_[slot] : 0.0;
            return span(v, v);
        }

        // The waves and the grains, all of which are written to land in [-1, 1].
        case Op::Sine:
        case Op::Waves:
        case Op::Noise:
        case Op::Fbm:
        case Op::Ridged:
        case Op::Checker:
        case Op::Stripes:
        case Op::Facing:
            return span(-1.0, 1.0);

        // A brick pattern is how far into a joint the point is, as a fraction of a brick: never
        // more than half a brick inside one, and never further out of a joint than the joint is
        // wide. Which is a real bound rather than the assumed one, and it matters — a wall
        // displaced by its own mortar with a wide joint was being charged for a unit swing it
        // could not make.
        case Op::Bricks: {
            const f64 across = (a[0] != 0.0) ? std::abs(a[0]) : 1.0;
            const f64 up = (a[1] != 0.0) ? std::abs(a[1]) : 1.0;
            const f64 joint = std::abs(a[3]) * 0.5;
            return span(-0.5, std::max(joint / across, joint / up));
        }

        // Rasp is a depth cut into a surface: never positive, never deeper than it was asked for.
        case Op::Rasp: return span(-std::abs(a[1]), 0.0);

        // How much of a small sphere is solid — a fraction, by construction.
        case Op::Occlusion: return span(0.0, 1.0);

        // A distance to the nearest of one scattered point per cell, and the gap between the two
        // nearest. Both are distances in metres and both are bounded by the search neighbourhood,
        // which is one cell either way in each direction.
        case Op::Cells:
        case Op::CellEdge: return span(0.0, 3.0 * std::abs(a[0]));

        case Op::Step:
        case Op::Smoothstep: return span(0.0, 1.0);
        case Op::Clamp: return span(a[0], a[1]);
        case Op::Remap: return span(a[2], a[3]);

        case Op::Add: {
            if (!children()) return false;
            f64 l = 0.0, h = 0.0;
            for (u32 i = 0; i < n.children; ++i) { l += lo[i]; h += hi[i]; }
            return span(l, h);
        }
        case Op::Multiply: {
            if (!children()) return false;
            f64 l = lo[0], h = hi[0];
            for (u32 i = 1; i < n.children; ++i) {
                // Four corners, because either factor may straddle zero and the product's extreme
                // can be any of them.
                const f64 c[4] = {l * lo[i], l * hi[i], h * lo[i], h * hi[i]};
                l = std::min(std::min(c[0], c[1]), std::min(c[2], c[3]));
                h = std::max(std::max(c[0], c[1]), std::max(c[2], c[3]));
            }
            return span(l, h);
        }
        case Op::Min: {
            if (!children()) return false;
            f64 l = lo[0], h = hi[0];
            for (u32 i = 1; i < n.children; ++i) { l = std::min(l, lo[i]); h = std::min(h, hi[i]); }
            return span(l, h);
        }
        case Op::Max: {
            if (!children()) return false;
            f64 l = lo[0], h = hi[0];
            for (u32 i = 1; i < n.children; ++i) { l = std::max(l, lo[i]); h = std::max(h, hi[i]); }
            return span(l, h);
        }
        case Op::Blend: {
            if (n.children < 2 || !children()) return false;
            const f64 t = clamp(a[0], 0.0, 1.0);
            return span(lo[0] * (1.0 - t) + lo[1] * t, hi[0] * (1.0 - t) + hi[1] * t);
        }
        case Op::Negate: {
            if (!children()) return false;
            return span(-lo[0], -hi[0]);
        }
        case Op::Abs: {
            if (!children()) return false;
            const f64 reach = std::max(std::abs(lo[0]), std::abs(hi[0]));
            const f64 nearest = (lo[0] <= 0.0 && hi[0] >= 0.0)
                                    ? 0.0
                                    : std::min(std::abs(lo[0]), std::abs(hi[0]));
            return span(nearest, reach);
        }
        case Op::Power: {
            if (!children()) return false;
            const f64 e = a[0];
            if (e < 0.0) return false;   // unbounded as the value approaches zero
            const f64 reach = std::pow(std::max(std::abs(lo[0]), std::abs(hi[0])), e);
            if (lo[0] >= 0.0) return span(std::pow(lo[0], e), reach);
            if (hi[0] <= 0.0) return span(-reach, -std::pow(std::abs(hi[0]), e));
            return span(-reach, reach);
        }

        // ---- the operations that ask somewhere else and hand back what they were told ----------
        //
        // Every one of these evaluates its child at some OTHER point and returns that answer
        // unchanged. A mirror folds the point, a translate shifts it, a tiling wraps it, a twist
        // turns it. So the values the parent can take are a SUBSET of the values the child can
        // take, and the child's range bounds the parent's — conservatively, which is the only
        // direction that is safe here.
        //
        // `repeat` takes the smallest of several such answers and `polar-repeat` folds by angle;
        // both are still the child's own values, so the same bound holds for them.
        //
        // Refusing these was a hole with one loud symptom and several silent ones. `value_range` is
        // asked exactly one question — how far can a displacement move a surface — and a refusal
        // means "as far as you like". `clips/facility/site.clip` displaces its gravel court and its
        // flagged apron by `mirror { fbm }`; both zones therefore had no bounding box at all, so
        // eight paint rules keyed on or placed on them were asked at every solid voxel of the
        // estate (62% of all its paint evaluations), and `skip_slack` — which is a sum over every
        // displacement in the field — was infinite for the whole seven-building site because of
        // those three nodes.
        //
        // `scatter` is deliberately not here: it scales each copy by a per-cell factor as well as
        // moving it, so its range is its child's times a factor this does not work out, and a
        // guess in this function is how voxels disappear.
        case Op::Translate:
        case Op::Rotate:
        case Op::Mirror:
        case Op::Repeat:
        case Op::PolarRepeat:
        case Op::Twist:
        case Op::Bend: {
            if (!g_rule_bounds) return false;
            if (!children()) return false;
            return span(lo[0], hi[0]);
        }

        // A scale asks its child at `p / s` and multiplies the answer by the SMALLEST factor, so
        // the range comes back multiplied by that same factor. Reading the multiplier off `eval`
        // rather than assuming it is one is the whole of the case.
        case Op::Scale: {
            if (!g_rule_bounds) return false;
            if (!children()) return false;
            const f64 least = std::min(std::abs(a[0] != 0.0 ? a[0] : 1.0),
                                       std::min(std::abs(a[1] != 0.0 ? a[1] : 1.0),
                                                std::abs(a[2] != 0.0 ? a[2] : 1.0)));
            return span(lo[0] * least, hi[0] * least);
        }

        // And the two that shift an answer by a known amount. `shell` is left out for the same
        // reason `abs` needed its own case: it is |d| - t, which is not the interval its child
        // hands over.
        case Op::Round:
        case Op::Offset: {
            if (!g_rule_bounds) return false;
            if (!children()) return false;
            const f64 by = (n.op == Op::Round) ? -a[0] : a[0];
            return span(lo[0] + by, hi[0] + by);
        }

        default:
            // A coordinate, a radius, a solid's own distance, a curvature: all perfectly good to
            // displace by, and none of them says how far it can go. Refusing is the whole point —
            // guessing here is how voxels disappear.
            return false;
    }
}

namespace {

// How far a displacement can move a surface: the amount it was given, times the largest value the
// pattern can reach. Infinite when the pattern will not say.
f64 displacement_reach(const Field& f, const Node& n) {
    if (n.children < 2) return 1e30;
    f64 lo = 0.0, hi = 0.0;
    if (!f.value_range(n.child[1], lo, hi)) return 1e30;
    return std::abs(n.a[0]) * std::max(std::abs(lo), std::abs(hi));
}

}  // namespace

f64 Field::skip_slack() const {
    f64 slack = 0.0;
    for (const Node& n : nodes_) {
        if (n.op != Op::Displace) continue;
        const f64 reach = displacement_reach(*this, n);
        if (reach >= 1e30) return 1e30;
        slack += reach;
    }
    // Twice the amplitude, and the factor of two is not caution — it is the arithmetic.
    //
    // A displaced field is f(p) = d(p) + a·n(p) with |n| ≤ 1. Reading f at one point and asking
    // what it says about another has to survive the displacement twice: once because the reading
    // itself may be up to `a` further out than the true shape, and once because the far point may
    // be up to `a` further in. So f(p) ≥ f(c) − |p − c| − 2a, and a skip that subtracts only one
    // `a` will eventually step over a bump.
    //
    // "Eventually" is the whole problem with getting this wrong: it cost 265 voxels out of fifty
    // nine million the first time, which is nothing to look at and a hole in a wall to stand in.
    return slack * 2.0;
}

u32 Field::undisplaced(u32 at, f64& amplitude) const {
    amplitude = 0.0;
    u32 node = at;
    while (node < nodes_.size()) {
        const Node& n = nodes_[node];
        if (n.op != Op::Displace || n.children < 2) break;
        const f64 reach = displacement_reach(*this, n);
        if (reach >= 1e30) break;
        amplitude += reach;
        node = n.child[0];
    }
    return node;
}

void Field::union_children(u32 at, std::vector<Part>& out) const {
    out.clear();
    if (at >= nodes_.size()) return;

    // Flattened through nested unions, and that is the point of the method rather than an
    // incidental tidiness. A node holds four children, so a union of six parts is stored as a
    // union of a union — and a nested union's bounding box is the box around everything in it.
    // Stopping at the first level therefore hands back a "part" whose box covers the ground *and*
    // the building, so a box anywhere in the building counts as near the ground and is charged
    // the ground's displacement. Which is how a five-centimetre allowance meant for a lawn ended
    // up deciding how fast every wall in the building sampled.
    std::vector<Part> pending{Part{at, 0.0}};
    while (!pending.empty() && out.size() < 256) {
        const Part here = pending.back();
        pending.pop_back();
        const u32 node = here.node;
        if (node >= nodes_.size()) continue;
        const Node& n = nodes_[node];

        // A displacement distributes over what is under it. Peel it off, charge its amplitude to
        // everything below, and carry on taking the shape apart — twice the amplitude, because a
        // reading may be that far out and the point asked about may be that far in.
        if (n.op == Op::Displace) {
            const f64 moves = displacement_reach(*this, n);
            if (moves < 1e29) {
                // Twice, because a reading may be that far out and the point being asked about
                // may be that far in — the same doubling every skip in this file allows for.
                pending.push_back(Part{n.child[0], here.extra + moves * 2.0});
                continue;
            }
            // A displacement nobody can bound stays whole; metric_slack will say so too.
        }

        // Which combinations may be taken apart, and which may not, and it turns on one question:
        // can a child whose box is FAR from the box being settled still change the answer there?
        //
        // A union takes the minimum, so a far child answers something large and positive and
        // cannot win it. A difference is the first child minus the rest, so a far subtracted
        // child answers large-and-positive too, is negated, and cannot win the maximum. Both are
        // safe to flatten: a part that is nowhere near this box cannot affect it, so this box
        // need not be charged that part's slack.
        //
        // An intersection is not. It takes the maximum, so a far child answers large and positive
        // and that is exactly what a maximum keeps — a distant child dominates the result
        // everywhere. It stays whole.
        //
        // Stopping at a difference is what this cost before it was written down. A manifest that
        // ends `difference { built ... }` reported ONE part, so the sampler charged every box in
        // the building the worst slack of anything in it, almost nothing settled, and a building
        // that had taken nine seconds took eight minutes.
        // The chamfered three are deliberately absent, and it costs nothing worth having. A
        // chamfer is a treatment of ONE seam between two adjacent shapes -- an arris, a stop, a
        // drip -- so the thing it stands over is small, and leaving it whole charges a small box
        // its own slack instead of two smaller ones. Nobody chamfers a manifest.
        const bool flatten = n.op == Op::Union || n.op == Op::SmoothUnion ||
                             n.op == Op::Difference || n.op == Op::SmoothDifference;
        if (!flatten) {
            out.push_back(here);
            continue;
        }
        for (u32 i = 0; i < n.children; ++i) pending.push_back(Part{n.child[i], here.extra});
    }
    // Too wide to finish taking apart. What is left goes in whole.
    //
    // It used to throw the entire decomposition away and report the root as one part, on the
    // reasoning that a truncated list silently leaves some of the shape unaccounted for. The
    // reasoning is right and the remedy was wrong: an unflattened node is still a *part*, with an
    // honest box and an honest slack, it is merely a coarser one. Keeping it accounts for
    // everything and costs only precision.
    //
    // Collapsing instead cost eight minutes of build time and hid the cause completely. The
    // facility flattens to two hundred and seventeen parts, and the six voids the manifest
    // subtracts push it past the limit — so a building whose parts were all perfectly separable
    // reported ONE, every box in it was charged the worst slack in the whole clip, and almost
    // nothing settled. The number in the report said "1 parts" and read like a fact about the
    // building rather than a limit being hit.
    for (const Part& left : pending) out.push_back(left);
}

f64 Field::metric_slack(u32 at) const {
    if (at >= nodes_.size()) return kInfiniteSlack;
    const Node& n = nodes_[at];

    const auto worst_child = [&](u32 count) {
        f64 slack = 0.0;
        for (u32 i = 0; i < count && i < n.children; ++i) {
            const f64 child = metric_slack(n.child[i]);
            if (child >= kInfiniteSlack) return kInfiniteSlack;
            slack = std::max(slack, child);
        }
        return slack;
    };

    switch (n.op) {
        // Constants have no gradient at all, so they never mislead about a neighbour.
        case Op::Constant:
        case Op::Parameter:
            return 0.0;

        // A coordinate and a radius both move exactly one metre per metre, which is the
        // condition. `below=0` on either is a half space or a ball, and both are decidable for a
        // block from its centre.
        case Op::Coordinate:
        case Op::Radius:
            return 0.0;

        // Negating a field mirrors its values and changes none of its variation, so a shape and
        // its complement are settleable on exactly the same terms.
        case Op::Negate: return worst_child(1);

        // `min` and `max` again, and settleable for the same reason unions and intersections are:
        // the larger (or smaller) of two fields that each move at most a metre per metre also
        // moves at most a metre per metre.
        case Op::Max:
        case Op::Min:
            return worst_child(n.children);

        // Adding is where this stops being obvious, and the condition is exact.
        //
        // Two fields that each vary by at most the radius of a box can, added, vary by twice it —
        // so a sum is NOT settleable in general and giving it a slack would let the sampler decide
        // a block from a reading that does not bound it. But a term that does not vary in space at
        // all contributes nothing to the variation, so a sum with at most one moving part is as
        // settleable as that part.
        //
        // That is not a corner case, it is how a mason's rule is written: the facility cuts its
        // rustication joints with `max { ashlar_band  add { constant 0.02  negate { bond } } }`,
        // and without this the four rules that do it were asked at every solid voxel inside the
        // walls — twenty-six million evaluations each, half of all the paint in the building.
        case Op::Add: {
            f64 slack = 0.0;
            u32 moving = 0;
            for (u32 i = 0; i < n.children; ++i) {
                const Node& child = nodes_[n.child[i]];
                if (child.op == Op::Constant || child.op == Op::Parameter) continue;
                if (++moving > 1) return kInfiniteSlack;
                slack = metric_slack(n.child[i]);
                if (slack >= kInfiniteSlack) return kInfiniteSlack;
            }
            return slack;
        }

        // The solids. Some of these are exact distances and some are bounds that under-state,
        // which is the safe direction: a reading that says "nearer than it really is" can only
        // make the sampler ask more often, never less.
        case Op::Sphere:
        case Op::Box:
        case Op::Cylinder:
        case Op::Capsule:
        case Op::Torus:
        // A segment of one is the exact distance to its centre-line less the tube, which is a real
        // distance to the real shape both within the arc and past either end of it.
        case Op::Arc:
        case Op::Cone:
        case Op::Plane:
        case Op::Ellipsoid:
        case Op::Prism:
        case Op::Platonic:
        case Op::Wedge:
        case Op::Stairs:
            return 0.0;

        // A spiral is walked as a chain of capsules and the answer is the least of their exact
        // distances, so it is a real distance to a real shape: no allowance at all.
        case Op::Spiral:
            return 0.0;

        // Revolving moves the point before asking, and the move — (x, y, z) to (radius, height) —
        // never separates two points by more than they were separated to begin with, because the
        // radius is a one-Lipschitz function of the two coordinates it collapses. So the profile's
        // own honesty carries through unchanged.
        //
        // It also never over-states. The profile is asked in the whole plane while only the half
        // with a positive radius is swept, so when a profile crosses its own axis the answer is
        // the distance to a shape slightly larger than the one that is built — smaller than the
        // truth, which is the direction that costs time rather than voxels.
        //
        // A PARTIAL revolve is still a distance and still on the safe side of one. Outside the
        // wedge it is exactly the distance to the end cap; inside the wedge and outside the
        // profile it is exactly the profile's own; and inside the solid it is the least of the
        // full revolution's distance and the two caps' — which can only be SHORTER than the true
        // distance to the segment's own surface, because the segment's swept face is a piece of
        // the full revolution's and a piece is never nearer than the whole.
        case Op::Revolve:
            return worst_child(1);

        // Combining by min, max or a blend of the two keeps the gradient bounded by its
        // steepest input, so the slack is the worst child's.
        case Op::Union:
        case Op::Intersection:
        case Op::Difference:
        case Op::SmoothUnion:
        case Op::SmoothDifference:
        case Op::SmoothIntersection:
            return worst_child(n.children);

        // A chamfer is `min` (or `max`) plus a bounded correction, and the bound is what the clamp
        // in `chamfer_min` exists to give. The correction never moves the answer more than one
        // chamfer's half-diagonal away from the plain minimum, so the field is the plain
        // minimum's — one metre per metre, and the children's own slack — plus twice that: once
        // because the reading may be that far out, and once because the point being asked about
        // may be that far in. The same doubling every allowance in this file carries.
        case Op::ChamferUnion:
        case Op::ChamferDifference:
        case Op::ChamferIntersection: {
            const f64 base = worst_child(n.children);
            if (base >= kInfiniteSlack) return kInfiniteSlack;
            return base + 2.0 * kInvRoot2 * std::abs(n.a[0]);
        }

        // Moving the point before asking. A rigid motion preserves distance exactly, and folding
        // about a plane is the distance to the shape and its reflection, which is also exact.
        case Op::Translate:
        case Op::Rotate:
        case Op::Mirror:
            return worst_child(1);

        // Scaling divides the point and multiplies the answer back by the *smallest* of the
        // factors, which is exactly the arithmetic that keeps the result from ever over-stating
        // the distance however unevenly the shape is stretched. So it says as much about its
        // neighbourhood as its child does.
        case Op::Scale:
            return worst_child(1);

        // Repetition does not, and this is worth being precise about because it looks like it
        // should. Folding a coordinate into its nearest cell gives the distance to the copy in
        // *that* cell — which is not the nearest copy when the shape sits off-centre in its cell.
        // A row of slats each hard against the left of its cell reports, for a point just past
        // one slat, the distance back to that slat rather than the shorter distance forward to
        // the next. That is an *over*-statement, which is the dangerous direction: it says
        // "nothing near here" when there is something near here, and a sampler that believes it
        // skips over the slat.
        //
        // Found by making the sampler faster: with the parts of a shape told apart, boxes near
        // the screen of slats began settling on that overstated distance, and sixteen hundred
        // voxels of slat quietly stopped existing.
        //
        // Except that eval no longer folds blindly: it checks the leaning neighbour as well, which
        // makes the answer the true distance to the nearest copy — provided a copy fits inside its
        // own cell. When one does not, copies overlap, no bounded number of neighbours is enough,
        // and there is nothing honest to say.
        case Op::Repeat: {
            const f64 base = metric_slack(n.child[0]);
            if (base >= kInfiniteSlack) return kInfiniteSlack;
            const Aabb child = bounds_of(n.child[0]);
            for (u32 axis = 0; axis < 3; ++axis) {
                const f64 period = n.a[axis];
                if (period <= 0.0) continue;      // this axis does not repeat
                if (child.infinite()) return kInfiniteSlack;
                if (axis_of(child.high, axis) - axis_of(child.low, axis) > period) {
                    return kInfiniteSlack;
                }
            }
            return base;
        }

        // The same argument as `repeat`, with the three things a cell draws counted into the room
        // a copy needs.
        //
        // Being able to say yes here is most of the point of the op. A gravel bed is tens of
        // thousands of copies, and a field that cannot settle a box is one asked per voxel: the
        // difference between a bed that samples in a second and one that samples in a minute is
        // entirely this test. So the room is worked out rather than assumed — the copy shrinks
        // (never grows), may be spun through the circle its own corners reach, and may be moved by
        // half the jitter either way, and all of that has to leave it inside its own cell.
        case Op::Scatter: {
            const f64 base = metric_slack(n.child[0]);
            if (base >= kInfiniteSlack) return kInfiniteSlack;
            const Aabb child = scatter_footprint(n.a, bounds_of(n.child[0]));
            const f64 jitter = clamp(n.a[6], 0.0, 1.0);
            for (u32 axis = 0; axis < 3; ++axis) {
                const f64 period = n.a[axis];
                if (period <= 0.0) continue;
                if (child.infinite()) return kInfiniteSlack;
                // INSIDE its own cell, and not merely narrower than one. `repeat` can get away
                // with the weaker test because all of its copies sit the same way in their cells,
                // so one that hangs over an edge hangs over every edge by the same amount and the
                // leaning neighbour is still the nearest thing. A scatter's copies are placed
                // INDEPENDENTLY, so one that hangs over can be beaten by a copy two cells away
                // that jittered toward the point — and that is an over-statement, the direction
                // that makes a sampler skip over matter that is there.
                //
                // Found by demanding the field be a distance: a bed of pebbles modelled 0.06 m
                // off their own origin reported a slack of nought and then moved 0.0098 m over a
                // step of 0.003. The remedy an author wants is the one they would have chosen
                // anyway — model the thing on its own origin — so the message this sends is a
                // refusal to promise rather than a different answer.
                const f64 half = period * 0.5;
                const f64 travel = period * jitter * 0.5;
                if (axis_of(child.low, axis) - travel < -half) return kInfiniteSlack;
                if (axis_of(child.high, axis) + travel > half) return kInfiniteSlack;
            }
            return base;
        }

        // The same objection, about an angle rather than a coordinate, and without the tidy
        // bound: how far a sector is depends on how far out you are. A partial `around` is the
        // same answer for the same reason — it folds to the nearest copy BY ANGLE, and the nearest
        // copy by angle is only the nearest by distance when the copy is round, which a colonnade
        // is and a bracket is not.
        case Op::PolarRepeat:
            return kInfiniteSlack;

        // Changing the answer by a constant, or taking its absolute value, leaves the gradient
        // alone.
        case Op::Shell:
        case Op::Round:
        case Op::Offset:
            return worst_child(1);

        case Op::Displace: {
            const f64 reach = displacement_reach(*this, n);
            if (reach >= kInfiniteSlack) return kInfiniteSlack;
            const f64 base = metric_slack(n.child[0]);
            if (base >= kInfiniteSlack) return kInfiniteSlack;
            // Twice the amplitude, for the reason spelled out in skip_slack.
            return base + reach * 2.0;
        }

        default:
            // Patterns, curvature, occlusion, facing, and anything that distorts space. All of
            // them are perfectly good to read — they just say nothing about the next voxel.
            return kInfiniteSlack;
    }
}


// ---- R12b: the non-recursive twin ------------------------------------------------------------
//
// See the declaration in field.hpp for why this exists. The shape of it: one frame per node on the
// way down, a `step` counter saying how far through that node's work we are, and one register for
// the value a finished child left behind. Nothing here recurses and nothing allocates.
namespace {

struct MirrorFrame {
    u32 node = 0;
    u32 step = 0;       // which child, or which sample point, this frame is on
    Vec3 p;             // the point this node was asked at
    f64 acc = 0.0;      // the answer so far
    // `repeat` works out its folded point and its leaning neighbours once, on the way in, and then
    // walks up to eight combinations of them. Kept in the frame because a shader has nowhere else.
    Vec3 fold;
    u32 axes[3]{0, 0, 0};
    f64 lean[3]{0.0, 0.0, 0.0};
    u32 neighbours = 0;
    // What the copy currently being asked about was scaled by, so the answer can be multiplied
    // back on the way out. `scatter` is the only op that changes the point AND the answer by the
    // same number, and a frame is the only place that number can wait — the recursive evaluator
    // holds it in a local, and this one has no locals that survive a push.
    f64 scale = 1.0;
};

// The fourteen directions `occlusion` asks along, and the six `curvature` does. Written here rather
// than rebuilt per visit for the same reason the recursive one has them as a local constant.
constexpr f64 kDiag = 0.5773502691896258;
const Vec3 kOcclusionDirs[14] = {{1, 0, 0},   {-1, 0, 0},  {0, 1, 0},      {0, -1, 0},
                                 {0, 0, 1},   {0, 0, -1},  {kDiag, kDiag, kDiag},
                                 {kDiag, kDiag, -kDiag},   {kDiag, -kDiag, kDiag},
                                 {kDiag, -kDiag, -kDiag},  {-kDiag, kDiag, kDiag},
                                 {-kDiag, kDiag, -kDiag},  {-kDiag, -kDiag, kDiag},
                                 {-kDiag, -kDiag, -kDiag}};

bool mirror_is_leaf(Op op) {
    switch (op) {
        case Op::Constant: case Op::Parameter: case Op::Coordinate: case Op::Radius:
        case Op::Sphere: case Op::Box: case Op::Cylinder: case Op::Capsule: case Op::Torus:
        case Op::Arc:
        case Op::Cone: case Op::Plane: case Op::Ellipsoid: case Op::Prism: case Op::Platonic:
        case Op::Wedge: case Op::Stairs: case Op::Spiral: case Op::Fbm: case Op::Noise:
        case Op::Ridged: case Op::Rasp: case Op::Cells: case Op::CellEdge: case Op::Sine:
        case Op::Waves: case Op::Checker: case Op::Stripes: case Op::Bricks:
            return true;
        default:
            return false;
    }
}

// What a one-child op does to its child's answer on the way out.
f64 mirror_after(Op op, const f64* a, f64 v) {
    switch (op) {
        case Op::Shell: return std::abs(v) - a[0];
        case Op::Round: return v - a[0];
        case Op::Offset: return v + a[0];
        case Op::Negate: return -v;
        case Op::Abs: return std::abs(v);
        case Op::Step: return (v > a[0]) ? 1.0 : 0.0;
        case Op::Smoothstep: {
            const f64 span = a[1] - a[0];
            if (span == 0.0) return (v > a[0]) ? 1.0 : 0.0;
            const f64 t = clamp((v - a[0]) / span, 0.0, 1.0);
            return t * t * (3.0 - 2.0 * t);
        }
        case Op::Clamp: return clamp(v, a[0], a[1]);
        case Op::Power: return (v < 0.0 ? -1.0 : 1.0) * std::pow(std::abs(v), a[0]);
        case Op::Remap: {
            const f64 span = a[1] - a[0];
            const f64 t = (span == 0.0) ? 0.0 : clamp((v - a[0]) / span, 0.0, 1.0);
            return a[2] + (a[3] - a[2]) * t;
        }
        default: return v;
    }
}

// ...and how a many-child op folds the next child into the answer so far.
f64 mirror_fold(Op op, const f64* a, f64 acc, f64 v) {
    switch (op) {
        case Op::Union: case Op::Min: return std::min(acc, v);
        case Op::Intersection: case Op::Max: return std::max(acc, v);
        case Op::Difference: return std::max(acc, -v);
        case Op::SmoothUnion: return smooth_min(acc, v, a[0]);
        case Op::SmoothIntersection: return smooth_max(acc, v, a[0]);
        case Op::SmoothDifference: return smooth_max(acc, -v, a[0]);
        case Op::ChamferUnion: return chamfer_min(acc, v, a[0]);
        case Op::ChamferIntersection: return chamfer_max(acc, v, a[0]);
        case Op::ChamferDifference: return chamfer_max(acc, -v, a[0]);
        case Op::Add: return acc + v;
        case Op::Multiply: return acc * v;
        default: return v;
    }
}

bool mirror_op_known(Op op) {
    if (mirror_is_leaf(op)) return true;
    switch (op) {
        case Op::Revolve: case Op::Translate: case Op::Rotate: case Op::Scale: case Op::Mirror:
        case Op::PolarRepeat: case Op::Twist: case Op::Bend: case Op::Shell: case Op::Round:
        case Op::Offset: case Op::Negate: case Op::Abs: case Op::Step: case Op::Smoothstep:
        case Op::Clamp: case Op::Power: case Op::Remap: case Op::Curvature: case Op::Occlusion:
        case Op::Facing: case Op::Displace: case Op::Blend: case Op::Union: case Op::Intersection:
        case Op::Difference: case Op::SmoothUnion: case Op::SmoothIntersection:
        case Op::SmoothDifference: case Op::Add: case Op::Multiply: case Op::Min: case Op::Max:
        case Op::ChamferUnion: case Op::ChamferIntersection: case Op::ChamferDifference:
        case Op::Repeat: case Op::Scatter:
            return true;
        default:
            return false;
    }
}

}  // namespace

bool Field::mirror_covers(u32 at, Op* missing) const {
    if (at >= nodes_.size()) return false;
    // Iterative, because a checker that recurses to ask whether something recurses is a poor joke.
    u32 stack[kMirrorStack];
    u32 top = 0;
    stack[top++] = at;
    std::vector<u8> seen(nodes_.size(), 0);
    while (top > 0) {
        const u32 index = stack[--top];
        if (index >= nodes_.size() || seen[index]) continue;
        seen[index] = 1;
        const Node& n = nodes_[index];
        if (!mirror_op_known(n.op)) {
            if (missing != nullptr) *missing = n.op;
            return false;
        }
        for (u32 c = 0; c < n.children && c < 4; ++c) {
            if (top < kMirrorStack) stack[top++] = n.child[c];
        }
    }
    return true;
}

namespace {
// One place where the narrowing happens, so the two entry points cannot drift apart.
thread_local bool g_mirror_single = false;
inline f64 narrow(f64 v) {
    return g_mirror_single ? static_cast<f64>(static_cast<float>(v)) : v;
}
inline Vec3 narrow(Vec3 v) { return {narrow(v.x), narrow(v.y), narrow(v.z)}; }
}  // namespace

bool Field::mirror_eval_single(u32 at, Vec3 p, f64& out) const {
    g_mirror_single = true;
    const bool ok = mirror_eval(at, narrow(p), out, nullptr);
    g_mirror_single = false;
    return ok;
}

bool Field::mirror_eval(u32 at, Vec3 p, f64& out, u32* deepest) const {
    if (at >= nodes_.size()) return false;
    MirrorFrame stack[kMirrorStack];
    u32 top = 0;
    f64 ret = 0.0;   // what the child that just finished came back with
    u32 low_water = 0;

    stack[top].node = at;
    stack[top].step = 0;
    stack[top].p = p;
    ++top;

    while (top > 0) {
        if (top > low_water) low_water = top;
        MirrorFrame& f = stack[top - 1];
        const Node& n = nodes_[f.node];
        const f64* a = n.a;

        // A child to push, worked out by the op and the step. `push` is the only way down and
        // `finish` the only way up, so a case that forgets to do one of them cannot silently
        // return the previous node's answer -- it loops, and the test catches a loop.
        const auto push = [&](u32 child, Vec3 where) -> bool {
            if (child >= nodes_.size() || top >= kMirrorStack) return false;
            stack[top].node = child;
            stack[top].step = 0;
            stack[top].p = narrow(where);
            stack[top].acc = 0.0;
            stack[top].neighbours = 0;
            stack[top].scale = 1.0;
            ++top;
            return true;
        };
        const auto finish = [&](f64 value) {
            ret = narrow(value);
            --top;
        };

        if (mirror_is_leaf(n.op)) {
            // A leaf has no children, so the recursive evaluator does not recurse on
            // one either: for these the two evaluators are the same code by
            // construction, and what is being proved here is the WALK.
            finish(eval(f.node, f.p));
            continue;
        }

        switch (n.op) {
            // ---- one child, the point changed on the way in -------------------------------
            case Op::Revolve: {
                const u32 axis = static_cast<u32>(a[3]);
                u32 u = 0, v = 0;
                other_axes(axis, u, v);
                const Vec3 q = f.p - Vec3{a[0], a[1], a[2]};
                const f64 r = std::hypot(axis_of(q, u), axis_of(q, v));
                // The profile's plane: a radius across, the height along, and nothing else.
                const auto flat_at = [&](f64 radius) {
                    Vec3 flat{0, 0, 0};
                    flat = with_axis(flat, u, radius);
                    flat = with_axis(flat, axis, axis_of(q, axis));
                    return flat;
                };

                if (!is_partial_sweep(a[5])) {
                    if (f.step == 0) {
                        f.step = 1;
                        if (!push(n.child[0], flat_at(r))) return false;
                        continue;
                    }
                    finish(ret);
                    continue;
                }

                // Part of the way round. The recursive evaluator asks the profile once, twice or
                // three times depending on where the point stands, so this one carries a step per
                // ASK — the same mechanism `curvature` and `occlusion` use — and `lean[0]` holds
                // the out-of-plane leg of whichever cap is being measured.
                const f64 rel = wrap_turn(std::atan2(axis_of(q, v), axis_of(q, u)) / kTau - a[4]);
                const auto ask_cap = [&](f64 delta, u32 next) -> bool {
                    const f64 turn = delta * kTau;
                    f.lean[0] = r * std::sin(turn);
                    f.step = next;
                    return push(n.child[0], flat_at(r * std::cos(turn)));
                };
                const auto cap_away = [&]() { return std::hypot(std::max(ret, 0.0), f.lean[0]); };

                switch (f.step) {
                    case 0:
                        if (rel > a[5]) {
                            // Outside the wedge: one ask, at the nearer end cap.
                            if (!ask_cap(nearer_end(rel, a[5]), 4)) return false;
                            continue;
                        }
                        f.step = 1;
                        if (!push(n.child[0], flat_at(r))) return false;
                        continue;
                    case 1:
                        if (ret >= 0.0) { finish(ret); continue; }
                        f.acc = ret;
                        if (!ask_cap(rel, 2)) return false;
                        continue;
                    case 2:
                        f.lean[1] = cap_away();
                        if (!ask_cap(rel - a[5], 3)) return false;
                        continue;
                    case 3:
                        finish(std::max(f.acc, -std::min(f.lean[1], cap_away())));
                        continue;
                    default:
                        finish(cap_away());
                        continue;
                }
            }
            case Op::Translate: {
                if (f.step == 0) {
                    f.step = 1;
                    if (!push(n.child[0], f.p - Vec3{a[0], a[1], a[2]})) return false;
                    continue;
                }
                finish(ret);
                continue;
            }
            case Op::Rotate: {
                if (f.step == 0) {
                    f.step = 1;
                    const f64 cx = std::cos(-a[0] * kTau), sx = std::sin(-a[0] * kTau);
                    const f64 cy = std::cos(-a[1] * kTau), sy = std::sin(-a[1] * kTau);
                    const f64 cz = std::cos(-a[2] * kTau), sz = std::sin(-a[2] * kTau);
                    Vec3 q = f.p;
                    q = {q.x, q.y * cx - q.z * sx, q.y * sx + q.z * cx};
                    q = {q.x * cy + q.z * sy, q.y, -q.x * sy + q.z * cy};
                    q = {q.x * cz - q.y * sz, q.x * sz + q.y * cz, q.z};
                    if (!push(n.child[0], q)) return false;
                    continue;
                }
                finish(ret);
                continue;
            }
            case Op::Scale: {
                const Vec3 s{a[0] != 0.0 ? a[0] : 1.0, a[1] != 0.0 ? a[1] : 1.0,
                             a[2] != 0.0 ? a[2] : 1.0};
                if (f.step == 0) {
                    f.step = 1;
                    if (!push(n.child[0], {f.p.x / s.x, f.p.y / s.y, f.p.z / s.z})) return false;
                    continue;
                }
                // ...and the smallest factor applied on the way OUT, which is the half of this op
                // a transliteration is most likely to drop.
                const f64 smallest =
                    std::min(std::abs(s.x), std::min(std::abs(s.y), std::abs(s.z)));
                finish(ret * smallest);
                continue;
            }
            case Op::Mirror: {
                if (f.step == 0) {
                    f.step = 1;
                    const u32 axis = static_cast<u32>(a[0]);
                    if (!push(n.child[0], with_axis(f.p, axis, std::abs(axis_of(f.p, axis)))))
                        return false;
                    continue;
                }
                finish(ret);
                continue;
            }
            case Op::PolarRepeat: {
                if (f.step == 0) {
                    f.step = 1;
                    const u32 count = std::max(1u, static_cast<u32>(a[0]));
                    const u32 axis = static_cast<u32>(a[1]);
                    u32 u = 0, v = 0;
                    other_axes(axis, u, v);
                    const f64 x = axis_of(f.p, u), y = axis_of(f.p, v);
                    f64 angle = std::atan2(y, x);
                    if (!is_partial_sweep(a[3])) {
                        const f64 sector = kTau / static_cast<f64>(count);
                        angle -= sector * std::round(angle / sector);
                    } else {
                        angle -= kTau * polar_copy_turn(angle / kTau, a[2], a[3], count);
                    }
                    const f64 r = std::hypot(x, y);
                    Vec3 q = f.p;
                    q = with_axis(q, u, std::cos(angle) * r);
                    q = with_axis(q, v, std::sin(angle) * r);
                    if (!push(n.child[0], q)) return false;
                    continue;
                }
                finish(ret);
                continue;
            }
            case Op::Twist:
            case Op::Bend: {
                if (f.step == 0) {
                    f.step = 1;
                    const u32 axis = static_cast<u32>(a[1]);
                    u32 u = 0, v = 0;
                    other_axes(axis, u, v);
                    const f64 along = (n.op == Op::Twist) ? axis_of(f.p, axis) : axis_of(f.p, u);
                    const f64 angle = -a[0] * kTau * along;
                    const f64 c = std::cos(angle), sn = std::sin(angle);
                    const f64 x = axis_of(f.p, u), y = axis_of(f.p, v);
                    Vec3 q = f.p;
                    q = with_axis(q, u, x * c - y * sn);
                    q = with_axis(q, v, x * sn + y * c);
                    if (!push(n.child[0], q)) return false;
                    continue;
                }
                finish(ret);
                continue;
            }

            // ---- one child, the ANSWER changed on the way out -----------------------------
            case Op::Shell: case Op::Round: case Op::Offset: case Op::Negate: case Op::Abs:
            case Op::Step: case Op::Smoothstep: case Op::Clamp: case Op::Power:
            case Op::Remap: case Op::Curvature: case Op::Occlusion: case Op::Facing: {
                // The three re-entrant ones share this block deliberately: they differ only in how
                // many times they ask and where, which is the `step` counter's whole job.
                if (n.op == Op::Curvature) {
                    const f64 r = (a[0] > 0.0) ? a[0] : 0.05;
                    // step 0 asks the centre; steps 1..6 ask the six axes. `lean[0]` holds the
                    // centre and `acc` the sum around it, because the answer needs both.
                    if (f.step == 1) f.lean[0] = ret;
                    else if (f.step > 1) f.acc += ret;
                    if (f.step == 7) {
                        finish((f.acc / 6.0 - f.lean[0]) / r);
                        continue;
                    }
                    Vec3 where = f.p;
                    switch (f.step) {
                        case 1: where.x += r; break;
                        case 2: where.x -= r; break;
                        case 3: where.y += r; break;
                        case 4: where.y -= r; break;
                        case 5: where.z += r; break;
                        case 6: where.z -= r; break;
                        default: break;   // step 0 is the centre, asked at f.p
                    }
                    ++f.step;
                    if (!push(n.child[0], where)) return false;
                    continue;
                }
                if (n.op == Op::Occlusion) {
                    const f64 r = (a[0] > 0.0) ? a[0] : 0.15;
                    if (f.step > 0 && ret < 0.0) f.acc += 1.0;
                    if (f.step == 14) {
                        finish(f.acc / 14.0);
                        continue;
                    }
                    const Vec3 where = f.p + kOcclusionDirs[f.step] * r;
                    ++f.step;
                    if (!push(n.child[0], where)) return false;
                    continue;
                }
                if (n.op == Op::Facing) {
                    const f64 step = (a[1] > 0.0) ? a[1] : 0.02;
                    if (f.step > 0) {
                        // +x -x +y -y +z -z, differenced in pairs exactly as normal_at does.
                        if (f.step % 2 == 1) f.lean[(f.step - 1) / 2] = ret;
                        else f.lean[(f.step - 1) / 2] -= ret;
                    }
                    if (f.step == 6) {
                        const Vec3 normal = normalise({f.lean[0], f.lean[1], f.lean[2]});
                        const u32 axis = static_cast<u32>(a[0]);
                        finish((axis == 0) ? normal.x : (axis == 1) ? normal.y : normal.z);
                        continue;
                    }
                    Vec3 where = f.p;
                    const f64 sign = (f.step % 2 == 0) ? step : -step;
                    if (f.step / 2 == 0) where.x += sign;
                    else if (f.step / 2 == 1) where.y += sign;
                    else where.z += sign;
                    ++f.step;
                    if (!push(n.child[0], where)) return false;
                    continue;
                }
                if (f.step == 0) {
                    f.step = 1;
                    if (!push(n.child[0], f.p)) return false;
                    continue;
                }
                finish(mirror_after(n.op, a, ret));
                continue;
            }

            // ---- two children at the same point --------------------------------------------
            case Op::Displace: {
                if (f.step == 0) {
                    f.step = 1;
                    if (!push(n.child[0], f.p)) return false;
                    continue;
                }
                if (f.step == 1) {
                    f.acc = ret;
                    f.step = 2;
                    if (n.children < 2) { finish(f.acc); continue; }
                    if (!push(n.child[1], f.p)) return false;
                    continue;
                }
                finish(f.acc + a[0] * ret);
                continue;
            }
            case Op::Blend: {
                if (f.step == 0) {
                    f.step = 1;
                    if (!push(n.child[0], f.p)) return false;
                    continue;
                }
                if (f.step == 1) {
                    f.acc = ret;
                    f.step = 2;
                    if (n.children < 2) { finish(f.acc); continue; }
                    if (!push(n.child[1], f.p)) return false;
                    continue;
                }
                finish(f.acc * (1.0 - a[0]) + ret * a[0]);
                continue;
            }

            // ---- every child at the same point, folded together ---------------------------
            case Op::Union: case Op::Intersection: case Op::Difference:
            case Op::SmoothUnion: case Op::SmoothIntersection: case Op::SmoothDifference:
            case Op::ChamferUnion: case Op::ChamferIntersection: case Op::ChamferDifference:
            case Op::Add: case Op::Multiply: case Op::Min: case Op::Max: {
                if (f.step > 0) {
                    f.acc = (f.step == 1) ? ret : mirror_fold(n.op, a, f.acc, ret);
                    // `multiply` stops at the first factor that is nought, and the mirror has to
                    // stop in the same place or it is a different amount of work with the same
                    // answer -- which is the kind of difference that only shows up as a timing.
                    if (n.op == Op::Multiply && f.acc == 0.0) { finish(0.0); continue; }
                }
                if (f.step >= n.children) { finish(f.acc); continue; }
                const u32 child = n.child[f.step];
                ++f.step;
                if (!push(child, f.p)) return false;
                continue;
            }

            // ---- the point folded into a cell, then the leaning neighbours ------------------
            case Op::Repeat: {
                if (f.step == 0) {
                    Vec3 q = f.p;
                    f.neighbours = 0;
                    for (u32 axis = 0; axis < 3; ++axis) {
                        const f64 period = a[axis];
                        if (period <= 0.0) continue;
                        const f64 limit = a[3 + axis];
                        const f64 value = axis_of(f.p, axis);
                        f64 cell = std::round(value / period);
                        if (limit > 0.0) cell = clamp(cell, -limit, limit);
                        const f64 folded = value - period * cell;
                        q = with_axis(q, axis, folded);
                        f64 other = cell + ((folded >= 0.0) ? 1.0 : -1.0);
                        if (limit > 0.0) other = clamp(other, -limit, limit);
                        if (other != cell) {
                            f.axes[f.neighbours] = axis;
                            f.lean[f.neighbours] = value - period * other;
                            ++f.neighbours;
                        }
                    }
                    f.fold = q;
                    f.step = 1;
                    if (!push(n.child[0], q)) return false;
                    continue;
                }
                const u32 done = f.step - 1;   // how many points have come back
                f.acc = (done == 0) ? ret : std::min(f.acc, ret);
                const u32 combinations = 1u << f.neighbours;
                if (f.step >= combinations) { finish(f.acc); continue; }
                const u32 mask = f.step;       // 1..combinations-1, exactly the recursive loop
                Vec3 shifted = f.fold;
                for (u32 i = 0; i < f.neighbours; ++i) {
                    if ((mask >> i) & 1u) shifted = with_axis(shifted, f.axes[i], f.lean[i]);
                }
                ++f.step;
                if (!push(n.child[0], shifted)) return false;
                continue;
            }

            // ---- the same walk, over cell INDICES rather than folded points ------------------
            //
            // `repeat` can carry the folded point from cell to cell because every copy sits the
            // same way in its cell. A scatter's do not, so what is carried is the index and the
            // point is rebuilt from it each time — which is also what makes this frame need a
            // `scale`, since the copy's own size has to survive the push and come back.
            case Op::Scatter: {
                const auto cell_at = [&](u32 mask, f64 out[3]) {
                    out[0] = f.fold.x;
                    out[1] = f.fold.y;
                    out[2] = f.fold.z;
                    for (u32 i = 0; i < f.neighbours; ++i) {
                        if ((mask >> i) & 1u) out[f.axes[i]] = f.lean[i];
                    }
                };

                if (f.step == 0) {
                    f64 cell[3] = {0.0, 0.0, 0.0};
                    f.neighbours = 0;
                    for (u32 axis = 0; axis < 3; ++axis) {
                        const f64 period = a[axis];
                        if (period <= 0.0) continue;
                        const f64 limit = a[3 + axis];
                        const f64 value = axis_of(f.p, axis);
                        f64 here = std::round(value / period);
                        if (limit > 0.0) here = clamp(here, -limit, limit);
                        cell[axis] = here;
                        f64 other = here + ((value - period * here >= 0.0) ? 1.0 : -1.0);
                        if (limit > 0.0) other = clamp(other, -limit, limit);
                        if (other != here) {
                            f.axes[f.neighbours] = axis;
                            f.lean[f.neighbours] = other;
                            ++f.neighbours;
                        }
                    }
                    f.fold = Vec3{cell[0], cell[1], cell[2]};
                    f.step = 1;
                    const Vec3 q = scatter_point(a, f.p, cell, f.scale);
                    f.scale = narrow(f.scale);
                    if (!push(n.child[0], q)) return false;
                    continue;
                }

                const f64 came_back = f.scale * ret;
                const u32 done = f.step - 1;
                f.acc = (done == 0) ? came_back : std::min(f.acc, came_back);
                const u32 combinations = 1u << f.neighbours;
                if (f.step >= combinations) { finish(f.acc); continue; }
                f64 cell[3];
                cell_at(f.step, cell);
                ++f.step;
                const Vec3 q = scatter_point(a, f.p, cell, f.scale);
                f.scale = narrow(f.scale);
                if (!push(n.child[0], q)) return false;
                continue;
            }

            default:
                return false;   // an op the mirror has not learned. Never a number.
        }
    }

    if (deepest != nullptr) *deepest = low_water;
    out = ret;
    return true;
}

Vec3 Field::normal_at(u32 at, Vec3 p, f64 step) const {
    const f64 dx = eval(at, {p.x + step, p.y, p.z}) - eval(at, {p.x - step, p.y, p.z});
    const f64 dy = eval(at, {p.x, p.y + step, p.z}) - eval(at, {p.x, p.y - step, p.z});
    const f64 dz = eval(at, {p.x, p.y, p.z + step}) - eval(at, {p.x, p.y, p.z - step});
    return normalise({dx, dy, dz});
}

}  // namespace forge
}  // namespace ws
