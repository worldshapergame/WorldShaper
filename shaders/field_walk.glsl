// The walk: one loop over an explicit stack, over every op that has children.
// `Field::mirror_eval`, transliterated.
//
// See field_types.glsl for what this file is and the rule that governs it — when this and
// `src/forge/field.cpp` disagree, field.cpp is right.
//
// # The shape of it, and the one thing that is not obvious
//
// One frame per node on the current path; `step` says how far through that node's work the frame
// is; `ret` carries what the child that just finished came back with. `WS_PUSH` is the only way
// down and `WS_FINISH` the only way up, so a case that forgets to do one of them cannot silently
// hand back the previous node's answer — it loops, which a test catches.
//
// **`step` counts SAMPLE POINTS, not children**, and that is the whole reason this file is worth
// writing carefully (D643). `curvature` asks its child seven times, `occlusion` fourteen and
// `facing` six, each at a different point; `repeat` and `scatter` ask theirs up to eight times for
// the leaning neighbours. All five share the one mechanism, and none of them is a push-the-children
// loop wearing a different hat.
//
// # What is deliberately not here
//
// The leaf ops. A node `ws_walk_is_leaf` accepts is handed to `field_leaf`, which is the other
// half of the evaluator and fails in a completely different way — see field_types.glsl.
//
// The accelerator. `Field::eval` answers a wide union out of a BVH it built over the children's
// boxes; that is an optimisation of the same min, and this walks the children instead. What it does
// keep is the box cull, which is not optional decoration: without it every point walks every node
// of a several-thousand-node tree.

#ifndef WS_FIELD_WALK_GLSL
#define WS_FIELD_WALK_GLSL

#include "field_types.glsl"

// ---------------------------------------------------------------------------------------------
// Local arithmetic.
//
// Everything defined here is prefixed `ws_walk_` rather than `ws_`, because field_leaf.glsl is
// compiled into the same shader and needs a hash and a clamp of its own. Two files defining one
// name is a link error rather than a wrong answer, but it is a link error nobody can act on
// without both files open. Anything both halves genuinely share belongs in field_types.glsl.
// ---------------------------------------------------------------------------------------------

// `clamp` as field.cpp's own `clamp` writes it, which is NOT GLSL's when the bounds are crossed:
// GLSL's is `min(max(v, lo), hi)` and answers `hi` for lo > hi, where this answers `lo`. A `clamp`
// node's bounds come out of a clip file, so crossed bounds are an author's typo rather than an
// impossibility, and the two evaluators must make the same sense of it.
float ws_walk_clamp(float v, float lo, float hi) { return (v < lo) ? lo : ((v > hi) ? hi : v); }

// `std::round`: halves away from zero. GLSL's `round` leaves the direction at exactly .5 to the
// implementation, which would put a repeat's cell index one out on the cell boundaries — a copy in
// the wrong place on precisely the planes where two cells meet, on some cards and not others.
// `floor(v + 0.5)` is not it either: for the largest float below 0.5 the add rounds up to one.
float ws_walk_round(float v) {
    const float m = abs(v);
    const float whole = floor(m);
    const float nearest = ((m - whole) >= 0.5) ? (whole + 1.0) : whole;
    return (v < 0.0) ? -nearest : nearest;
}

// The CPU says `std::hypot`, which is careful about overflow at magnitudes nothing in a clip
// reaches. This is the same number to within a rounding.
float ws_walk_hypot(float x, float y) { return length(vec2(x, y)); }

// Whether a stored width is a real arc rather than the whole turn.
bool ws_walk_is_partial_sweep(float span) { return span > 0.0 && span < 1.0; }

// A turn folded into [0, 1). `floor` rather than a modulo because a modulo keeps the sign.
float ws_walk_wrap_turn(float t) {
    const float w = t - floor(t);
    return (w < 1.0) ? w : 0.0;   // a tiny negative t can round up to exactly 1.0
}

// Which end of an arc a point outside it is nearer to, as a signed offset in turns: positive when
// the point lies past `to`, negative when it lies short of `from`. The nearer end by ANGLE is the
// nearer end by DISTANCE, which is what makes clamping to it exact rather than plausible.
float ws_walk_nearer_end(float rel, float span) {
    const float past_end = rel - span;      // beyond `to`, going the way the sweep runs
    const float before_start = 1.0 - rel;   // short of `from`, going the same way round the seam
    return (past_end <= before_start) ? past_end : -before_start;
}

// Which of a partial `around`'s copies a point belongs to, as the turn that copy stands at.
//
// n copies and n-1 gaps, first on `from` and last on `to` — see Op::PolarRepeat in field.hpp for
// why that rather than n gaps. A point in the gap outside the arc folds into whichever END copy is
// nearer, which invents nothing: that copy really does stand there.
float ws_walk_polar_copy_turn(float turn, float from, float span, uint count) {
    if (count <= 1u) return from;   // one copy, and it sits on `from`
    const float step = span / float(count - 1u);
    const float rel = ws_walk_wrap_turn(turn - from);
    if (rel <= span) {
        const float k = ws_walk_clamp(ws_walk_round(rel / step), 0.0, float(count - 1u));
        return from + k * step;
    }
    return (ws_walk_nearer_end(rel, span) > 0.0) ? (from + span) : from;
}

