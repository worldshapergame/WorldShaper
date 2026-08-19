#include "forge/compile.hpp"

#include <cmath>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace ws {
namespace forge {
namespace {

constexpr u32 kNone = 0xFFFFFFFFu;
constexpr f64 kTau = 6.283185307179586476925286766559;

// The three coordinate helpers `field.cpp` keeps to itself. Copied rather than exported because
// exporting them would mean editing `field.hpp`, and five agents are in that file today.
f64 axis_of(Vec3 v, u32 axis) { return (axis == 0) ? v.x : (axis == 1) ? v.y : v.z; }

bool is_zero(Vec3 v) { return v.x == 0.0 && v.y == 0.0 && v.z == 0.0; }

// The point mapping `Op::Rotate` applies, spelled exactly as `Field::eval` spells it.
//
// It has to be exactly that and not merely an equivalent rotation, because this is used to carry
// a translation THROUGH a rotate: `rotate(child)(p - t)` is `child(M(p) - M(t))`, and M has to be
// the same M on both sides of that identity or the shape lands somewhere else. Copied from the
// `case Op::Rotate` arm, angle negation and euler order included.
Vec3 rotate_like_eval(Vec3 p, const f64* a) {
    const f64 cx = std::cos(-a[0] * kTau), sx = std::sin(-a[0] * kTau);
    const f64 cy = std::cos(-a[1] * kTau), sy = std::sin(-a[1] * kTau);
    const f64 cz = std::cos(-a[2] * kTau), sz = std::sin(-a[2] * kTau);
    Vec3 q = p;
    q = {q.x, q.y * cx - q.z * sx, q.y * sx + q.z * cx};
    q = {q.x * cy + q.z * sy, q.y, -q.x * sy + q.z * cy};
    q = {q.x * cz - q.y * sz, q.x * sz + q.y * cz, q.z};
    return q;
}

// --- what each op is, for the purposes of moving a translation about -----------------------

// A primitive that subtracts a centre out of `a[0..2]` before it does anything else. For every one
// of these, `prim(p - t)` and `prim'(p)` with the centre moved by `t` are the same shape — which is
// the whole of "a translate over a box is a box somewhere else".
//
// `Revolve` is on the list and it is worth saying why: it folds `p - a[0..2]` into (radius, height)
// and asks its profile there, so moving its centre moves the solid and leaves the profile's own
// coordinates alone. `Spiral` and `Radius` are the same shape of thing.
bool absorbs_centre(Op op) {
    switch (op) {
        case Op::Radius: case Op::Sphere: case Op::Box: case Op::Cylinder: case Op::Torus:
        case Op::Arc: case Op::Cone: case Op::Ellipsoid: case Op::Prism: case Op::Platonic:
        case Op::Wedge: case Op::Stairs: case Op::Revolve: case Op::Spiral:
            return true;
        default: return false;
    }
}

// Every child is asked about the same point, or about a point a FIXED offset from it, so a
// translation applied to the parent is the same shape as the translation applied to each child.
//
// `Curvature`, `Occlusion` and `Facing` are on this list although they evaluate their child six,
// seven and fourteen times: every one of those samples is `p + delta` for a delta that does not
// depend on where the shape is, so shifting the child shifts all of them together. That is exactly
// the property `Field::mirror_eval` relies on when it counts steps over SAMPLE POINTS rather than
// over children (D643), read from the other end.
bool is_pointwise(Op op) {
    switch (op) {
        case Op::Union: case Op::Intersection: case Op::Difference:
        case Op::SmoothUnion: case Op::SmoothDifference: case Op::SmoothIntersection:
        case Op::ChamferUnion: case Op::ChamferDifference: case Op::ChamferIntersection:
        case Op::Displace: case Op::Add: case Op::Multiply: case Op::Min: case Op::Max:
        case Op::Blend: case Op::Shell: case Op::Round: case Op::Offset: case Op::Remap:
        case Op::Abs: case Op::Negate: case Op::Step: case Op::Smoothstep: case Op::Clamp:
        case Op::Power: case Op::Curvature: case Op::Occlusion: case Op::Facing:
            return true;
        default: return false;
    }
}

// A combine whose op is exactly associative AND commutative in IEEE-754 for finite operands, so
// its parts may be re-grouped and re-ordered without moving a single bit of the answer.
//
// `min` and `max` are the only two that qualify. `Add` and `Multiply` are associative in real
// arithmetic and are NOT in floating point, and the smooth and chamfer joins are not associative
// even in real arithmetic — a blend of a blend is a different surface. Those are still flattened
// along the chain `Field::combine` built, which preserves their order exactly; they are never
// re-balanced.
bool is_free_combine(Op op) {
    return op == Op::Union || op == Op::Intersection || op == Op::Min || op == Op::Max;
}

// A combine whose parts may be flattened only at child 0 — which is where `Field::combine` puts
// the rest of the chain, so a chain still collapses and nothing else moves.
bool is_ordered_combine(Op op) {
    switch (op) {
        case Op::Difference: case Op::SmoothUnion: case Op::SmoothDifference:
        case Op::SmoothIntersection: case Op::ChamferUnion: case Op::ChamferDifference:
        case Op::ChamferIntersection: case Op::Add: case Op::Multiply:
            return true;
        default: return false;
    }
}

bool is_combine(Op op) { return is_free_combine(op) || is_ordered_combine(op); }

// Whether this pass knows how to place the op back into a field.
//
// A separate list from the switch in `construct` deliberately, and both have to be edited when an
// op is added. That is the point: an op nobody taught this file about must make `compile_field`
// REFUSE and hand back the original, not silently drop a shape. Trap 7 — "I could not" and "the
// answer is nought" must never be the same reply.
bool can_rebuild(Op op) {
    switch (op) {
        case Op::Constant: case Op::Parameter: case Op::Coordinate: case Op::Radius:
        case Op::Sphere: case Op::Box: case Op::Cylinder: case Op::Capsule: case Op::Torus:
        case Op::Arc: case Op::Cone: case Op::Plane: case Op::Ellipsoid: case Op::Prism:
        case Op::Platonic: case Op::Wedge: case Op::Stairs: case Op::Revolve: case Op::Spiral:
        case Op::Union: case Op::Intersection: case Op::Difference: case Op::SmoothUnion:
        case Op::SmoothDifference: case Op::SmoothIntersection: case Op::ChamferUnion:
        case Op::ChamferDifference: case Op::ChamferIntersection:
        case Op::Translate: case Op::Rotate: case Op::Scale: case Op::Mirror: case Op::Repeat:
        case Op::PolarRepeat: case Op::Scatter:
        case Op::Shell: case Op::Round: case Op::Offset: case Op::Displace: case Op::Twist:
        case Op::Bend:
        case Op::Sine: case Op::Waves: case Op::Noise: case Op::Fbm: case Op::Ridged:
        case Op::Rasp: case Op::Cells: case Op::CellEdge:
        case Op::Curvature: case Op::Occlusion: case Op::Facing: case Op::Checker:
        case Op::Stripes: case Op::Bricks:
        case Op::Add: case Op::Multiply: case Op::Min: case Op::Max: case Op::Blend:
        case Op::Remap: case Op::Abs: case Op::Negate: case Op::Step: case Op::Smoothstep:
        case Op::Clamp: case Op::Power:
            return true;
    }
    return false;
}

// A leaf that reads the point directly. Anything built only out of constants and parameters
// answers the same number everywhere, which is what lets a whole subexpression be folded to one
// node AND lets a translation over it be dropped without a thought.
bool reads_point_leaf(Op op) {
    switch (op) {
        case Op::Constant: case Op::Parameter: return false;
        default: return true;
    }
}

u64 mix(u64 h, u64 v) {
    h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    return h;
}

u64 hash_node(const Node& n) {
    u64 h = static_cast<u64>(n.op);
    for (u32 i = 0; i < 8; ++i) {
        u64 bits = 0;
        std::memcpy(&bits, &n.a[i], sizeof(bits));
        h = mix(h, bits);
    }
    for (u32 i = 0; i < 4; ++i) h = mix(h, n.child[i]);
    return mix(h, n.children);
}

bool same_node(const Node& a, const Node& b) {
    if (a.op != b.op || a.children != b.children) return false;
    for (u32 i = 0; i < 8; ++i) {
        u64 x = 0, y = 0;
        std::memcpy(&x, &a.a[i], sizeof(x));
        std::memcpy(&y, &b.a[i], sizeof(y));
        if (x != y) return false;
    }
    for (u32 i = 0; i < a.children; ++i) {
        if (a.child[i] != b.child[i]) return false;
    }
    return true;
}

// ------------------------------------------------------------------------------------------
// The pass itself.
// ------------------------------------------------------------------------------------------
struct Compiler {
    const Field& in;
    CompileOptions opt;
    CompileReport rep;
    Field out;

