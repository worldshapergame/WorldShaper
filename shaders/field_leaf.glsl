// The leaves: every op with no children, evaluated directly. `Field::eval`'s leaf cases.
//
// See field_types.glsl for what this file is and the rule that governs it — when this and
// `src/forge/field.cpp` disagree, field.cpp is right.
//
// Twenty-nine ops, and they are exactly the set `mirror_is_leaf` returns true for, because that
// set is what the walk hands down here. Everything below is `Field::eval` and the helpers it
// calls, transliterated line for line and in the same order. Where an expression looks redundant
// it has been LEFT redundant: the two evaluators disagreeing by one voxel is the fault this whole
// arrangement exists to catch, and a line tidied on the way across is a disagreement nobody wrote
// down and nobody can bisect.
//
// # The node index is not checked here, and that is deliberate
//
// `Field::eval` opens with `if (at >= nodes_.size()) return 1e30;` and this does not, because the
// WALK owns that question and answers it differently: `mirror_eval`'s `push` refuses outright
// rather than handing back a distance. Repeating the check here would make a bad index a shape a
// long way away instead of a refusal, which is the one failure D676 is about — a refusal that
// reads as an answer.
//
// # Two things the card does not have, and what stands in for them
//
// `std::hypot` is not a GLSL function. Every one below is `sqrt(x*x + y*y)`, which is the same
// number to within an ulp: the CPU's hypot is chosen for not overflowing at 1e150, and nothing at
// metre scale is near that.
//
// `f64` is not what a card is fast at, so every line here is `float`. D676 measured that and
// allowed it — the estate's isosurface walked in single precision gives 0 sign changes over
// 44,084 points, worst difference 0.47 µm against a 31.25 mm voxel.
//
// **The one place that is NOT allowed to lose precision is the hashing.** `wsl_hash_u32` is `uint`
// arithmetic from end to end, and every cell index reaches it as an `int` taken straight from a
// `floor` and never through a `float`. A cell index that has lost a low bit is not a slightly
// wrong grain; it is a different grain, on one machine and not the other, and the promise the
// noise makes is that the same clip file builds the same clip everywhere.

#ifndef WS_FIELD_LEAF_GLSL
#define WS_FIELD_LEAF_GLSL

#include "field_types.glsl"

// ---------------------------------------------------------------------------------------------
// The small arithmetic. Written out rather than reached for as builtins wherever the CPU writes
// it out, so the sums happen in the order the CPU sums them in.
// ---------------------------------------------------------------------------------------------

float wsl_length(vec3 v) { return sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }

float wsl_dot(vec3 a, vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

// `std::hypot`, to the accuracy this needs and no further. See the header.
float wsl_hypot(float x, float y) { return sqrt(x * x + y * y); }

// `forge::normalise`, fallback included: a zero vector comes back as up rather than as NaN.
// Nothing below ever hands it one — every Platonic normal is a real corner of a real solid — but
// the CPU's answer at zero is (0,1,0), and a transliteration that differs anywhere is one somebody
// has to think about twice.
vec3 wsl_normalise(vec3 v) {
    const float len = wsl_length(v);
    return (len > 0.0) ? vec3(v.x / len, v.y / len, v.z / len) : vec3(0.0, 1.0, 0.0);
}

// ---------------------------------------------------------------------------------------------
// Deterministic value noise.
//
// Hash-based rather than table-based, so it needs no setup and no memory, and two machines agree
// exactly — which is the whole point, and is why this is the part of the file that stays in
// integers.
// ---------------------------------------------------------------------------------------------

uint wsl_hash_u32(uint x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

// The CPU casts an `i64` cell index down to `u32` before multiplying, and that WRAP is part of
// what the hash is — not an accident of the type. GLSL's `int` is the same two's complement
// pattern, so `uint(xi)` here is `static_cast<u32>(i64)` there for every index a world can reach,
// and the addition of a corner offset wraps the same way in 32 bits as it does in 64 then 32.
float wsl_hash_to_unit(int xi, int yi, int zi, uint seed) {
    const uint h = wsl_hash_u32(uint(xi) * 0x8da6b343u ^
                                uint(yi) * 0xd8163841u ^
                                uint(zi) * 0xcb1ab31fu ^ seed);
    return float(h) * (1.0 / 4294967296.0);
}

float wsl_smootherstep(float t) { return t * t * t * (t * (t * 6.0 - 15.0) + 10.0); }

// Value noise in [-1, 1], with a feature size in metres.
float wsl_value_noise(vec3 p, float size, uint seed) {
    if (size <= 0.0) size = 1.0;
    const vec3 q = vec3(p.x / size, p.y / size, p.z / size);
    const float fx = floor(q.x), fy = floor(q.y), fz = floor(q.z);
    // Integers, taken from the floor rather than carried as floats: see the header.
    const int xi = int(fx), yi = int(fy), zi = int(fz);
    const float tx = wsl_smootherstep(q.x - fx);
    const float ty = wsl_smootherstep(q.y - fy);
    const float tz = wsl_smootherstep(q.z - fz);

    float accum = 0.0;
    for (int dz = 0; dz < 2; ++dz) {
        for (int dy = 0; dy < 2; ++dy) {
            for (int dx = 0; dx < 2; ++dx) {
                const float wx = (dx != 0) ? tx : (1.0 - tx);
                const float wy = (dy != 0) ? ty : (1.0 - ty);
                const float wz = (dz != 0) ? tz : (1.0 - tz);
                accum += wx * wy * wz * wsl_hash_to_unit(xi + dx, yi + dy, zi + dz, seed);
            }
        }
    }
    return accum * 2.0 - 1.0;
}

float wsl_fbm_noise(vec3 p, float size, uint octaves, float gain, float lacunarity, uint seed) {
    if (octaves == 0u) return 0.0;
    float sum = 0.0;
    float amplitude = 1.0;
    float total = 0.0;
    float current = size;
    for (uint i = 0u; i < octaves; ++i) {
        // The per-octave seed step is `uint` arithmetic and is allowed to wrap, exactly as it does
        // on the CPU — it is a decorrelation of one hash from the next, not a count.
        sum += amplitude * wsl_value_noise(p, current, seed + i * 131u);
        total += amplitude;
        amplitude *= gain;
        current /= (lacunarity > 0.0) ? lacunarity : 2.0;
    }
    return (total > 0.0) ? sum / total : 0.0;
}

// Distance to the nearest of one scattered point per cell, and to the second nearest.
//
// The nearest alone gives grains, cobbles and crystals. The *difference* between the two gives the
// seams between them, and a seam is what a crack is: a thin branching network that meets itself at
// junctions and never simply stops.
void wsl_cell_noise(vec3 p, float size, uint seed, out float nearest, out float second) {
    if (size <= 0.0) size = 1.0;
    const vec3 q = vec3(p.x / size, p.y / size, p.z / size);
    const float fx = floor(q.x), fy = floor(q.y), fz = floor(q.z);
    float best = 1e30;
    float next = 1e30;
    for (int dz = -1; dz <= 1; ++dz) {
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                const int cx = int(fx) + dx;
                const int cy = int(fy) + dy;
                const int cz = int(fz) + dz;
                const vec3 jitter = vec3(wsl_hash_to_unit(cx, cy, cz, seed),
                                         wsl_hash_to_unit(cx, cy, cz, seed + 7919u),
                                         wsl_hash_to_unit(cx, cy, cz, seed + 104729u));
                const vec3 site = vec3(float(cx) + jitter.x, float(cy) + jitter.y,
                                       float(cz) + jitter.z);
                const float d = wsl_length(q - site);
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

// ---------------------------------------------------------------------------------------------
// The exact distance functions.
// ---------------------------------------------------------------------------------------------

// `half` is a reserved word in GLSL, so the C++ parameter of that name is `half_extent` here and
// everywhere below. Nothing else about these differs.
float wsl_sd_box(vec3 p, vec3 half_extent) {
    const vec3 q = vec3(abs(p.x) - half_extent.x, abs(p.y) - half_extent.y,
                        abs(p.z) - half_extent.z);
    const vec3 outside = vec3(max(q.x, 0.0), max(q.y, 0.0), max(q.z, 0.0));
    const float inside = min(max(q.x, max(q.y, q.z)), 0.0);
    return wsl_length(outside) + inside;
}

float wsl_sd_cylinder(vec3 p, float r, float half_height, uint axis) {
    uint a = 0u, b = 0u;
    ws_other_axes(axis, a, b);
    const float radial = wsl_hypot(ws_axis_of(p, a), ws_axis_of(p, b)) - r;
    const float along = abs(ws_axis_of(p, axis)) - half_height;
    const float outside = wsl_hypot(max(radial, 0.0), max(along, 0.0));
    return min(max(radial, along), 0.0) + outside;
}

float wsl_sd_capsule(vec3 p, vec3 a, vec3 b, float r) {
    const vec3 pa = p - a;
    const vec3 ba = b - a;
    const float denom = wsl_dot(ba, ba);
    const float h = (denom > 0.0) ? clamp(wsl_dot(pa, ba) / denom, 0.0, 1.0) : 0.0;
    return wsl_length(pa - ba * h) - r;
}

float wsl_sd_torus(vec3 p, float ring, float tube, uint axis) {
    uint a = 0u, b = 0u;
    ws_other_axes(axis, a, b);
    const float radial = wsl_hypot(ws_axis_of(p, a), ws_axis_of(p, b)) - ring;
    return wsl_hypot(radial, ws_axis_of(p, axis)) - tube;
}

// --- part of the way round ---------------------------------------------------------------------
//
// `Arc` is the only sweep that is a leaf — `Revolve` and `PolarRepeat` have children and belong to
// the walk — but it measures its angle the same way they do: zero along the FIRST cross-axis in
// `ws_other_axes` order, increasing toward the second, which is the sense `atan(y, x)` already
// grows in.

// Whether a stored width is a real arc rather than the whole turn. A width of exactly one is the
// whole turn, and this is tested before anything else, so a clip that never writes `from=` pays
// not one extra multiply for the feature.
bool wsl_is_partial_sweep(float span) { return span > 0.0 && span < 1.0; }

// A turn folded into [0, 1). Subtracting a floor rather than taking a remainder, because a
// remainder keeps the sign of what it was given.
float wsl_wrap_turn(float t) {
    const float w = t - floor(t);
    return (w < 1.0) ? w : 0.0;   // a tiny negative t can round up to exactly 1.0
}

// Which end of an arc a point outside it is nearer to, as a signed offset in turns: positive when
// the point lies past `to`, negative when it lies short of `from`.
//
// The nearer end by ANGLE is the nearer end by DISTANCE, and that is what makes clamping to it
// exact rather than plausible — two points sharing a radius and a height are further apart the
// further apart their angles are, the whole way from nought to half a turn.
float wsl_nearer_end(float rel, float span) {
    const float past_end = rel - span;        // beyond `to`, going the way the sweep runs
    const float before_start = 1.0 - rel;     // short of `from`, the same way round the seam
    return (past_end <= before_start) ? past_end : -before_start;
}

float wsl_sd_cone(vec3 p, float base_r, float height, uint axis) {
    // Measured from the base, growing along the axis. Written as the intersection of the side and
    // the two caps, which is exact and needs no special case for a degenerate tip.
    uint a = 0u, b = 0u;
    ws_other_axes(axis, a, b);
    const float radial = wsl_hypot(ws_axis_of(p, a), ws_axis_of(p, b));
    const float along = ws_axis_of(p, axis);
    const float slope = wsl_hypot(height, base_r);
    const float side = (radial * height + along * base_r - base_r * height) /
                       ((slope > 0.0) ? slope : 1.0);
    const float bottom = -along;
    const float top = along - height;
    return max(side, max(bottom, top));
}

float wsl_sd_ellipsoid(vec3 p, vec3 r) {
    // The standard bounded approximation. Exact distance to an ellipsoid has no closed form; this
    // is correct in sign everywhere and accurate near the surface, which is what a voxel sampler
    // and a displacement need.
    const vec3 safe = vec3((r.x > 0.0) ? r.x : 1e-9, (r.y > 0.0) ? r.y : 1e-9,
                           (r.z > 0.0) ? r.z : 1e-9);
    const float k0 = wsl_length(vec3(p.x / safe.x, p.y / safe.y, p.z / safe.z));
    const float k1 = wsl_length(vec3(p.x / (safe.x * safe.x), p.y / (safe.y * safe.y),
                                     p.z / (safe.z * safe.z)));
    if (k1 == 0.0) return -min(safe.x, min(safe.y, safe.z));
    return k0 * (k0 - 1.0) / k1;
}

// A regular n-gon prism, as the intersection of n half planes. Exact, and it gives every polygonal
// cross-section from a triangle upwards from one node.
float wsl_sd_prism(vec3 p, float circumradius, float half_height, uint sides, uint axis,
                   float turn) {
    if (sides < 3u) sides = 3u;
    uint a = 0u, b = 0u;
    ws_other_axes(axis, a, b);
    const float u = ws_axis_of(p, a);
    const float v = ws_axis_of(p, b);
    // The apothem: a circumradius is the distance to a CORNER, which is what an author means by "a
    // hexagon this big", so the faces sit closer in by cos(pi/n).
    const float apothem = circumradius * cos(WS_PI / float(sides));
    // `cross` is a GLSL builtin, so the C++ local of that name is `cross_d`.
    float cross_d = -1e30;
    for (uint i = 0u; i < sides; ++i) {
        const float angle = WS_TAU * (float(i) / float(sides) + turn);
        cross_d = max(cross_d, u * cos(angle) + v * sin(angle) - apothem);
    }
    const float along = abs(ws_axis_of(p, axis)) - half_height;
    const float outside = wsl_hypot(max(cross_d, 0.0), max(along, 0.0));
    return min(max(cross_d, along), 0.0) + outside;
}

// The five Platonic solids, each as the intersection of its face planes.
//
// One mechanism for all five, because that is what they are: a set of unit normals and a distance.
// The distance is scaled so the solid's circumradius — the distance to a *vertex*, which is the
// size an author means — is the number asked for.
//
// An intersection of half spaces is exact inside the solid and an UNDER-estimate outside it,
// because near a corner the nearest face plane is further away than the corner itself. That is the
// safe direction and it is invisible to the sampler, which only reads the sign; it is visible to
// `round` and `shell`, which is why the cube is special-cased to a box rather than written as six
// planes.
float wsl_sd_platonic(vec3 p, float circumradius, uint which) {
    const float phi = (1.0 + sqrt(5.0)) * 0.5;
    // Twenty, because that is how many faces an icosahedron has and it is the largest of the five.
    // Sized at sixteen first on the CPU, which is the count for none of them and overran the moment
    // an icosahedron was asked for.
    vec3 normals[20];
    uint count = 0u;
    float face_over_circum = 1.0;   // inradius / circumradius, so the size means the vertex

    if (which == 0u) {          // tetrahedron
        vec3 n[4] = vec3[4](vec3(1, 1, 1), vec3(1, -1, -1), vec3(-1, 1, -1), vec3(-1, -1, 1));
        for (uint i = 0u; i < 4u; ++i) {
            normals[count++] = wsl_normalise(vec3(-n[i].x, -n[i].y, -n[i].z));
        }
        face_over_circum = 1.0 / 3.0;
    } else if (which == 1u) {   // cube — handled exactly below rather than as six planes
        return wsl_sd_box(p, vec3(circumradius / sqrt(3.0), circumradius / sqrt(3.0),
                                  circumradius / sqrt(3.0)));
    } else if (which == 2u) {   // octahedron
        for (int sx = -1; sx <= 1; sx += 2) {
            for (int sy = -1; sy <= 1; sy += 2) {
                for (int sz = -1; sz <= 1; sz += 2) {
                    normals[count++] = wsl_normalise(vec3(float(sx), float(sy), float(sz)));
                }
            }
        }
        face_over_circum = 1.0 / sqrt(3.0);
    } else if (which == 3u) {   // dodecahedron: twelve faces, normals along the icosahedron's vertices
        vec3 base[6] = vec3[6](vec3(0, 1, phi), vec3(0, 1, -phi), vec3(1, phi, 0),
                               vec3(1, -phi, 0), vec3(phi, 0, 1), vec3(-phi, 0, 1));
        for (uint i = 0u; i < 6u; ++i) {
            normals[count++] = wsl_normalise(base[i]);
            normals[count++] = wsl_normalise(vec3(-base[i].x, -base[i].y, -base[i].z));
        }
        face_over_circum = 0.7947;
    } else {                    // icosahedron: twenty faces, normals along the dodecahedron's vertices
        vec3 base[10] = vec3[10](vec3(1, 1, 1), vec3(1, 1, -1), vec3(1, -1, 1), vec3(1, -1, -1),
                                 vec3(0, 1.0 / phi, phi), vec3(0, 1.0 / phi, -phi),
                                 vec3(1.0 / phi, phi, 0), vec3(-1.0 / phi, phi, 0),
                                 vec3(phi, 0, 1.0 / phi), vec3(phi, 0, -1.0 / phi));
        for (uint i = 0u; i < 10u; ++i) {
            normals[count++] = wsl_normalise(base[i]);
            normals[count++] = wsl_normalise(vec3(-base[i].x, -base[i].y, -base[i].z));
        }
        face_over_circum = 0.7947;
    }

    const float inradius = circumradius * face_over_circum;
    float d = -1e30;
    for (uint i = 0u; i < count; ++i) d = max(d, wsl_dot(p, normals[i]) - inradius);
    return d;
}

// A ramp: a box cut by the diagonal plane that rises along one axis as another advances.
float wsl_sd_wedge(vec3 p, vec3 half_extent, uint rise_axis, uint run_axis) {
    const float box = wsl_sd_box(p, half_extent);
    const float rise = ws_axis_of(half_extent, rise_axis);
    const float run = ws_axis_of(half_extent, run_axis);
    if (rise <= 0.0 || run <= 0.0) return box;
    // The plane through (-run, -rise) and (+run, +rise), normalised so the result stays a distance
    // rather than merely having the right sign.
    const float u = ws_axis_of(p, run_axis);
    const float v = ws_axis_of(p, rise_axis);
    const float nx = rise, ny = -run;
    const float inv = 1.0 / wsl_hypot(nx, ny);
    return max(box, (u * nx + v * ny) * inv);
}

// A flight of steps, as the ramp it approximates minus the treads' overhang. Written by folding the
// run into one step, which makes it O(1) rather than one node per step and means a staircase of any
// length costs the same.
float wsl_sd_stairs(vec3 p, vec3 half_extent, float run, float rise, uint run_axis,
                    uint rise_axis) {
    if (run <= 0.0 || rise <= 0.0) return wsl_sd_box(p, half_extent);
    // Measured from the bottom of the flight rather than from its middle.
    const float u = ws_axis_of(p, run_axis) + ws_axis_of(half_extent, run_axis);
    const float v = ws_axis_of(p, rise_axis) + ws_axis_of(half_extent, rise_axis);

    const float tread = floor(u / run);
    const float top = (tread + 1.0) * rise;
    // Inside the flight's box, and below the top of the step this point stands on.
    const float box = wsl_sd_box(p, half_extent);
    return max(box, v - top);
}

// How many straight pieces a spiral of this many turns is cut into.
//
// Twenty-four to the turn, which puts the gap between a chord and its arc under a thousandth of the
// radius — a tenth of a voxel on a volute the size of a dinner plate, at the finest resolution this
// engine samples. Capped, because the cost is linear in it.
uint wsl_spiral_pieces(float turns) {
    const float wanted = abs(turns) * 24.0;
    if (wanted < 4.0) return 4u;
    if (wanted > 320.0) return 320u;
    return uint(wanted + 0.5);
}

// The distance to a logarithmic spiral swept as a round tube.
//
// Walked as a chain of straight pieces rather than solved: r = a·k^θ has no closed form for the
// nearest point, and every approximation of one either over-states the distance somewhere — which
// lets a sampler skip over the shape — or is slower than this. A chain of capsules has an exact
// distance, so what comes back is the true distance to the thing that actually gets built.
//
// The pieces are stepped round by one complex multiply each rather than by a fresh cosine, so a
// spiral of a hundred pieces costs one pair of trigonometric calls and a hundred multiplies.
float wsl_sd_spiral(vec3 p, float start_r, float per_turn, float tube, float turns, uint axis) {
    if (start_r <= 0.0 || turns <= 0.0) return 1e30;
    uint u = 0u, v = 0u;
    ws_other_axes(axis, u, v);
    const float px = ws_axis_of(p, u);
    const float py = ws_axis_of(p, v);
    const float along = ws_axis_of(p, axis);

    const uint pieces = wsl_spiral_pieces(turns);
    // `step` is a GLSL builtin, so the C++ local of that name is `step_angle`.
    const float step_angle = WS_TAU * turns / float(pieces);
    const float c = cos(step_angle), s = sin(step_angle);
    // The radius is multiplied by `per_turn` over a whole turn, so by this much over one piece.
    const float grow = pow((per_turn > 0.0) ? per_turn : 1.0, turns / float(pieces));

    float dirx = 1.0, diry = 0.0;
    float radius = start_r;
    float ax = radius, ay = 0.0;
    float best = 1e30;
    for (uint i = 0u; i < pieces; ++i) {
        const float ndirx = dirx * c - diry * s;
        const float ndiry = dirx * s + diry * c;
        radius *= grow;
        const float bx = ndirx * radius, by = ndiry * radius;

        // Point to segment, in the plane; the height off the plane is carried through as the third
        // component, which is what makes the sweep a tube rather than a ribbon.
        const float ex = bx - ax, ey = by - ay;
        const float wx = px - ax, wy = py - ay;
        const float denom = ex * ex + ey * ey;
        const float t = (denom > 0.0) ? clamp((wx * ex + wy * ey) / denom, 0.0, 1.0) : 0.0;
        const float dx = wx - ex * t, dy = wy - ey * t;
        const float d = sqrt(dx * dx + dy * dy + along * along);
        if (d < best) best = d;

        dirx = ndirx; diry = ndiry;
        ax = bx; ay = by;
    }
    return best - tube;
}

// ---------------------------------------------------------------------------------------------
// The leaves themselves. `Field::eval`'s switch, in its own order.
// ---------------------------------------------------------------------------------------------

float field_leaf(uint at, vec3 p) {
    const uint op = field_nodes.items[at].op;
    // `const f64* a = n.a;`, so every case below reads as its C++ twin does. Eight scalar loads out
    // of a readonly buffer, which the compiler is free to sink into whichever branch uses them.
    float a[8];
    for (int i = 0; i < 8; ++i) a[i] = field_nodes.items[at].a[i];

    switch (op) {
        case WS_OP_CONSTANT: return a[0];
        case WS_OP_PARAMETER: {
            // A slot past the end of the table is nought, not an error: a field asked about a
            // parameter it does not have answers zero, and the CPU does the same.
            const uint slot = uint(a[0]);
            return (slot < uint(field_params.items.length())) ? field_params.items[slot] : 0.0;
        }
        case WS_OP_COORDINATE: return ws_axis_of(p, uint(a[0]));
        case WS_OP_RADIUS: return wsl_length(p - vec3(a[0], a[1], a[2]));

        case WS_OP_SPHERE: return wsl_length(p - vec3(a[0], a[1], a[2])) - a[3];
        case WS_OP_BOX: {
            // The corner radius comes OFF the half extent on the way in and back off the answer on
            // the way out, which is what makes a rounded box a rounded box of the size asked for
            // rather than an inset one.
            const float d = wsl_sd_box(p - vec3(a[0], a[1], a[2]),
                                       vec3(a[3] - a[6], a[4] - a[6], a[5] - a[6]));
            return d - a[6];
        }
        case WS_OP_CYLINDER:
            return wsl_sd_cylinder(p - vec3(a[0], a[1], a[2]), a[3], a[4], uint(a[5]));
        case WS_OP_CAPSULE:
            // The point is NOT offset here, and that is not an omission: a capsule is given its two
            // ends outright, so there is no centre to subtract.
            return wsl_sd_capsule(p, vec3(a[0], a[1], a[2]), vec3(a[3], a[4], a[5]), a[6]);
        case WS_OP_TORUS:
            return wsl_sd_torus(p - vec3(a[0], a[1], a[2]), a[3], a[4], uint(a[5]));
        case WS_OP_ARC: {
            const vec3 q = p - vec3(a[0], a[1], a[2]);
            const uint axis = uint(a[5]);
            if (!wsl_is_partial_sweep(a[7])) return wsl_sd_torus(q, a[3], a[4], axis);
            uint u = 0u, v = 0u;
            ws_other_axes(axis, u, v);
            const float x = ws_axis_of(q, u), y = ws_axis_of(q, v);
            const float rel = wsl_wrap_turn(ws_atan2(y, x) / WS_TAU - a[6]);
            // Within the arc the nearest point of the centre-line is at the asking point's own
            // angle, which is exactly what the torus already computes — so the same call answers it
            // and the two can never drift apart.
            if (rel <= a[7]) return wsl_sd_torus(q, a[3], a[4], axis);
            // Past an end, the nearest point of the centre-line IS that end, and the cap is round
            // because the segment is a swept sphere. So: the distance to one point, less the tube.
            const float turn = (a[6] + ((wsl_nearer_end(rel, a[7]) > 0.0) ? a[7] : 0.0)) * WS_TAU;
            const float ex = a[3] * cos(turn), ey = a[3] * sin(turn);
            return wsl_hypot(wsl_hypot(x - ex, y - ey), ws_axis_of(q, axis)) - a[4];
        }
        case WS_OP_CONE:
            return wsl_sd_cone(p - vec3(a[0], a[1], a[2]), a[3], a[4], uint(a[5]));
        case WS_OP_PLANE: return wsl_dot(p, vec3(a[0], a[1], a[2])) - a[3];
        case WS_OP_ELLIPSOID:
            return wsl_sd_ellipsoid(p - vec3(a[0], a[1], a[2]), vec3(a[3], a[4], a[5]));
        case WS_OP_PRISM:
            return wsl_sd_prism(p - vec3(a[0], a[1], a[2]), a[3], a[4], uint(a[5]), uint(a[6]),
                                a[7]);
        case WS_OP_PLATONIC:
            return wsl_sd_platonic(p - vec3(a[0], a[1], a[2]), a[3], uint(a[4]));
        case WS_OP_WEDGE:
            // a[6] is the RISE axis and a[7] the run, which is the order `sd_wedge` takes them in
            // and the opposite of the order they read in.
            return wsl_sd_wedge(p - vec3(a[0], a[1], a[2]), vec3(a[3], a[4], a[5]), uint(a[6]),
                                uint(a[7]));
        case WS_OP_STAIRS:
            // The two axes are fixed at the call site rather than stored — a flight runs along z
            // and rises along y — because a[6] and a[7] are already spent on the run and the rise.
            return wsl_sd_stairs(p - vec3(a[0], a[1], a[2]), vec3(a[3], a[4], a[5]), a[6], a[7],
                                 /*run*/ 2u, /*rise*/ 1u);

        case WS_OP_SPIRAL:
            return wsl_sd_spiral(p - vec3(a[0], a[1], a[2]), a[3], a[4], a[5], a[6], uint(a[7]));

        case WS_OP_SINE: {
            const float period = (a[1] != 0.0) ? a[1] : 1.0;
            return sin(WS_TAU * (ws_axis_of(p, uint(a[0])) / period + a[2]));
        }
        case WS_OP_WAVES: {
            const uint axis = uint(a[0]);
            uint u = 0u, v = 0u;
            ws_other_axes(axis, u, v);
            const float pa = (a[1] != 0.0) ? a[1] : 1.0;
            const float pb = (a[2] != 0.0) ? a[2] : 1.0;
            // One phase for both, so the crossing pattern keeps its diagonal registration.
            return sin(WS_TAU * (ws_axis_of(p, u) / pa + a[3])) *
                   sin(WS_TAU * (ws_axis_of(p, v) / pb + a[3]));
        }
        case WS_OP_NOISE:
            return wsl_value_noise(ws_stretched(p, a[2], a[3], a[4]), a[0], uint(a[1]));
        case WS_OP_FBM:
            return wsl_fbm_noise(ws_stretched(p, a[5], a[6], a[7]), a[0], uint(a[1]), a[2], a[3],
                                 uint(a[4]));
        case WS_OP_RIDGED: {
            const float v = wsl_fbm_noise(ws_stretched(p, a[5], a[6], a[7]), a[0], uint(a[1]),
                                          a[2], a[3], uint(a[4]));
            return 1.0 - 2.0 * abs(v);
        }
        case WS_OP_RASP: {
            // Ridges an order finer than the surface they sit on, which is what a filed or
            // scratched face looks like: many shallow parallel gouges rather than lumps. The three
            // octaves, the gain and the lacunarity are FIXED here rather than stored, because the
            // eight slots are already spent on the size, the depth, the seed and the stretch.
            const float v = wsl_fbm_noise(ws_stretched(p, a[3], a[4], a[5]), a[0], 3u, 0.5, 2.7,
                                          uint(a[2]));
            return -abs(v) * a[1];
        }
        case WS_OP_CELLS: {
            float nearest = 0.0, second = 0.0;
            wsl_cell_noise(ws_stretched(p, a[2], a[3], a[4]), a[0], uint(a[1]), nearest, second);
            return nearest;
        }
        case WS_OP_CELL_EDGE: {
            float nearest = 0.0, second = 0.0;
            wsl_cell_noise(ws_stretched(p, a[2], a[3], a[4]), a[0], uint(a[1]), nearest, second);
            return second - nearest;   // zero on a seam, growing towards a cell's middle
        }

        case WS_OP_CHECKER: {
            float sum = 0.0;
            for (uint axis = 0u; axis < 3u; ++axis) {
                const float cell = a[axis];
                if (cell <= 0.0) continue;   // an axis with no cell size does not alternate
                sum += floor(ws_axis_of(p, axis) / cell);
            }
            // `mod` stands in for `std::fmod`, and the two agree here because the argument is
            // already non-negative — which is the only case where they can differ.
            return (mod(abs(sum), 2.0) < 1.0) ? -1.0 : 1.0;
        }
        case WS_OP_STRIPES: {
            const float period = (a[1] != 0.0) ? a[1] : 1.0;
            float t = ws_axis_of(p, uint(a[0])) / period;
            t -= floor(t);
            return (t < a[2]) ? -1.0 : 1.0;
        }
        case WS_OP_BRICKS: {
            // Running bond: every other course offset by half a brick, and the value is how deep
            // into the mortar this point is — negative on a brick face, positive in a joint. So it
            // can carve the joints or colour them without any further work.
            const uint face = uint(a[4]);
            uint u = 0u, v = 0u;
            ws_other_axes(face, u, v);
            const float course_height = (a[1] != 0.0) ? a[1] : 1.0;
            const float length_ = (a[0] != 0.0) ? a[0] : 1.0;
            const float course = floor(ws_axis_of(p, v) / course_height);
            const float shift = (mod(abs(course), 2.0) < 1.0) ? 0.0 : 0.5;
            float along = ws_axis_of(p, u) / length_ + shift;
            along -= floor(along);
            float up = ws_axis_of(p, v) / course_height;
            up -= floor(up);
            const float joint_u = a[3] / (2.0 * length_);
            const float joint_v = a[3] / (2.0 * course_height);
            // Distance into the brick from the nearest joint, on both axes.
            const float du = min(along, 1.0 - along) - joint_u;
            const float dv = min(up, 1.0 - up) - joint_v;
            return -min(du, dv);
        }

        default: break;
    }
    // `Field::eval`'s own fall-through, and the same value: an op this does not know is answered
    // "a long way away" rather than "here". Unreachable while the walk only sends leaves down.
    return 1.0e30;
}

#endif   // WS_FIELD_LEAF_GLSL