// The fourteen directions `occlusion` asks along: the six axes, then the eight diagonals in the
// order field.cpp lists them. Written as arithmetic rather than as a constant array because an
// array indexed by a varying step is scratch memory on a card, and the stack already costs enough.
vec3 ws_walk_occlusion_dir(uint which) {
    if (which < 6u) {
        // {1,0,0} {-1,0,0} {0,1,0} {0,-1,0} {0,0,1} {0,0,-1}
        return ws_with_axis(vec3(0.0), which >> 1u, ((which & 1u) == 0u) ? 1.0 : -1.0);
    }
    // The eight sign combinations of one over root three, x the high bit — which is the order
    // {k,k,k} {k,k,-k} {k,-k,k} ... {-k,-k,-k} is already in.
    const float k = 0.5773502691896258;
    const uint bits = which - 6u;
    return vec3(((bits & 4u) == 0u) ? k : -k,
                ((bits & 2u) == 0u) ? k : -k,
                ((bits & 1u) == 0u) ? k : -k);
}

// `normalise` in field.cpp, INCLUDING its answer for a zero vector, which is {0,1,0} and not
// {0,0,0}. Only `facing` reaches it here, and only where the field is flat enough that six samples
// cancel — a rule keyed on "up" then matches, which is what the CPU does too.
vec3 ws_walk_normalise(vec3 v) {
    const float len = length(v);
    return (len > 0.0) ? (v / len) : vec3(0.0, 1.0, 0.0);
}

// --- the hash a scatter draws its per-cell numbers from ----------------------------------------
//
// `hash_u32` and `hash_to_unit` in field.cpp. Keyed on the cell's INDEX and on nothing else, so the
// scatter is identical on every machine and does not shimmer when the clip is re-sampled.
uint ws_walk_hash_u32(uint x) {
    x ^= x >> 16u;
    x *= 0x7feb352du;
    x ^= x >> 15u;
    x *= 0x846ca68bu;
    x ^= x >> 16u;
    return x;
}

// The one place this file is knowingly coarser than the CPU: `float(h)` keeps 24 bits of a 32-bit
// hash where `double(h)` keeps all of them. It moves a jittered copy by about a nanometre against a
// 31.25 mm voxel, which is four orders below the f32 error D676 already measured and allowed.
float ws_walk_hash_to_unit(int xi, int yi, int zi, uint seed) {
    const uint h = ws_walk_hash_u32(uint(xi) * 0x8da6b343u ^ uint(yi) * 0xd8163841u ^
                                    uint(zi) * 0xcb1ab31fu ^ seed);
    return float(h) * (1.0 / 4294967296.0);
}

// Which axis a scattered copy spins about: the first with no period, and y when all three repeat.
// A gravel bed repeats in x and z so its pebbles turn about y; ivy on a wall repeats in x and y so
// its leaves turn about z. Chosen rather than given, because an author never writes it.
uint ws_walk_scatter_spin_axis(uint at) {
    for (uint axis = 0u; axis < 3u; ++axis) {
        if (field_nodes.items[at].a[axis] <= 0.0) return axis;
    }
    return 1u;
}

// Where a scattered copy's cell asks its child, and by how much the answer must be multiplied on
// the way back out. `scatter_point` in field.cpp.
//
// The forward transform on a copy is scale, then spin, then shift, then tile; this is its inverse
// applied in the opposite order. The scale comes back as a multiplier because a UNIFORM scale of s
// reports s * d(p / s) exactly — no approximation and nothing to allow for.
vec3 ws_walk_scatter_point(uint at, vec3 p, vec3 cell, out float scale) {
    const float jitter = ws_walk_clamp(field_nodes.items[at].a[6], 0.0, 1.0);
    const int cx = int(cell.x);
    const int cy = int(cell.y);
    const int cz = int(cell.z);

    vec3 q = p;
    for (uint axis = 0u; axis < 3u; ++axis) {
        const float period = field_nodes.items[at].a[axis];
        if (period <= 0.0) continue;   // no period, no cell, and so nothing to move it within
        const float shift =
            (jitter > 0.0)
                ? period * jitter * (ws_walk_hash_to_unit(cx, cy, cz, 0x51ed270bu + axis) - 0.5)
                : 0.0;
        q = ws_with_axis(q, axis, ws_axis_of(p, axis) - period * cell[axis] - shift);
    }

    const float spin_turns = field_nodes.items[at].a[7];
    const float turn = (spin_turns != 0.0)
                           ? spin_turns * (ws_walk_hash_to_unit(cx, cy, cz, 0x9e3779b9u) - 0.5) * 2.0
                           : 0.0;
    if (turn != 0.0) {
        const uint spin = ws_walk_scatter_spin_axis(at);
        uint u = 0u, v = 0u;
        ws_other_axes(spin, u, v);
        const float angle = -turn * WS_TAU;
        const float c = cos(angle), s = sin(angle);
        const float x = ws_axis_of(q, u), y = ws_axis_of(q, v);
        q = ws_with_axis(q, u, x * c - y * s);
        q = ws_with_axis(q, v, x * s + y * c);
    }

    // The same dial as the position: "how irregular", once, rather than three keys an author has to
    // balance. Never larger than one, so a copy never outgrows the box its bounds came from.
    scale = (jitter > 0.0) ? (1.0 - jitter * ws_walk_hash_to_unit(cx, cy, cz, 0x2545f491u)) : 1.0;
    if (scale <= 1.0e-6) scale = 1.0e-6;
    if (scale != 1.0) q = q / scale;
    return q;
}