    std::vector<u32> refs;        // parent EDGES into each node, over what `root` can reach
    // Two marks and not one, because the two walks below no longer run back to back: the witness
    // pass surveys and classifies the NAMES after the roots have been emitted, and a single shared
    // mark would have one of the two silently skip everything the other had already touched.
    std::vector<u8> surveyed;
    std::vector<u8> classified;
    std::vector<u8> reads;        // the value depends on where you ask
    std::vector<u8> has_param;    // ...and on a dial somebody can drag
    std::vector<u8> cost;         // 0, 1, or 2 meaning "two or more"
    std::vector<u8> cost_known;
    std::vector<u32> emitted;     // input node -> output node, for the shift-free emission
    std::vector<u32> slot_node;   // parameter slot -> the one output node that reads it
    std::vector<u32> scratch;     // subtree sizing, stamped rather than cleared
    u32 stamp = 0;

    std::unordered_multimap<u64, u32> seen_nodes;

    explicit Compiler(const Field& field, const CompileOptions& options)
        : in(field), opt(options) {}

    // --- the walk that has to happen before any rewriting can ------------------------------
    //
    // `refs` is the whole safety mechanism of this file. A subtree with two parents is never
    // specialised for one of them, so no subtree is ever copied and every input node is emitted
    // at most once — which is what makes "the output is never bigger than the input" a property
    // of the construction rather than a hope.
    void survey(u32 n) {
        if (surveyed[n]) return;
        surveyed[n] = 1;
        const Node& s = in.node(n);
        for (u32 i = 0; i < s.children; ++i) {
            ++refs[s.child[i]];
            survey(s.child[i]);
        }
    }

    void classify(u32 n) {
        if (classified[n]) return;
        classified[n] = 1;
        const Node& s = in.node(n);
        bool r = reads_point_leaf(s.op) && s.children == 0;
        bool p = (s.op == Op::Parameter);
        // A revolve reads the point even when its profile does not: a partial sweep measures the
        // point's own angle and its distance from the axis to find the end cap. Said explicitly
        // because "no child reads the point" would otherwise fold a half dome to a number.
        if (s.op == Op::Revolve || s.op == Op::Spiral) r = true;
        for (u32 i = 0; i < s.children; ++i) {
            classify(s.child[i]);
            r = r || reads[s.child[i]] != 0;
            p = p || has_param[s.child[i]] != 0;
        }
        reads[n] = r ? 1 : 0;
        has_param[n] = p ? 1 : 0;
    }

    // How many `Translate` nodes are left behind if a non-zero shift is pushed into `n`.
    //
    // Capped at two, because the only question the decision asks is "more than the one translate
    // this would replace?". Pushing is allowed at nought and at one: nought is a node removed, one
    // is the same node count with the translate moved deeper, where it is inside a union's cull
    // instead of outside it.
    u8 cost_of(u32 n) {
        if (cost_known[n]) return cost[n];
        cost_known[n] = 1;
        cost[n] = 2;   // in case anything ever asks re-entrantly; a field has no cycles
        const Node& s = in.node(n);
        u8 c = 2;
        if (!reads[n]) {
            c = 0;                              // the shift cannot be observed
        } else if (absorbs_centre(s.op) || s.op == Op::Capsule || s.op == Op::Plane) {
            c = 0;                              // it lands in the node's own numbers
        } else if (s.op == Op::Translate || s.op == Op::Rotate || s.op == Op::Scale) {
            c = effective_cost(s.child[0]);     // carried straight through
        } else if (is_pointwise(s.op)) {
            u32 total = 0;
            for (u32 i = 0; i < s.children; ++i) total += effective_cost(s.child[i]);
            c = static_cast<u8>(total > 2 ? 2 : total);
        } else {
            // Mirror is here rather than with rotate and scale on purpose: whether a shift can
            // cross a fold depends on the shift, not on the node, so costing it optimistically
            // would license a push that then costs more than it saved. If the shift does turn out
            // to lie in the mirror plane the emission takes it through anyway and beats this.
            c = 1;
        }
        cost[n] = c;
        return c;
    }

    // One node, no children, and it takes the shift into its own numbers.
    //
    // The only shape a subtree may have and still be specialised for one of several parents. A copy
    // of it is one node and it replaces a translate that was one node, so the clip does not grow;
    // everything larger than this would be a duplicated subtree, which is the failure this pass
    // refuses by construction. Sharing then usually takes the copy back: two `translate { block }`
    // over the same block by the same vector become one node again in `place`.
    bool absorbing_leaf(u32 n) const {
        const Node& s = in.node(n);
        if (s.children != 0) return false;
        if (!reads[n]) return true;   // a constant: the shift is not observable at all
        return absorbs_centre(s.op) || s.op == Op::Capsule || s.op == Op::Plane;
    }

