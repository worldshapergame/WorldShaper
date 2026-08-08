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

// How far a point is from a box, or zero inside it. A lower bound on the distance to anything
// the box contains, which is exactly what a union needs to decide whether to look further.
f64 distance_to(const Field::Aabb& box, Vec3 p) {
    if (box.infinite()) return 0.0;
    const f64 dx = std::max(std::max(box.low.x - p.x, p.x - box.high.x), 0.0);
    const f64 dy = std::max(std::max(box.low.y - p.y, p.y - box.high.y), 0.0);
    const f64 dz = std::max(std::max(box.low.z - p.z, p.z - box.high.z), 0.0);
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// The same, squared, for the cull test — which compares against a distance it already has and so
// never needs the root. One square root per child per evaluation is not much; a hundred million
// evaluations with thirty children each is a hundred million square roots too many.
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

u32 Field::revolve(u32 profile, Vec3 centre, u32 axis) {
    Node n;
    n.op = Op::Revolve;
    n.child[0] = profile;
    n.children = 1;
    n.a[0] = centre.x; n.a[1] = centre.y; n.a[2] = centre.z;
    n.a[3] = static_cast<f64>(axis);
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

u32 Field::polar_repeat(u32 child, u32 count, u32 axis) {
    Node n = unary(Op::PolarRepeat, child);
    n.a[0] = static_cast<f64>(count);
    n.a[1] = static_cast<f64>(axis);
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

u32 Field::noise(f64 size, u32 seed) {
    Node n;
    n.op = Op::Noise;
    n.a[0] = size; n.a[1] = static_cast<f64>(seed);
    return push(n);
}

u32 Field::fbm(f64 size, u32 octaves, f64 gain, f64 lacunarity, u32 seed) {
    Node n;
    n.op = Op::Fbm;
    n.a[0] = size; n.a[1] = static_cast<f64>(octaves); n.a[2] = gain;
    n.a[3] = lacunarity; n.a[4] = static_cast<f64>(seed);
    return push(n);
}

u32 Field::ridged(f64 size, u32 octaves, f64 gain, f64 lacunarity, u32 seed) {
    Node n;
    n.op = Op::Ridged;
    n.a[0] = size; n.a[1] = static_cast<f64>(octaves); n.a[2] = gain;
    n.a[3] = lacunarity; n.a[4] = static_cast<f64>(seed);
    return push(n);
}

u32 Field::rasp(f64 size, f64 depth, u32 seed) {
    Node n;
    n.op = Op::Rasp;
    n.a[0] = size; n.a[1] = depth; n.a[2] = static_cast<f64>(seed);
    return push(n);
}

u32 Field::cells(f64 size, u32 seed) {
    Node n;
    n.op = Op::Cells;
    n.a[0] = size; n.a[1] = static_cast<f64>(seed);
    return push(n);
}

u32 Field::cell_edge(f64 size, u32 seed) {
    Node n;
    n.op = Op::CellEdge;
    n.a[0] = size; n.a[1] = static_cast<f64>(seed);
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
            Vec3 flat{0, 0, 0};
            flat = with_axis(flat, u, r);
            flat = with_axis(flat, axis, axis_of(q, axis));
            return eval(n.child[0], flat);
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
            u32 order[4] = {0, 1, 2, 3};
            if (!bounds_.empty() && n.children > 1) {
                f64 away[4];
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
                if (!bounds_.empty()) {
                    const f64 away = squared_distance_to(bounds_[n.child[i]], p);
                    // Outside the box at all, and either already inside something (so nothing
                    // outside can be nearer) or nearer than the box can possibly be.
                    if (away > 0.0 && (d < 0.0 || d * d <= away)) continue;
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
        case Op::PolarRepeat: {
            const u32 count = std::max(1u, static_cast<u32>(a[0]));
            const u32 axis = static_cast<u32>(a[1]);
            u32 u = 0, v = 0;
            other_axes(axis, u, v);
            const f64 x = axis_of(p, u), y = axis_of(p, v);
            const f64 sector = kTau / static_cast<f64>(count);
            f64 angle = std::atan2(y, x);
            angle -= sector * std::round(angle / sector);
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
        case Op::Noise: return value_noise(p, a[0], static_cast<u32>(a[1]));
        case Op::Fbm:
            return fbm_noise(p, a[0], static_cast<u32>(a[1]), a[2], a[3], static_cast<u32>(a[4]));
        case Op::Ridged: {
            const f64 v = fbm_noise(p, a[0], static_cast<u32>(a[1]), a[2], a[3],
                                    static_cast<u32>(a[4]));
            return 1.0 - 2.0 * std::abs(v);
        }
        case Op::Rasp: {
            // Ridges an order finer than the surface they sit on, which is what a filed or
            // scratched face looks like: many shallow parallel gouges rather than lumps.
            const f64 v = fbm_noise(p, a[0], 3u, 0.5, 2.7, static_cast<u32>(a[2]));
            return -std::abs(v) * a[1];
        }
        case Op::Cells: {
            f64 nearest = 0.0, second = 0.0;
            cell_noise(p, a[0], static_cast<u32>(a[1]), nearest, second);
            return nearest;
        }
        case Op::CellEdge: {
            f64 nearest = 0.0, second = 0.0;
            cell_noise(p, a[0], static_cast<u32>(a[1]), nearest, second);
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
            f64 v = eval(n.child[0], p);
            for (u32 i = 1; i < n.children; ++i) v *= eval(n.child[i], p);
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

}  // namespace

Field::Aabb Field::bounds_of(u32 node) const {
    if (node < bounds_.size()) return bounds_[node];
    return everywhere();
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
    u32 stack[64];
    u32 top = 0;
    stack[top++] = 0;
    while (top > 0) {
        const BvhNode& node = bvh.nodes[stack[--top]];
        const f64 away = squared_distance_to(node.box, p);
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
                stack[top++] = node.right;
                stack[top++] = node.left;
            } else {
                stack[top++] = node.left;
                stack[top++] = node.right;
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
            case Op::Torus: {
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
            case Op::SmoothUnion: {
                box = bounds_of(n.child[0]);
                for (u32 c = 1; c < n.children; ++c) box = merged(box, bounds_of(n.child[c]));
                if (n.op == Op::SmoothUnion) box = grown(box, a[0]);
                break;
            }
            case Op::Intersection:
            case Op::SmoothIntersection: {
                box = bounds_of(n.child[0]);
                for (u32 c = 1; c < n.children; ++c) box = overlapped(box, bounds_of(n.child[c]));
                break;
            }
            // Carving can only remove, so what is left is inside what it started as.
            case Op::Difference:
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
            // A shell reaches out as far as it reaches in; rounding and offsetting move the
            // surface by a known amount. Everything else — rotation, scaling, repetition,
            // twisting — is left unbounded rather than approximated, because a bound that is
            // wrong by a little produces a clip with pieces missing.
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
        if (leaves.size() < kAccelerateFrom) continue;

        Accelerator bvh;
        bvh.nodes.reserve(leaves.size() * 2);
        bvh.order.reserve(leaves.size());
        build_bvh(bvh, leaves, 0, leaves.size());
        accelerator_of_[i] = static_cast<u32>(accelerators_.size());
        accelerators_.push_back(std::move(bvh));
    }
}

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

        // The solids. Some of these are exact distances and some are bounds that under-state,
        // which is the safe direction: a reading that says "nearer than it really is" can only
        // make the sampler ask more often, never less.
        case Op::Sphere:
        case Op::Box:
        case Op::Cylinder:
        case Op::Capsule:
        case Op::Torus:
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

        // The same objection, about an angle rather than a coordinate, and without the tidy
        // bound: how far a sector is depends on how far out you are.
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

Vec3 Field::normal_at(u32 at, Vec3 p, f64 step) const {
    const f64 dx = eval(at, {p.x + step, p.y, p.z}) - eval(at, {p.x - step, p.y, p.z});
    const f64 dy = eval(at, {p.x, p.y + step, p.z}) - eval(at, {p.x, p.y - step, p.z});
    const f64 dz = eval(at, {p.x, p.y, p.z + step}) - eval(at, {p.x, p.y, p.z - step});
    return normalise({dx, dy, dz});
}

}  // namespace forge
}  // namespace ws