// The profile's own plane, for `revolve`: a radius across the first cross-axis, the height along
// the axis itself, and the third coordinate nought. A negative radius is meaningful and is what the
// far side of an end cap asks for, so it is not clamped.
// (`flat`, which is what field.cpp calls this local, is a reserved interpolation qualifier here.)
vec3 ws_walk_profile_point(vec3 q, uint axis, uint cross, float radius) {
    vec3 in_plane = vec3(0.0);
    in_plane = ws_with_axis(in_plane, cross, radius);
    in_plane = ws_with_axis(in_plane, axis, ws_axis_of(q, axis));
    return in_plane;
}

// How far a point is from a revolve's end cap, given the profile's answer at the cap's own radius
// and the leg out of the cap's plane. The cap is a flat region, so the distance to it is the
// hypotenuse of the two. Exact.
float ws_walk_cap_away(float profile, float out_of_plane) {
    return ws_walk_hypot(max(profile, 0.0), out_of_plane);
}

// Whether an op is one `field_leaf` finishes. Exactly the set `mirror_is_leaf` returns true for,
// written against the op names so it can be read off against the enum: Constant through Stairs,
// then Spiral (Revolve sits between them and is this file's), then the eight grains, then the three
// tilings. Anything else here has children.
bool ws_walk_is_leaf(uint op) {
    return (op <= WS_OP_STAIRS) ||
           (op == WS_OP_SPIRAL) ||
           (op >= WS_OP_SINE && op <= WS_OP_CELL_EDGE) ||
           (op >= WS_OP_CHECKER && op <= WS_OP_BRICKS);
}

// What a one-child op does to its child's answer on the way out. `mirror_after`.
float ws_walk_after(uint at, uint op, float v) {
    if (op == WS_OP_SHELL) return abs(v) - field_nodes.items[at].a[0];
    if (op == WS_OP_ROUND) return v - field_nodes.items[at].a[0];
    if (op == WS_OP_OFFSET) return v + field_nodes.items[at].a[0];
    if (op == WS_OP_NEGATE) return -v;
    if (op == WS_OP_ABS) return abs(v);
    if (op == WS_OP_STEP) return (v > field_nodes.items[at].a[0]) ? 1.0 : 0.0;
    if (op == WS_OP_SMOOTHSTEP) {
        const float lo = field_nodes.items[at].a[0];
        const float span = field_nodes.items[at].a[1] - lo;
        if (span == 0.0) return (v > lo) ? 1.0 : 0.0;
        const float t = ws_walk_clamp((v - lo) / span, 0.0, 1.0);
        return t * t * (3.0 - 2.0 * t);
    }
    if (op == WS_OP_CLAMP) {
        return ws_walk_clamp(v, field_nodes.items[at].a[0], field_nodes.items[at].a[1]);
    }
    if (op == WS_OP_POWER) {
        return ((v < 0.0) ? -1.0 : 1.0) * pow(abs(v), field_nodes.items[at].a[0]);
    }
    if (op == WS_OP_REMAP) {
        const float lo = field_nodes.items[at].a[0];
        const float span = field_nodes.items[at].a[1] - lo;
        const float t = (span == 0.0) ? 0.0 : ws_walk_clamp((v - lo) / span, 0.0, 1.0);
        return field_nodes.items[at].a[2] +
               (field_nodes.items[at].a[3] - field_nodes.items[at].a[2]) * t;
    }
    return v;
}

// ...and how a many-child op folds the next child into the answer so far. `mirror_fold`.
float ws_walk_fold(uint at, uint op, float acc, float v) {
    const float k = field_nodes.items[at].a[0];   // the blend or the chamfer width
    if (op == WS_OP_UNION || op == WS_OP_MIN) return min(acc, v);
    if (op == WS_OP_INTERSECTION || op == WS_OP_MAX) return max(acc, v);
    if (op == WS_OP_DIFFERENCE) return max(acc, -v);
    if (op == WS_OP_SMOOTH_UNION) return ws_smooth_min(acc, v, k);
    if (op == WS_OP_SMOOTH_INTERSECTION) return ws_smooth_max(acc, v, k);
    if (op == WS_OP_SMOOTH_DIFFERENCE) return ws_smooth_max(acc, -v, k);
    if (op == WS_OP_CHAMFER_UNION) return ws_chamfer_min(acc, v, k);
    if (op == WS_OP_CHAMFER_INTERSECTION) return ws_chamfer_max(acc, v, k);
    if (op == WS_OP_CHAMFER_DIFFERENCE) return ws_chamfer_max(acc, -v, k);
    if (op == WS_OP_ADD) return acc + v;
    if (op == WS_OP_MULTIPLY) return acc * v;
    return v;
}

// ---------------------------------------------------------------------------------------------
// The walk.
// ---------------------------------------------------------------------------------------------

// The node's arguments and children, spelt short. `ni` is the frame's node, declared once at the
// top of the loop; these read straight out of the buffer rather than into a local array, because a
// per-invocation `float a[8]` on top of a 96-frame stack is scratch memory this cannot spare.
#define WS_A(i) field_nodes.items[ni].a[i]
#define WS_CHILD(i) field_nodes.items[ni].child[i]