    u8 effective_cost(u32 n) {
        if (refs[n] == 1) return cost_of(n);
        if (opt.absorb_into_shared_leaves && absorbing_leaf(n)) return 0;
        return 1;
    }

    // --- placing a node in the output ------------------------------------------------------

    u32 construct(const Node& w) {
        const f64* a = w.a;
        const u32 c0 = w.child[0];
        std::vector<u32> parts;
        if (is_combine(w.op)) {
            for (u32 i = 0; i < w.children; ++i) parts.push_back(w.child[i]);
        }
        switch (w.op) {
            case Op::Constant: return out.constant(a[0]);
            case Op::Coordinate: return out.coordinate(static_cast<u32>(a[0]));
            case Op::Radius: return out.radius({a[0], a[1], a[2]});
            case Op::Sphere: return out.sphere({a[0], a[1], a[2]}, a[3]);
            case Op::Box: return out.box({a[0], a[1], a[2]}, {a[3], a[4], a[5]}, a[6]);
            case Op::Cylinder:
                return out.cylinder({a[0], a[1], a[2]}, a[3], a[4], static_cast<u32>(a[5]));
            case Op::Capsule: return out.capsule({a[0], a[1], a[2]}, {a[3], a[4], a[5]}, a[6]);
            case Op::Torus:
                return out.torus({a[0], a[1], a[2]}, a[3], a[4], static_cast<u32>(a[5]));
            case Op::Arc:
                return out.arc({a[0], a[1], a[2]}, a[3], a[4], static_cast<u32>(a[5]),
                               a[6], a[6] + a[7]);
            case Op::Cone:
                return out.cone({a[0], a[1], a[2]}, a[3], a[4], static_cast<u32>(a[5]));
            case Op::Plane: return out.plane({a[0], a[1], a[2]}, a[3]);
            case Op::Ellipsoid: return out.ellipsoid({a[0], a[1], a[2]}, {a[3], a[4], a[5]});
            case Op::Prism:
                return out.prism({a[0], a[1], a[2]}, a[3], a[4], static_cast<u32>(a[5]),
                                 static_cast<u32>(a[6]), a[7]);
            case Op::Platonic:
                return out.platonic({a[0], a[1], a[2]}, a[3], static_cast<u32>(a[4]));
            case Op::Wedge:
                return out.wedge({a[0], a[1], a[2]}, {a[3], a[4], a[5]},
                                 static_cast<u32>(a[6]), static_cast<u32>(a[7]));
            case Op::Stairs:
                return out.stairs({a[0], a[1], a[2]}, {a[3], a[4], a[5]}, a[6], a[7]);
            case Op::Revolve:
                return out.revolve(c0, {a[0], a[1], a[2]}, static_cast<u32>(a[3]),
                                   a[4], a[4] + a[5]);
            case Op::Spiral:
                return out.spiral({a[0], a[1], a[2]}, a[3], a[4], a[5], a[6],
                                  static_cast<u32>(a[7]));

            case Op::Union: return out.unite(parts);
            case Op::Intersection: return out.intersect(parts);
            case Op::Difference: return out.subtract(parts);
            case Op::SmoothUnion: return out.smooth_unite(parts, a[0]);
            case Op::SmoothDifference: return out.smooth_subtract(parts, a[0]);
            case Op::SmoothIntersection: return out.smooth_intersect(parts, a[0]);
            case Op::ChamferUnion: return out.chamfer_unite(parts, a[0]);
            case Op::ChamferDifference: return out.chamfer_subtract(parts, a[0]);
            case Op::ChamferIntersection: return out.chamfer_intersect(parts, a[0]);
            case Op::Add: return out.add(parts);
            case Op::Multiply: return out.multiply(parts);
            case Op::Min: return out.minimum(parts);
            case Op::Max: return out.maximum(parts);

            case Op::Translate: return out.translate(c0, {a[0], a[1], a[2]});
            case Op::Rotate: return out.rotate(c0, {a[0], a[1], a[2]});
            case Op::Scale: return out.scale(c0, {a[0], a[1], a[2]});
            case Op::Mirror: return out.mirror(c0, static_cast<u32>(a[0]));
            case Op::Repeat: return out.repeat(c0, {a[0], a[1], a[2]}, {a[3], a[4], a[5]});
            case Op::Scatter:
                return out.scatter(c0, {a[0], a[1], a[2]}, {a[3], a[4], a[5]}, a[6], a[7]);
            case Op::PolarRepeat:
                return out.polar_repeat(c0, static_cast<u32>(a[0]), static_cast<u32>(a[1]),
                                        a[2], a[2] + a[3]);

            case Op::Shell: return out.shell(c0, a[0]);
            case Op::Round: return out.round_off(c0, a[0]);
            case Op::Offset: return out.offset(c0, a[0]);
            case Op::Displace: return out.displace(c0, w.child[1], a[0]);
            case Op::Twist: return out.twist(c0, a[0], static_cast<u32>(a[1]));
            case Op::Bend: return out.bend(c0, a[0], static_cast<u32>(a[1]));

            case Op::Sine: return out.sine(static_cast<u32>(a[0]), a[1], a[2]);
            case Op::Waves: return out.waves(static_cast<u32>(a[0]), a[1], a[2], a[3]);
            case Op::Noise: return out.noise(a[0], static_cast<u32>(a[1]), {a[2], a[3], a[4]});
            case Op::Fbm:
                return out.fbm(a[0], static_cast<u32>(a[1]), a[2], a[3],
                               static_cast<u32>(a[4]), {a[5], a[6], a[7]});
            case Op::Ridged:
                return out.ridged(a[0], static_cast<u32>(a[1]), a[2], a[3],
                                  static_cast<u32>(a[4]), {a[5], a[6], a[7]});
            case Op::Rasp:
                return out.rasp(a[0], a[1], static_cast<u32>(a[2]), {a[3], a[4], a[5]});
            case Op::Cells: return out.cells(a[0], static_cast<u32>(a[1]), {a[2], a[3], a[4]});
            case Op::CellEdge:
                return out.cell_edge(a[0], static_cast<u32>(a[1]), {a[2], a[3], a[4]});

            case Op::Curvature: return out.curvature(c0, a[0]);
            case Op::Occlusion: return out.occlusion(c0, a[0]);
            case Op::Facing: return out.facing(c0, static_cast<u32>(a[0]));
            case Op::Checker: return out.checker({a[0], a[1], a[2]});
            case Op::Stripes: return out.stripes(static_cast<u32>(a[0]), a[1], a[2]);
            case Op::Bricks: return out.bricks({a[0], a[1], a[2]}, a[3], static_cast<u32>(a[4]));

            case Op::Blend: return out.blend(c0, w.child[1], a[0]);
            case Op::Remap: return out.remap(c0, a[0], a[1], a[2], a[3]);
            case Op::Abs: return out.absolute(c0);
            case Op::Negate: return out.negate(c0);
            case Op::Step: return out.step(c0, a[0]);
            case Op::Smoothstep: return out.smoothstep(c0, a[0], a[1]);
            case Op::Clamp: return out.clamp_to(c0, a[0], a[1]);
            case Op::Power: return out.power(c0, a[0]);

            case Op::Parameter: break;   // never reaches here; slots are placed once, up front
        }
        return out.constant(1e30);
    }

    // The one door every output node goes through: the identities, then the sharing, then the
    // builder. Doing it in one place is D204's rule applied to a compiler — two routes into the
    // output are two chances for one of them to skip a check.
    u32 place(Node w) {
        if (w.children == 0) {
            for (u32 i = 0; i < 4; ++i) w.child[i] = 0;
        }
        if (opt.remove_identities) {
            switch (w.op) {
                case Op::Translate:
                    if (w.a[0] == 0.0 && w.a[1] == 0.0 && w.a[2] == 0.0) {
                        ++rep.identities_removed;
                        return w.child[0];
                    }
                    break;
                case Op::Rotate:
                    if (w.a[0] == 0.0 && w.a[1] == 0.0 && w.a[2] == 0.0) {
                        ++rep.identities_removed;
                        return w.child[0];
                    }
                    break;
                case Op::Scale: {
                    // A stored nought means one, exactly as `Field::eval` reads it.
                    const f64 sx = w.a[0] != 0.0 ? w.a[0] : 1.0;
                    const f64 sy = w.a[1] != 0.0 ? w.a[1] : 1.0;
                    const f64 sz = w.a[2] != 0.0 ? w.a[2] : 1.0;
                    if (sx == 1.0 && sy == 1.0 && sz == 1.0) {
                        ++rep.identities_removed;
                        return w.child[0];
                    }
                    break;
                }
                case Op::Offset:
                case Op::Round:
                    if (w.a[0] == 0.0) {
                        ++rep.identities_removed;
                        return w.child[0];
                    }
                    break;
                case Op::Mirror: {
                    // Folding about a plane twice is folding about it once: |,|x|,| is |x|.
                    const Node& kid = out.node(w.child[0]);
                    if (kid.op == Op::Mirror && kid.a[0] == w.a[0]) {
                        ++rep.identities_removed;
                        return w.child[0];
                    }
                    break;
                }
                default: break;
            }
        }

        if (opt.share_identical) {
            const u64 h = hash_node(w);
            auto range = seen_nodes.equal_range(h);
            for (auto it = range.first; it != range.second; ++it) {
                if (same_node(out.node(it->second), w)) {
                    ++rep.shared_by_cse;
                    return it->second;
                }
            }
            const u32 made = construct(w);
            seen_nodes.emplace(h, made);
            return made;
        }
        return construct(w);
    }

    u32 place_translate(u32 child, Vec3 by) {
        Node w;
        w.op = Op::Translate;
        w.child[0] = child;
        w.children = 1;
        w.a[0] = by.x; w.a[1] = by.y; w.a[2] = by.z;
        return place(w);
    }

    // --- combines: flatten the chain, then decide how to nest what is left ------------------

    void gather(u32 n, Op op, f64 blend, std::vector<u32>& parts) {
        const Node& s = in.node(n);
        for (u32 i = 0; i < s.children; ++i) {
            const u32 c = s.child[i];
            const Node& kid = in.node(c);
            const bool position_ok = is_free_combine(op) || i == 0;
            // `refs[c] == 1` is what makes this a strict removal: the inner node has one parent,
            // so absorbing its parts here deletes it outright rather than inlining a copy of
            // something that still has to exist for somebody else.
            if (opt.flatten_combines && kid.op == op && kid.a[0] == blend && refs[c] == 1 &&
                position_ok && parts.size() < opt.flatten_limit) {
                ++rep.combines_flattened;
                gather(c, op, blend, parts);
            } else {
                parts.push_back(c);
            }
        }
    }

    // Exactly the nesting `Field::combine` builds: child 0 carries the chain and up to three more
    // ride with it. Used for every op whose arithmetic is order-dependent, so their answers cannot
    // move at all.
    u32 chain(Op op, f64 blend, const std::vector<u32>& parts) {
        u32 current = parts[0];
        usize i = 1;
        while (i < parts.size()) {
            Node w;
            w.op = op;
            w.a[0] = blend;
            w.child[0] = current;
            w.children = 1;
            while (i < parts.size() && w.children < 4) w.child[w.children++] = parts[i++];
            current = place(w);
        }
        return current;
    }

    // The same parts, in the same order, nested as a tree instead of a list.
    //
    // This is worth the paragraph because it is the one structural rewrite that is NOT obviously
    // free. `Field::combine` chains a wide union left-deep, and the cull in `Field::eval` sorts a
    // union's children by how far the point is from each child's box and stops at the first one it
    // can reject. Child 0 of a chain link is the whole rest of the chain, so its box is the union
    // of everything before it — a box no point inside the building is ever outside — so it sorts
    // first, is never rejected, and the walk descends the entire chain at every sample. Forty
    // parts is thirteen frames that cannot be skipped.
    //
    // Grouped four at a time IN ORDER, and not by where things are. Re-grouping a union spatially
    // is the accelerator, and the accelerator has been built, measured and refused twice — on the
    // facility (D637) and again on the estate (D682) — because the parts of a building are LAYERS
    // and not regions. This changes only the nesting, so an author's grouping survives it and the
    // answer is bit-identical: min and max are exactly associative and commutative.
    u32 balanced(Op op, f64 blend, std::vector<u32> level) {
        while (level.size() > 4) {
            std::vector<u32> next;
            for (usize i = 0; i < level.size(); i += 4) {
                const usize span = (level.size() - i < 4) ? (level.size() - i) : 4;
                if (span == 1) { next.push_back(level[i]); continue; }
                Node w;
                w.op = op;
                w.a[0] = blend;
                w.children = static_cast<u32>(span);
                for (usize k = 0; k < span; ++k) w.child[k] = level[i + k];
                next.push_back(place(w));
            }
            level.swap(next);
        }
        if (level.size() == 1) return level[0];
        Node w;
        w.op = op;
        w.a[0] = blend;
        w.children = static_cast<u32>(level.size());
        for (usize k = 0; k < level.size(); ++k) w.child[k] = level[k];
        return place(w);
    }