// The only way down. Refuses rather than answering when it would run off the stack or off the node
// array: "I could not" and "the answer is nought" must never be the same reply (D676).
#define WS_PUSH(child_index, where)                                     \
    {                                                                   \
        const uint ws_child = (child_index);                            \
        if (ws_child >= node_count || top >= WS_FIELD_STACK) {          \
            /* 128u marks a refusal that is DEPTH, not a walk that would not end. */ \
            ws_field_refused_op = 128u;                                 \
            return WS_FIELD_REFUSED;                                    \
        }                                                               \
        stack[top].node = ws_child;                                     \
        stack[top].step = 0u;                                           \
        stack[top].p = (where);                                         \
        stack[top].acc = 0.0;                                           \
        stack[top].neighbours = 0u;                                     \
        stack[top].axes = 0u;                                           \
        stack[top].scale = 1.0;                                         \
        ++top;                                                          \
        continue;                                                       \
    }

// ...and the only way up.
#define WS_FINISH(value) \
    {                    \
        ret = (value);   \
        --top;           \
        continue;        \
    }

float field_eval(uint root, vec3 p) {
    const uint node_count = field_nodes.items.length();
    if (root >= node_count) {
        // 129 for "asked about a node that is not there". EVERY refusal path has to stamp its own
        // number here, and the reason is that the default value of this variable is nought, which
        // is `constant` -- a real op. A path that refuses without writing it reports a confident,
        // specific and entirely false culprit, and two of the three paths did exactly that for an
        // hour before this comment existed. Trap 7, in the one place that cannot assert.
        ws_field_refused_op = 129u;
        return WS_FIELD_REFUSED;
    }

    FieldFrame stack[WS_FIELD_STACK];
    uint top = 0u;
    float ret = 0.0;   // what the child that just finished came back with

    stack[0].node = root;
    stack[0].step = 0u;
    stack[0].p = p;
    stack[0].acc = 0.0;
    stack[0].neighbours = 0u;
    stack[0].axes = 0u;
    stack[0].scale = 1.0;
    top = 1u;

    uint turns = 0u;
    while (top > 0u) {
        // Bounded, and the bound is not paranoia -- see WS_FIELD_TURNS. A case that neither pushes
        // nor finishes is a dispatch that never returns, and that is a LOST DEVICE rather than a
        // wrong number.
        if (++turns > WS_FIELD_TURNS) {
            // The PARENT'S op, not the top frame's, and the difference is the whole point.
            //
            // A walk that will not end is one whose case pushes a child and then does not advance
            // `step`, so it pushes the same child again for ever. The frame on TOP when the count
            // trips is therefore the innocent child — usually a leaf, which provably cannot loop —
            // and reporting that one sends somebody to read the one op in the file that is not the
            // problem. The frame under it is the one that is not advancing.
            //
            // `ws_field_refused_op` also has to be SET here rather than left at its initial value:
            // nought is `constant`, which is a real op number, so a path that refuses without
            // writing this reports a confident, specific and completely false answer. That happened
            // once already, in the hour this line was written.
            ws_field_refused_op =
                field_nodes.items[stack[top >= 2u ? top - 2u : top - 1u].node].op;
            return WS_FIELD_REFUSED;
        }
        ++ws_field_visits;   // the instrument; see field_types.glsl
        const uint fi = top - 1u;
        const uint ni = stack[fi].node;
        const uint op = field_nodes.items[ni].op;
        const uint step = stack[fi].step;
        const vec3 fp = stack[fi].p;
        // A node holds four children at most — `Field::combine` folds a wider `union { a b c d e }`
        // into a chain rather than growing the record. Held to that here because an index past the
        // fourth is not a wrong number on a card, it is a read of whatever lies next in the buffer.
        const uint nc = min(field_nodes.items[ni].children, 4u);

        if (ws_walk_is_leaf(op)) {
            // A leaf has no children, so the recursive evaluator does not recurse on one either:
            // for these the two evaluators are the same code by construction, and what is being
            // proved here is the WALK.
            WS_FINISH(field_leaf(ni, fp));
        }

        switch (op) {
            // ---- one child, the point changed on the way in -------------------------------
            case WS_OP_REVOLVE: {
                // The point folded into the profile's half plane: how far it is from the axis, how
                // far along the axis it is, and nothing else. That fold is exactly what revolving
                // means, and it is why the answer is a real distance and not a bound — the nearest
                // point of a surface of revolution is always at the asking point's own angle.
                const uint axis = uint(WS_A(3));
                uint u = 0u, v = 0u;
                ws_other_axes(axis, u, v);
                const vec3 q = fp - vec3(WS_A(0), WS_A(1), WS_A(2));
                const float r = ws_walk_hypot(ws_axis_of(q, u), ws_axis_of(q, v));

                if (!ws_walk_is_partial_sweep(WS_A(5))) {
                    if (step == 0u) {
                        stack[fi].step = 1u;
                        WS_PUSH(WS_CHILD(0), ws_walk_profile_point(q, axis, u, r));
                    }
                    WS_FINISH(ret);
                }

                // Part of the way round. The recursive evaluator asks the profile once, twice or
                // three times depending on where the point stands, so this one carries a step per
                // ASK — the same mechanism `curvature` and `occlusion` use — and `lean.x` holds the
                // out-of-plane leg of whichever cap is being measured.
                const float rel =
                    ws_walk_wrap_turn(ws_atan2(ws_axis_of(q, v), ws_axis_of(q, u)) / WS_TAU - WS_A(4));

                // Which cap to ask about next, and the step to come back on. Worked out by the
                // branches below, then asked once at the bottom — the C++ has a lambda for this and
                // this is the same three lines, spelt out.
                float cap_delta = 0.0;
                uint cap_next = 0u;

                if (step == 0u) {
                    if (rel <= WS_A(5)) {
                        stack[fi].step = 1u;
                        WS_PUSH(WS_CHILD(0), ws_walk_profile_point(q, axis, u, r));
                    }
                    // Outside the wedge, and this is the whole trap. The honest distance here is to
                    // the END CAP, not to the surface of the full revolution — answer the latter
                    // and every normal near the cut is wrong while a slice through the shape still
                    // looks exactly right. One ask, at the nearer end.
                    cap_delta = ws_walk_nearer_end(rel, WS_A(5));
                    cap_next = 4u;
                } else if (step == 1u) {
                    // Outside the profile and inside the wedge: the nearest matter is at the
                    // point's own angle, so the profile's own answer is already the true distance.
                    if (ret >= 0.0) WS_FINISH(ret);
                    // Inside the solid the caps are surface too, and either may be nearer than the
                    // swept face. Magnitude is what normals are made of, so this is not optional.
                    stack[fi].acc = ret;
                    cap_delta = rel;
                    cap_next = 2u;
                } else if (step == 2u) {
                    // The first cap's distance, banked before `lean.x` is overwritten for the
                    // second. Order matters here and it is the C++'s order.
                    stack[fi].lean.y = ws_walk_cap_away(ret, stack[fi].lean.x);
                    cap_delta = rel - WS_A(5);
                    cap_next = 3u;
                } else if (step == 3u) {
                    WS_FINISH(max(stack[fi].acc,
                                  -min(stack[fi].lean.y,
                                       ws_walk_cap_away(ret, stack[fi].lean.x))));
                } else {
                    WS_FINISH(ws_walk_cap_away(ret, stack[fi].lean.x));
                }

                // Rotating the point into the cap's plane splits it into a distance measured IN
                // that plane and one perpendicular to it; the profile answers the first and
                // `lean.x` carries the second across the push.
                const float cap_turn = cap_delta * WS_TAU;
                stack[fi].lean.x = r * sin(cap_turn);
                stack[fi].step = cap_next;
                WS_PUSH(WS_CHILD(0), ws_walk_profile_point(q, axis, u, r * cos(cap_turn)));
            }

            case WS_OP_TRANSLATE: {
                if (step == 0u) {
                    stack[fi].step = 1u;
                    WS_PUSH(WS_CHILD(0), fp - vec3(WS_A(0), WS_A(1), WS_A(2)));
                }
                WS_FINISH(ret);
            }

            case WS_OP_ROTATE: {
                if (step == 0u) {
                    stack[fi].step = 1u;
                    // Applied backwards, because moving the shape one way is asking about the point
                    // the other. Euler xyz, in turns, because a quarter is a rounder thing to type.
                    const float cx = cos(-WS_A(0) * WS_TAU), sx = sin(-WS_A(0) * WS_TAU);
                    const float cy = cos(-WS_A(1) * WS_TAU), sy = sin(-WS_A(1) * WS_TAU);
                    const float cz = cos(-WS_A(2) * WS_TAU), sz = sin(-WS_A(2) * WS_TAU);
                    vec3 q = fp;
                    q = vec3(q.x, q.y * cx - q.z * sx, q.y * sx + q.z * cx);
                    q = vec3(q.x * cy + q.z * sy, q.y, -q.x * sy + q.z * cy);
                    q = vec3(q.x * cz - q.y * sz, q.x * sz + q.y * cz, q.z);
                    WS_PUSH(WS_CHILD(0), q);
                }
                WS_FINISH(ret);
            }

            case WS_OP_SCALE: {
                // A stored zero means one, so a node built before a factor existed reads unchanged.
                const vec3 s = vec3((WS_A(0) != 0.0) ? WS_A(0) : 1.0,
                                    (WS_A(1) != 0.0) ? WS_A(1) : 1.0,
                                    (WS_A(2) != 0.0) ? WS_A(2) : 1.0);
                if (step == 0u) {
                    stack[fi].step = 1u;
                    WS_PUSH(WS_CHILD(0), vec3(fp.x / s.x, fp.y / s.y, fp.z / s.z));
                }
                // ...and the smallest factor applied on the way OUT, which is the half of this op a
                // transliteration is most likely to drop. Scaled back by the smallest so the answer
                // never over-states the distance, which would let a march step through the surface.
                const float smallest = min(abs(s.x), min(abs(s.y), abs(s.z)));
                WS_FINISH(ret * smallest);
            }

            case WS_OP_MIRROR: {
                if (step == 0u) {
                    stack[fi].step = 1u;
                    const uint axis = uint(WS_A(0));
                    WS_PUSH(WS_CHILD(0), ws_with_axis(fp, axis, abs(ws_axis_of(fp, axis))));
                }
                WS_FINISH(ret);
            }

            case WS_OP_POLAR_REPEAT: {
                if (step == 0u) {
                    stack[fi].step = 1u;
                    const uint count = max(1u, uint(WS_A(0)));
                    const uint axis = uint(WS_A(1));
                    uint u = 0u, v = 0u;
                    ws_other_axes(axis, u, v);
                    const float x = ws_axis_of(fp, u), y = ws_axis_of(fp, v);
                    float angle = ws_atan2(y, x);
                    if (!ws_walk_is_partial_sweep(WS_A(3))) {
                        // A whole turn: n copies and n gaps, because the first and the last would
                        // otherwise land on top of one another.
                        const float sector = WS_TAU / float(count);
                        angle -= sector * ws_walk_round(angle / sector);
                    } else {
                        // An arc: n copies and n-1 gaps, first on `from` and last on `to`.
                        angle -= WS_TAU * ws_walk_polar_copy_turn(angle / WS_TAU, WS_A(2), WS_A(3),
                                                                  count);
                    }
                    const float r = ws_walk_hypot(x, y);
                    vec3 q = fp;
                    q = ws_with_axis(q, u, cos(angle) * r);
                    q = ws_with_axis(q, v, sin(angle) * r);
                    WS_PUSH(WS_CHILD(0), q);
                }
                WS_FINISH(ret);
            }

            case WS_OP_TWIST:
            case WS_OP_BEND: {
                // One block, because the two differ only in which coordinate drives the angle: a
                // twist turns about its axis by how far ALONG it you are, a bend by how far ACROSS.
                if (step == 0u) {
                    stack[fi].step = 1u;
                    const uint axis = uint(WS_A(1));
                    uint u = 0u, v = 0u;
                    ws_other_axes(axis, u, v);
                    const float along =
                        (op == WS_OP_TWIST) ? ws_axis_of(fp, axis) : ws_axis_of(fp, u);
                    const float angle = -WS_A(0) * WS_TAU * along;
                    const float c = cos(angle), sn = sin(angle);
                    const float x = ws_axis_of(fp, u), y = ws_axis_of(fp, v);
                    vec3 q = fp;
                    q = ws_with_axis(q, u, x * c - y * sn);
                    q = ws_with_axis(q, v, x * sn + y * c);
                    WS_PUSH(WS_CHILD(0), q);
                }
                WS_FINISH(ret);
            }

            // ---- one child, the ANSWER changed on the way out -----------------------------
            case WS_OP_SHELL:
            case WS_OP_ROUND:
            case WS_OP_OFFSET:
            case WS_OP_NEGATE:
            case WS_OP_ABS:
            case WS_OP_STEP:
            case WS_OP_SMOOTHSTEP:
            case WS_OP_CLAMP:
            case WS_OP_POWER:
            case WS_OP_REMAP:
            case WS_OP_CURVATURE:
            case WS_OP_OCCLUSION:
            case WS_OP_FACING: {
                // The three re-entrant ones share this block deliberately: they differ from the
                // rest only in how many times they ask and where, which is the step counter's job.
                if (op == WS_OP_CURVATURE) {
                    // The mean of the field around a point against the field at it. For a distance
                    // field that is its Laplacian, and the Laplacian of a distance field is
                    // curvature: outside a convex shape the neighbourhood is further away than the
                    // centre, inside a concave one it is nearer. `lean.x` holds the centre and
                    // `acc` the sum around it, because the answer needs both.
                    const float r = (WS_A(0) > 0.0) ? WS_A(0) : 0.05;
                    if (step == 1u) stack[fi].lean.x = ret;
                    else if (step > 1u) stack[fi].acc = stack[fi].acc + ret;
                    if (step == 7u) WS_FINISH((stack[fi].acc / 6.0 - stack[fi].lean.x) / r);

                    vec3 where = fp;   // step 0 is the centre, asked at the frame's own point
                    if (step == 1u) where.x += r;
                    else if (step == 2u) where.x -= r;
                    else if (step == 3u) where.y += r;
                    else if (step == 4u) where.y -= r;
                    else if (step == 5u) where.z += r;
                    else if (step == 6u) where.z -= r;
                    stack[fi].step = step + 1u;
                    WS_PUSH(WS_CHILD(0), where);
                }
                if (op == WS_OP_OCCLUSION) {
                    // How much of a small sphere around this point is solid. Fourteen FIXED
                    // directions rather than a random spray, because a weathering pattern that
                    // shimmers when the clip is re-sampled is not a pattern.
                    const float r = (WS_A(0) > 0.0) ? WS_A(0) : 0.15;
                    if (step > 0u && ret < 0.0) stack[fi].acc = stack[fi].acc + 1.0;
                    if (step == 14u) WS_FINISH(stack[fi].acc / 14.0);
                    const vec3 where = fp + ws_walk_occlusion_dir(step) * r;
                    stack[fi].step = step + 1u;
                    WS_PUSH(WS_CHILD(0), where);
                }
                if (op == WS_OP_FACING) {
                    const float reach = (WS_A(1) > 0.0) ? WS_A(1) : 0.02;
                    if (step > 0u) {
                        // +x -x +y -y +z -z, differenced in pairs exactly as `normal_at` does: the
                        // odd step banks the plus sample, the even one subtracts the minus.
                        const uint leg = (step - 1u) / 2u;
                        if ((step % 2u) == 1u) stack[fi].lean[leg] = ret;
                        else stack[fi].lean[leg] = stack[fi].lean[leg] - ret;
                    }
                    if (step == 6u) {
                        const vec3 normal = ws_walk_normalise(stack[fi].lean);
                        WS_FINISH(ws_axis_of(normal, uint(WS_A(0))));
                    }
                    vec3 where = fp;
                    // (`sign` here would shadow the built-in of that name.)
                    const float nudge = ((step % 2u) == 0u) ? reach : -reach;
                    if ((step / 2u) == 0u) where.x += nudge;
                    else if ((step / 2u) == 1u) where.y += nudge;
                    else where.z += nudge;
                    stack[fi].step = step + 1u;
                    WS_PUSH(WS_CHILD(0), where);
                }
                if (step == 0u) {
                    stack[fi].step = 1u;
                    WS_PUSH(WS_CHILD(0), fp);
                }
                WS_FINISH(ws_walk_after(ni, op, ret));
            }

            // ---- two children at the same point --------------------------------------------
            case WS_OP_DISPLACE: {
                if (step == 0u) {
                    stack[fi].step = 1u;
                    WS_PUSH(WS_CHILD(0), fp);
                }
                if (step == 1u) {
                    stack[fi].acc = ret;
                    stack[fi].step = 2u;
                    if (nc < 2u) WS_FINISH(stack[fi].acc);
                    WS_PUSH(WS_CHILD(1), fp);
                }
                WS_FINISH(stack[fi].acc + WS_A(0) * ret);
            }

            case WS_OP_BLEND: {
                if (step == 0u) {
                    stack[fi].step = 1u;
                    WS_PUSH(WS_CHILD(0), fp);
                }
                if (step == 1u) {
                    stack[fi].acc = ret;
                    stack[fi].step = 2u;
                    if (nc < 2u) WS_FINISH(stack[fi].acc);
                    WS_PUSH(WS_CHILD(1), fp);
                }
                // `mirror_eval` does not clamp the mix where `Field::eval` does. Followed as
                // written, because this file is the mirror's transliteration and the two agree
                // everywhere a builder made the node — `Field::blend` is the only way to build one
                // and its `t` reaches here unaltered.
                WS_FINISH(stack[fi].acc * (1.0 - WS_A(0)) + ret * WS_A(0));
            }

            // ---- every child at the same point, folded together ---------------------------
            case WS_OP_UNION:
            case WS_OP_INTERSECTION:
            case WS_OP_DIFFERENCE:
            case WS_OP_SMOOTH_UNION:
            case WS_OP_SMOOTH_INTERSECTION:
            case WS_OP_SMOOTH_DIFFERENCE:
            case WS_OP_CHAMFER_UNION:
            case WS_OP_CHAMFER_INTERSECTION:
            case WS_OP_CHAMFER_DIFFERENCE:
            case WS_OP_ADD:
            case WS_OP_MULTIPLY:
            case WS_OP_MIN:
            case WS_OP_MAX: {
                uint next = step;
                if (step > 0u) {
                    const float acc =
                        (step == 1u) ? ret : ws_walk_fold(ni, op, stack[fi].acc, ret);
                    stack[fi].acc = acc;
                    // `multiply` stops at the first factor that is nought, and the mirror has to
                    // stop in the same place or it is a different amount of work for the same
                    // answer — the kind of difference that only ever shows up as a timing. It is
                    // exact: zero times anything finite is zero, and nothing here has side effects.
                    if (op == WS_OP_MULTIPLY && acc == 0.0) WS_FINISH(0.0);

                    // The box cull, and it is where the time goes on a clip of any size. A union of
                    // thirty parts costs thirty evaluations at every point, and at all but a
                    // handful of them twenty-nine are answering about something metres away.
                    //
                    // Two halves of the condition, both from `Field::eval`, and the second is the
                    // one that is easy to get wrong. STRICTLY OUTSIDE the box: a point inside a
                    // child's box has a box distance of nought, and nought is not a lower bound on
                    // what that child will say — a shape you are inside reports a negative
                    // distance. And then either already inside something, so nothing outside can be
                    // nearer, or nearer than the box can possibly be.
                    //
                    // Getting it wrong is not loud. The sign never changes, so nothing appears or
                    // vanishes; the magnitude does, which moves the normals, which moves the paint
                    // rule that follows them. Four hundred voxels of moss in the wrong place (D644).
                    if (op == WS_OP_UNION) {
                        while (next < nc) {
                            const uint candidate = WS_CHILD(next);
                            if (candidate >= node_count) break;   // let the push refuse it
                            const float away = ws_box_away_sq(candidate, fp);
                            if (!(away > 0.0 && (acc < 0.0 || acc * acc <= away))) break;
                            ++next;
                        }
                    }
                }
                if (next >= nc) WS_FINISH(stack[fi].acc);
                stack[fi].step = next + 1u;
                WS_PUSH(WS_CHILD(next), fp);
            }

            // ---- the point folded into a cell, then the leaning neighbours ------------------
            case WS_OP_REPEAT: {
                // The fold on its own answers "how far to the copy in THIS cell", which is not the
                // same question as "how far to the nearest copy" whenever a copy sits off-centre in
                // its cell — an overstatement, and the dangerous direction, because a sampler that
                // believes there is nothing nearby skips over the slat that is. Checking the
                // leaning neighbour makes it exact, and exact is worth far more than the extra
                // evaluation costs: an honest distance settles whole boxes at once.
                if (step == 0u) {
                    vec3 q = fp;
                    vec3 lean = vec3(0.0);
                    uint packed = 0u;
                    uint neighbours = 0u;
                    for (uint axis = 0u; axis < 3u; ++axis) {
                        const float period = WS_A(axis);
                        if (period <= 0.0) continue;
                        const float limit = WS_A(3u + axis);
                        const float value = ws_axis_of(fp, axis);
                        float cell = ws_walk_round(value / period);
                        if (limit > 0.0) cell = ws_walk_clamp(cell, -limit, limit);
                        const float folded = value - period * cell;
                        q = ws_with_axis(q, axis, folded);

                        float other = cell + ((folded >= 0.0) ? 1.0 : -1.0);
                        if (limit > 0.0) other = ws_walk_clamp(other, -limit, limit);
                        // A cell the limit has clamped away has no copy in it and must not be
                        // consulted: taking the minimum against a copy that does not exist invents
                        // matter. That is what `other != cell` is testing.
                        if (other != cell) {
                            packed |= axis << (2u * neighbours);
                            lean[neighbours] = value - period * other;
                            ++neighbours;
                        }
                    }
                    stack[fi].fold = q;
                    stack[fi].lean = lean;
                    stack[fi].axes = packed;
                    stack[fi].neighbours = neighbours;
                    stack[fi].step = 1u;
                    WS_PUSH(WS_CHILD(0), q);
                }

                const uint done = step - 1u;   // how many points have come back
                stack[fi].acc = (done == 0u) ? ret : min(stack[fi].acc, ret);
                const uint neighbours = stack[fi].neighbours;
                const uint combinations = 1u << neighbours;
                if (step >= combinations) WS_FINISH(stack[fi].acc);

                // Every combination of leaning neighbours, because with two axes repeating it is
                // the DIAGONAL copy that can be nearest. `step` is the mask, 1..combinations-1,
                // exactly the loop the recursive evaluator writes.
                vec3 shifted = stack[fi].fold;
                for (uint i = 0u; i < neighbours; ++i) {
                    if (((step >> i) & 1u) != 0u) {
                        shifted = ws_with_axis(shifted, (stack[fi].axes >> (2u * i)) & 3u,
                                               stack[fi].lean[i]);
                    }
                }
                stack[fi].step = step + 1u;
                WS_PUSH(WS_CHILD(0), shifted);
            }

            // ---- the same walk, over cell INDICES rather than folded points ------------------
            //
            // `repeat` can carry the folded point from cell to cell because every copy sits the
            // same way in its cell. A scatter's do not, so what is carried in `fold` and `lean` is
            // the INDEX and the point is rebuilt from it each time — which is also what makes this
            // frame need a `scale`, since the copy's own size has to survive the push and come back.
            case WS_OP_SCATTER: {
                if (step == 0u) {
                    vec3 cell = vec3(0.0);
                    vec3 lean = vec3(0.0);
                    uint packed = 0u;
                    uint neighbours = 0u;
                    for (uint axis = 0u; axis < 3u; ++axis) {
                        const float period = WS_A(axis);
                        if (period <= 0.0) continue;
                        const float limit = WS_A(3u + axis);
                        const float value = ws_axis_of(fp, axis);
                        float here = ws_walk_round(value / period);
                        if (limit > 0.0) here = ws_walk_clamp(here, -limit, limit);
                        cell[axis] = here;

                        float other = here + (((value - period * here) >= 0.0) ? 1.0 : -1.0);
                        if (limit > 0.0) other = ws_walk_clamp(other, -limit, limit);
                        if (other != here) {
                            packed |= axis << (2u * neighbours);
                            lean[neighbours] = other;
                            ++neighbours;
                        }
                    }
                    stack[fi].fold = cell;
                    stack[fi].lean = lean;
                    stack[fi].axes = packed;
                    stack[fi].neighbours = neighbours;
                    stack[fi].step = 1u;

                    float scale = 1.0;
                    const vec3 q = ws_walk_scatter_point(ni, fp, cell, scale);
                    stack[fi].scale = scale;
                    WS_PUSH(WS_CHILD(0), q);
                }

                const float came_back = stack[fi].scale * ret;
                const uint done = step - 1u;
                stack[fi].acc = (done == 0u) ? came_back : min(stack[fi].acc, came_back);
                const uint neighbours = stack[fi].neighbours;
                const uint combinations = 1u << neighbours;
                if (step >= combinations) WS_FINISH(stack[fi].acc);

                // The mask names which axes take their neighbouring cell instead of this one. Note
                // it indexes by the AXIS the slot holds, not by the slot — two axes repeating means
                // slot 0 might be x and slot 1 z.
                vec3 cell = stack[fi].fold;
                for (uint i = 0u; i < neighbours; ++i) {
                    if (((step >> i) & 1u) != 0u) {
                        cell[(stack[fi].axes >> (2u * i)) & 3u] = stack[fi].lean[i];
                    }
                }
                stack[fi].step = step + 1u;

                float scale = 1.0;
                const vec3 q = ws_walk_scatter_point(ni, fp, cell, scale);
                stack[fi].scale = scale;
                WS_PUSH(WS_CHILD(0), q);
            }

            default:
                // An op this file has not learned. Never a number — see WS_FIELD_REFUSED.
                ws_field_refused_op = op;
                return WS_FIELD_REFUSED;
        }
    }

    return ret;
}

#undef WS_A
#undef WS_CHILD
#undef WS_PUSH
#undef WS_FINISH

#endif   // WS_FIELD_WALK_GLSL