    u32 assemble(Op op, f64 blend, std::vector<u32> parts) {
        if (opt.drop_duplicates && is_free_combine(op) && parts.size() > 1) {
            // min(a, a) is a, and after sharing two identical subtrees carry one index. A union
            // that names the same shape twice is not hypothetical in a clip assembled out of
            // twenty fragments by many hands.
            std::vector<u32> kept;
            for (u32 p : parts) {
                bool already = false;
                for (u32 q : kept) { if (q == p) { already = true; break; } }
                if (already) { ++rep.duplicate_children_dropped; continue; }
                kept.push_back(p);
            }
            parts.swap(kept);
        }
        if (parts.empty()) return place_constant(1e30);
        if (parts.size() == 1) {
            // Every combine in `Field::eval` starts at child 0 and loops from one, so a combine of
            // one part IS that part, blend or no blend.
            ++rep.identities_removed;
            return parts[0];
        }
        if (parts.size() > 4 && opt.rebalance_combines && is_free_combine(op)) {
            ++rep.combines_rebalanced;
            return balanced(op, blend, std::move(parts));
        }
        return chain(op, blend, parts);
    }

    u32 place_constant(f64 v) {
        Node w;
        w.op = Op::Constant;
        w.a[0] = v;
        return place(w);
    }

    // --- how many distinct nodes a subtree holds, for the fold's own accounting -------------
    usize subtree_size(u32 n) {
        ++stamp;
        return subtree_walk(n);
    }
    usize subtree_walk(u32 n) {
        if (scratch[n] == stamp) return 0;
        scratch[n] = stamp;
        usize total = 1;
        const Node& s = in.node(n);
        for (u32 i = 0; i < s.children; ++i) total += subtree_walk(s.child[i]);
        return total;
    }

    // --- the witness pass: what carrying a NAME would cost, asked before it is carried ---------

    // A subtree the compilation has already finished with. Both walks below stop here, and for the
    // same reason each time: whatever this node became, it is in the output, it was rebuildable
    // because it WAS rebuilt, and re-emitting it will cost nothing.
    bool already_answered(u32 n) const {
        if (emitted[n] != kNone) return true;
        // ...and an expression with no point in it will be folded to a number by `emit_fresh`
        // before any op inside it is looked at, so the ops inside it are not this pass's problem
        // and its nodes are not this pass's cost. `Field::eval` can read anything.
        return opt.fold_constants && !reads[n] && !has_param[n];
    }

    // The most new nodes emitting `n` shift-free could add, which is an upper bound and not a
    // guess: every rewrite in this file emits at most one output node per input node, and
    // everything already in `emitted` is a lookup.
    usize unemitted_size(u32 n) {
        ++stamp;
        return unemitted_walk(n);
    }
    usize unemitted_walk(u32 n) {
        if (scratch[n] == stamp) return 0;
        scratch[n] = stamp;
        if (already_answered(n)) return 0;
        usize total = 1;
        const Node& s = in.node(n);
        for (u32 i = 0; i < s.children; ++i) total += unemitted_walk(s.child[i]);
        return total;
    }

    // Whether this pass could place every op under `n` back into a field.
    //
    // `compile_field` scans the ROOTS for this before it rewrites anything, and that scan cannot
    // have reached here: a name is carried precisely because nothing reachable from a root leads to
    // it. Without this the unhandled op would fall off the end of `construct`'s switch and become
    // `constant(1e30)` — a name silently meaning "empty everywhere", which is the one reply trap 7
    // says must never be how "I could not" comes out.
    bool rebuildable(u32 n) {
        ++stamp;
        return rebuildable_walk(n);
    }
    bool rebuildable_walk(u32 n) {
        if (scratch[n] == stamp) return true;
        scratch[n] = stamp;
        if (already_answered(n)) return true;
        const Node& s = in.node(n);
        if (!can_rebuild(s.op)) return false;
        for (u32 i = 0; i < s.children; ++i) {
            if (!rebuildable_walk(s.child[i])) return false;
        }
        return true;
    }

    // --- the walk ---------------------------------------------------------------------------

    // Hand `t` to `c`, or give up and put a translate over it.
    //
    // The two refusals are the two ways a push can cost more than it saves, and they are counted
    // separately because they are different facts about a clip: `refused_shared` says the author
    // reused a shape under two different placements, `refused_costly` says the shape has a
    // pattern or a repeat in it that no shift can cross.
    u32 carry(u32 c, Vec3 t) {
        if (is_zero(t)) return emit(c, Vec3{0, 0, 0});
        if (!opt.fold_transforms) {
            ++rep.materialised;
            return place_translate(emit(c, Vec3{0, 0, 0}), t);
        }
        if (refs[c] != 1 && !(opt.absorb_into_shared_leaves && absorbing_leaf(c))) {
            ++rep.refused_shared;
            ++rep.materialised;
            return place_translate(emit(c, Vec3{0, 0, 0}), t);
        }
        if (cost_of(c) > 1) {
            ++rep.refused_costly;
            ++rep.materialised;
            return place_translate(emit(c, Vec3{0, 0, 0}), t);
        }
        return emit(c, t);
    }

    u32 emit(u32 n, Vec3 shift) {
        // Only the shift-free emission is remembered, and that is not a cache policy, it is the
        // invariant: a node reached with a shift has exactly one parent (`carry` refuses
        // otherwise), so it is reached exactly once and has nothing to remember. If that ever
        // stopped being true the worst that happens is a node built twice — bigger, never wrong.
        const bool plain = is_zero(shift);
        if (plain && emitted[n] != kNone) return emitted[n];
        const u32 result = emit_fresh(n, shift);
        if (plain) emitted[n] = result;
        return result;
    }

    u32 emit_fresh(u32 n, Vec3 shift) {
        const Node& s = in.node(n);

        // An expression with no point in it is a number, and asking it about a moved point is
        // still that number. Both halves matter: the fold removes the subtree, and the shift it
        // was carrying evaporates instead of being materialised over it.
        if (!reads[n] && !has_param[n] && opt.fold_constants) {
            const usize held = subtree_size(n);
            if (held > 1) {
                ++rep.constant_subtrees_folded;
                rep.constant_nodes_removed += held - 1;
            }
            return place_constant(in.eval(n, Vec3{0, 0, 0}));
        }
        if (!reads[n]) shift = Vec3{0, 0, 0};   // parameters only: point-independent, dial intact

        if (s.op == Op::Parameter) return slot_node[static_cast<usize>(s.a[0])];

        // --- the fold, and it is the whole reason this file exists --------------------------
        if (s.op == Op::Translate) {
            const Vec3 by{s.a[0], s.a[1], s.a[2]};
            if (!opt.fold_transforms) {
                return place_translate(emit(s.child[0], Vec3{0, 0, 0}), by);
            }
            if (!is_zero(shift)) ++rep.chains_merged;
            return carry(s.child[0], shift + by);
        }

        if (!is_zero(shift)) {
            if (absorbs_centre(s.op)) {
                ++rep.absorbed_by_centre;
                Node w = s;
                w.a[0] += shift.x; w.a[1] += shift.y; w.a[2] += shift.z;
                // A revolve's profile lives in the folded (radius, height) plane and knows nothing
                // about where the solid stands, so it is emitted unmoved.
                for (u32 i = 0; i < s.children; ++i) w.child[i] = emit(s.child[i], Vec3{0, 0, 0});
                return place(w);
            }
            if (s.op == Op::Capsule) {
                ++rep.absorbed_by_capsule;
                Node w = s;
                w.a[0] += shift.x; w.a[1] += shift.y; w.a[2] += shift.z;
                w.a[3] += shift.x; w.a[4] += shift.y; w.a[5] += shift.z;
                return place(w);
            }
            if (s.op == Op::Plane) {
                // The half space behind the same normal, moved: `dot(p - t, n) - d` is
                // `dot(p, n) - (d + dot(t, n))`.
                ++rep.absorbed_by_plane;
                Node w = s;
                w.a[3] += shift.x * s.a[0] + shift.y * s.a[1] + shift.z * s.a[2];
                return place(w);
            }
        }

        if (is_pointwise(s.op)) {
            if (!is_zero(shift)) ++rep.pushed_through_pointwise;
            if (is_combine(s.op)) {
                std::vector<u32> in_parts;
                gather(n, s.op, s.a[0], in_parts);
                std::vector<u32> out_parts;
                out_parts.reserve(in_parts.size());
                for (u32 c : in_parts) out_parts.push_back(carry(c, shift));
                return assemble(s.op, s.a[0], std::move(out_parts));
            }
            Node w = s;
            for (u32 i = 0; i < s.children; ++i) w.child[i] = carry(s.child[i], shift);
            return place(w);
        }

        if (s.op == Op::Rotate) {
            // `rotate(child)(p - t)` is `child(M(p) - M(t))`, so the shift crosses the node by
            // being rotated itself. Exact in real arithmetic; the two roundings differ by an ulp.
            if (!is_zero(shift)) ++rep.pushed_through_transform;
            Node w = s;
            w.child[0] = carry(s.child[0], is_zero(shift) ? shift : rotate_like_eval(shift, s.a));
            return place(w);
        }
        if (s.op == Op::Scale) {
            if (!is_zero(shift)) ++rep.pushed_through_transform;
            const f64 sx = s.a[0] != 0.0 ? s.a[0] : 1.0;
            const f64 sy = s.a[1] != 0.0 ? s.a[1] : 1.0;
            const f64 sz = s.a[2] != 0.0 ? s.a[2] : 1.0;
            Node w = s;
            w.child[0] = carry(s.child[0], Vec3{shift.x / sx, shift.y / sy, shift.z / sz});
            return place(w);
        }
        if (s.op == Op::Mirror) {
            const u32 axis = static_cast<u32>(s.a[0]);
            if (!is_zero(shift) && axis_of(shift, axis) != 0.0) {
                // The fold takes an absolute value on this axis, and |x - t| is not |x| - t. A
                // shift with no component on the axis crosses it untouched; one with a component
                // cannot cross at all.
                ++rep.materialised;
                return place_translate(emit(n, Vec3{0, 0, 0}), shift);
            }
            if (!is_zero(shift)) ++rep.pushed_through_transform;
            Node w = s;
            w.child[0] = carry(s.child[0], shift);
            return place(w);
        }

        // Everything left folds, tiles, twists or draws a grain about the ORIGIN, so a shift
        // cannot be pushed into it and has to stay as a node of its own.
        if (!is_zero(shift)) {
            ++rep.materialised;
            return place_translate(emit(n, Vec3{0, 0, 0}), shift);
        }
        Node w = s;
        for (u32 i = 0; i < s.children; ++i) w.child[i] = emit(s.child[i], Vec3{0, 0, 0});
        return place(w);
    }
};

// The depth of every node of a field, in one forward pass.
//
// Every builder pushes a node after its children, so one forward pass is the whole of it — the same
// reason `Field::build_bounds` needs no recursion.
std::vector<u32> depths_of(const Field& f) {
    std::vector<u32> depth(f.size(), 0);
    for (usize i = 0; i < f.size(); ++i) {
        const Node& n = f.node(static_cast<u32>(i));
        u32 deepest = 0;
        for (u32 c = 0; c < n.children; ++c) {
            if (n.child[c] < i && depth[n.child[c]] > deepest) deepest = depth[n.child[c]];
        }
        depth[i] = deepest + 1;
    }
    return depth;
}

usize depth_from(const Field& f, u32 root) {
    if (root >= f.size()) return 0;
    return depths_of(f)[root];
}

// The deepest of a set. A stack has to hold the worst of them, not the first.
usize depth_from(const Field& f, const std::vector<u32>& roots) {
    if (f.size() == 0) return 0;
    const std::vector<u32> depth = depths_of(f);
    usize worst = 0;
    for (u32 r : roots) {
        if (r < f.size() && depth[r] > worst) worst = depth[r];
    }
    return worst;
}

void reach(const Field& f, u32 n, std::vector<u8>& seen, usize& count) {
    if (seen[n]) return;
    seen[n] = 1;
    ++count;
    const Node& s = f.node(n);
    for (u32 i = 0; i < s.children; ++i) reach(f, s.child[i], seen, count);
}

// How many nodes of `op` the set can reach, counting a shared subtree once.
//
// **The bounds check on `root` is not decoration and it is the crash this file was pulled out of
// `main` for.** `compile_field` used to count here BEFORE it checked that the root was in range, so
// a test asking about node `b + 99` of a one-node field indexed `seen` and `f.node` a hundred
// entries past the end: garbage `children`, garbage indices pushed onto the stack, and a SIGSEGV
// two frames later that read as a fault in the compiler rather than in the counting. A crashed
// doctest case aborts the process, so the suite reported 493 SKIPPED and one failure with ZERO
// failed assertions — quieter in the summary than a case that merely fails.
usize count_op(const Field& f, const std::vector<u32>& roots, Op op) {
    if (f.size() == 0) return 0;
    std::vector<u8> seen(f.size(), 0);
    usize total = 0;
    std::vector<u32> stack;
    for (u32 r : roots) {
        if (r < f.size()) stack.push_back(r);
    }
    while (!stack.empty()) {
        const u32 n = stack.back();
        stack.pop_back();
        if (seen[n]) continue;
        seen[n] = 1;
        const Node& s = f.node(n);
        if (s.op == op) ++total;
        for (u32 i = 0; i < s.children; ++i) {
            if (s.child[i] < f.size()) stack.push_back(s.child[i]);
        }
    }
    return total;
}

}  // namespace

usize expression_depth(const Field& f, u32 root) { return depth_from(f, root); }

usize expression_depth(const Field& f, const std::vector<u32>& roots) {
    return depth_from(f, roots);
}

usize reachable_nodes(const Field& f, u32 root) {
    if (root >= f.size()) return 0;
    std::vector<u8> seen(f.size(), 0);
    usize count = 0;
    reach(f, root, seen, count);
    return count;
}

usize reachable_nodes(const Field& f, const std::vector<u32>& roots) {
    if (f.size() == 0) return 0;
    std::vector<u8> seen(f.size(), 0);
    usize count = 0;
    for (u32 r : roots) {
        if (r < f.size()) reach(f, r, seen, count);
    }
    return count;
}

Field compile_field(const Field& in, const std::vector<u32>& roots,
                    const std::vector<u32>& names, CompileReport* report,
                    const CompileOptions& options) {
    CompileReport local;
    CompileReport& rep = report ? *report : local;
    rep = CompileReport{};
    rep.roots = roots;
    rep.root = roots.empty() ? 0 : roots[0];
    rep.names = names;
    rep.nodes_in_input = in.size();
    rep.nodes_before = reachable_nodes(in, roots);
    rep.depth_before = depth_from(in, roots);
    rep.translates_before = count_op(in, roots, Op::Translate);

    // The identity remap, so that every refusal below hands back something a caller can apply
    // blindly. A refusal that leaves `remap` empty is a refusal that crashes its caller.
    const auto refuse = [&](bool unhandled_op, Op which) {
        rep.ok = false;
        if (unhandled_op) rep.unhandled = which;
        rep.roots = roots;
        rep.root = roots.empty() ? 0 : roots[0];
        // The names come back as themselves for the same reason the roots and the remap do: the
        // field handed back IS the input, so every index the caller holds still means what it meant.
        rep.names = names;
        rep.remap.resize(in.size());
        for (usize i = 0; i < in.size(); ++i) rep.remap[i] = static_cast<u32>(i);
        rep.nodes_after = in.size();
        rep.depth_after = rep.depth_before;
        rep.translates_after = rep.translates_before;
    };

    // A root nothing can answer refuses the whole compilation rather than the one root, because a
    // caller holding a set holds them together: half a remapped Script is worse than none.
    for (u32 r : roots) {
        if (r >= in.size()) {
            refuse(false, Op::Constant);
            return in;
        }
    }
    if (roots.empty()) {
        refuse(false, Op::Constant);
        return in;
    }

    // Refuse before rewriting rather than during it. An op nobody has taught this pass about must
    // hand back the ORIGINAL field, because a compiler that drops a shape it did not recognise is
    // exactly the fault D204 is about: two readers of one description, one of them quietly short.
    {
        std::vector<u8> seen(in.size(), 0);
        std::vector<u32> stack(roots.begin(), roots.end());
        while (!stack.empty()) {
            const u32 n = stack.back();
            stack.pop_back();
            if (seen[n]) continue;
            seen[n] = 1;
            const Node& s = in.node(n);
            if (!can_rebuild(s.op)) {
                refuse(true, s.op);
                return in;
            }
            for (u32 i = 0; i < s.children; ++i) stack.push_back(s.child[i]);
        }
    }

    Compiler c(in, options);
    const usize n = in.size();
    c.refs.assign(n, 0);
    c.reads.assign(n, 0);
    c.has_param.assign(n, 0);
    c.cost.assign(n, 0);
    c.cost_known.assign(n, 0);
    c.emitted.assign(n, kNone);
    c.scratch.assign(n, 0);
    c.surveyed.assign(n, 0);
    c.classified.assign(n, 0);

    // A root counts as a reference to itself, and that is the one line that makes a SET safe.
    //
    // `refs` is what stops a subtree being specialised for one of its parents, and a root IS a
    // parent — an outside one. Without this, a paint rule's `test` that also sits inside the solid
    // reads `refs == 1`, so a translate above it could be pushed into it for the solid's sake and
    // the rule's own emission would then build a second copy of the whole subtree. The output grows
    // instead of shrinking, which is the one failure nobody sees in a screenshot.
    //
    // **The names are deliberately NOT surveyed here**, and that is the whole of why they can be
    // carried at all. A name is a parent too, so counting one would push every named intermediate
    // of a building to two references and stop `gather` flattening it — which is the entire 1.19x
    // spent on a diagnostic (D690). They are surveyed after the roots are emitted, where the count
    // can no longer change a decision that has already been taken.
    for (u32 r : roots) {
        ++c.refs[r];
        c.survey(r);
    }
    for (u32 r : roots) c.classify(r);

    // Every parameter slot, in the input's own slot order, so a dial by name still moves the
    // shape it moved before. A slot nothing reachable reads costs one node and is kept anyway:
    // a compiled clip whose dial list is shorter than the original's is a clip that looks right
    // until somebody drags one.
    c.slot_node.resize(in.parameter_count());
    for (usize slot = 0; slot < in.parameter_count(); ++slot) {
        c.slot_node[slot] = c.out.parameter(in.parameter_name(slot), in.parameter_value(slot));
        ++c.rep.parameter_nodes_kept;
    }

    std::vector<u32> out_roots;
    out_roots.reserve(roots.size());
    for (u32 r : roots) out_roots.push_back(c.emit(r, Vec3{0, 0, 0}));

    // --- the witness pass ---------------------------------------------------------------------
    //
    // Everything above is finished before a single name is looked at, and the order is the design.
    // The roots' output is node-for-node what it would have been with no names in the call at all,
    // because `refs` never saw one; what the names do now is READ the decisions rather than take
    // part in them.
    //
    // `emitted` is the table those decisions left behind, so a name whose node came through as
    // itself is a lookup and costs nothing. A name whose node dissolved — flattened into its
    // parent, folded to a number, or never reached because nothing but the name wanted it — is
    // re-emitted through the same one door, out of the same parts, and lands on a node that is
    // equivalent rather than identical. That is the honest answer to "which node in the OUTPUT is
    // the shape this name meant", and it is a construction and not a resemblance.
    const usize nodes_for_roots = c.out.size();
    // Every counter as the ROOTS left it. The witness pass runs through the same `place`, `gather`
    // and `assemble` as everything else and would otherwise add its own flattens and shares to the
    // building's tallies — so the table in `tests/test_compile.cpp` would move because somebody
    // asked a question, which is a measurement changing under its own instrument. Restored below.
    const CompileReport roots_only = c.rep;
    std::vector<u32> out_names(names.size(), kUnmapped);
    if (!names.empty()) {
        // Survey and classify EVERY name before emitting any of them. Two names over one subtree
        // have to see two references or the first would flatten what the second still needs, which
        // is the same duplication `refs` exists to refuse — the marks left by the roots' own walks
        // stop either from touching a count the building already settled.
        for (u32 nm : names) {
            if (nm >= in.size()) continue;
            ++c.refs[nm];
            c.survey(nm);
        }
        for (u32 nm : names) {
            if (nm < in.size()) c.classify(nm);
        }

        // The output may not grow past the input PLUS one node a name, and both halves of that are
        // measured rather than chosen.
        //
        // The file's promise is "it will not make the field bigger", and the flat version of it —
        // never past `in.size()` — was tried first and is WRONG. It refused `slabs` on
        // `clips/sampler.clip`: a 47-node clip whose roots compile to 43, where carrying all 28
        // names wants 48. One node over the input, and the arm came back with `--part slabs` saying
        // "does not name anything" while the walk was 40 nodes → 43. **A name is not on the walk**,
        // so paying an array slot for one costs nothing per sample, and refusing it to hold a total
        // under a line is spending the only thing anybody notices to protect a number nobody reads.
        //
        // One node a name is what a name is WORTH: a name that dissolved dissolved into its parent,
        // and putting it back is the one combine node that says where it ended. A name that wants
        // more than that is one the rewrite genuinely spread over many places, and refusing it is
        // the guard still doing its job — loudly, in `names_dropped_over_budget`.
        const usize budget = in.size() + names.size();
        for (usize i = 0; i < names.size(); ++i) {
            const u32 nm = names[i];
            if (nm >= in.size()) {
                ++c.rep.names_dropped_out_of_range;
                continue;
            }
            const bool free_of_charge = c.emitted[nm] != kNone;
            if (!free_of_charge) {
                if (!c.rebuildable(nm)) {
                    ++c.rep.names_dropped_unhandled;
                    continue;
                }
                if (c.out.size() + c.unemitted_size(nm) > budget) {
                    ++c.rep.names_dropped_over_budget;
                    continue;
                }
            }
            out_names[i] = c.emit(nm, Vec3{0, 0, 0});
            ++c.rep.names_kept;
            if (free_of_charge) {
                ++c.rep.names_answered_as_themselves;
            } else {
                ++c.rep.names_rebuilt;
            }
        }
    }

    const usize nodes_with_names = c.out.size();

    // The rewrite counters describe the building; the name counters describe the names. Put back.
    {
        const usize kept_n = c.rep.names_kept;
        const usize as_themselves = c.rep.names_answered_as_themselves;
        const usize rebuilt = c.rep.names_rebuilt;
        const usize unhandled_n = c.rep.names_dropped_unhandled;
        const usize over_budget = c.rep.names_dropped_over_budget;
        const usize out_of_range = c.rep.names_dropped_out_of_range;
        c.rep = roots_only;
        c.rep.names_kept = kept_n;
        c.rep.names_answered_as_themselves = as_themselves;
        c.rep.names_rebuilt = rebuilt;
        c.rep.names_dropped_unhandled = unhandled_n;
        c.rep.names_dropped_over_budget = over_budget;
        c.rep.names_dropped_out_of_range = out_of_range;
    }

    // With ONE root it is also the last node, so a caller who never looked at the report can still
    // find it. Sharing can steal that — an expression identical to the root's may already sit
    // inside it — so in that one case the root node is placed a second time, unshared.
    //
    // With several roots the promise cannot hold for all of them and is not made for any: they come
    // back through `report->roots` or not at all.
    //
    // With NAMES in the call it cannot hold either — a name is emitted after every root and is a
    // node of the same field — so it is not attempted, rather than attempted and then quietly
    // untrue. `report->roots` is the answer in both of those cases.
    if (names.empty() && out_roots.size() == 1 && out_roots[0] + 1 != c.out.size()) {
        const Node top = c.out.node(out_roots[0]);
        if (top.op != Op::Parameter) out_roots[0] = c.construct(top);
    }

    const usize kept = c.rep.parameter_nodes_kept;
    const usize before = rep.nodes_before;
    const usize depth_in = rep.depth_before;
    const usize trans_in = rep.translates_before;
    rep = c.rep;
    rep.roots = out_roots;
    rep.root = out_roots[0];
    rep.names = out_names;
    rep.ok = true;
    rep.nodes_in_input = in.size();
    rep.nodes_before = before;
    rep.depth_before = depth_in;
    rep.translates_before = trans_in;
    rep.parameter_nodes_kept = kept;
    rep.nodes_after = c.out.size();
    // What the ROOTS came to, which is the figure `nodes_before` is the comparison for. A name is
    // not on the walk — nothing reaches one from `solid` — so charging the walk for it would report
    // a compiler that had stopped compressing when what it had done was start answering `--part`.
    rep.nodes_for_roots = nodes_for_roots;
    rep.name_nodes_added = nodes_with_names - nodes_for_roots;
    // Both taken over the roots alone, and for the same reason.
    rep.depth_after = depth_from(c.out, out_roots);
    rep.translates_after = count_op(c.out, out_roots, Op::Translate);

    // What every OTHER index the caller was holding became. `emitted` is filled by the one door
    // every shift-free emission goes through, so an entry here is the output node that answers what
    // that input node answered — exact, not a guess. A node that was flattened into its parent,
    // folded to a number or never reached has no answer of its own and stays `kUnmapped`.
    rep.remap.assign(in.size(), kUnmapped);
    for (usize i = 0; i < in.size(); ++i) {
        if (c.emitted[i] != kNone) rep.remap[i] = c.emitted[i];
    }
    for (usize i = 0; i < roots.size(); ++i) rep.remap[roots[i]] = out_roots[i];
    // A name that WAS answered belongs in the remap too, so a caller holding one index does not
    // have to know which of the two lists it came out of.
    for (usize i = 0; i < names.size(); ++i) {
        if (out_names[i] != kUnmapped && names[i] < in.size()) rep.remap[names[i]] = out_names[i];
    }
    return c.out;
}

Field compile_field(const Field& in, const std::vector<u32>& roots, CompileReport* report,
                    const CompileOptions& options) {
    return compile_field(in, roots, std::vector<u32>{}, report, options);
}

Field compile_field(const Field& in, u32 root, CompileReport* report,
                    const CompileOptions& options) {
    return compile_field(in, std::vector<u32>{root}, std::vector<u32>{}, report, options);
}

}  // namespace forge
}  // namespace ws
