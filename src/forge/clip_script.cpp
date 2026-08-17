#include "forge/clip_script.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <cstdio>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>

#include "core/log.hpp"
#include "world/tags.hpp"



namespace ws {

// A displacement smaller than a voxel cannot be geometry. It can only be dither.
//
// Moving a surface by a fraction of a voxel is a thing an SDF can express and a voxel grid cannot.
// What the sampler does with it is decide, voxel by voxel, whether the shifted surface now falls on
// the far side of that voxel's centre -- so instead of a surface moving smoothly, individual voxels
// switch on and off wherever the pattern crosses the threshold. On a curved or diagonal surface that
// is nearly harmless, because the surface already cuts the grid at every angle. On a FLAT
// AXIS-ALIGNED one it is at its worst, because the whole face lies on the grid at once and the
// entire wall dithers together.
//
// The facility is flat axis-aligned walls almost everywhere, and it carried
//
//     let all = displace { hollowed grain_fine } amount=0.012
//     # ... which is under half a voxel at full detail, so it moves no wall
//
// At metre 32 that is 0.38 of a voxel. The comment states the danger and reads it as the safety
// argument: being under a voxel is not what makes it harmless, it is precisely what makes it
// dither, because a displacement too small to move a wall is still large enough to flip the voxels
// the wall is made of. It rendered as a raspy speckle over every flat surface, and once the
// rasterizer gained real sun-and-sky lighting it became black pepper, because a flipped voxel
// exposes a face pointing a different way and a differently-facing face is now several times
// darker.
//
// Below the cutoff the displacement is dropped rather than scaled up. Scaling it up would be
// inventing detail nobody asked for; the shape the author wrote is simply finer than the grid they
// chose to sample it on, and the honest answer at that resolution is the undisplaced surface.
f64 usable_displacement(f64 amount, i32 voxels_per_metre, const char* what) {
    if (voxels_per_metre <= 0) return amount;
    // HALF a voxel, and the number is derived rather than chosen.
    //
    // A surface sitting at a voxel's centre has half a voxel to travel before it reaches the next
    // one. A displacement smaller than that can therefore never move a grid-aligned surface a whole
    // cell anywhere along it -- all it can do is push voxels back and forth across their own
    // thresholds, which is dither by definition. At or above a half, the surface genuinely
    // relocates somewhere, and what it does in between is a real deformation with ragged edges
    // rather than noise pretending to be one.
    //
    // Three quarters was tried first and is too strict: it swallowed a 0.72-voxel weathering
    // deformation, which is very nearly a whole cell and unambiguously real. A test caught it.
    const f64 kLeastUseful = 0.5;
    const f64 voxels = std::abs(amount) * static_cast<f64>(voxels_per_metre);
    if (voxels >= kLeastUseful) return amount;
    if (voxels > 1.0e-9) {
        WS_LOG_WARN("clip",
                    "{} amount {} m is {:.2f} of a voxel at metre {} -- too small to move a "
                    "surface, large enough to dither one. Dropped.",
                    what, amount, voxels, voxels_per_metre);
    }
    return 0.0;
}
namespace forge {

namespace {

// --- tokens ---------------------------------------------------------------------------
//
// Whitespace separated, with braces, equals and commas as tokens of their own so that
// `rgb=120,120,116` and `union { a b }` both fall out without the tokenizer knowing what either
// means. A comment runs to the end of its line.
struct Token {
    std::string text;
    u32 line = 0;
    bool starts_line = false;
};

std::vector<Token> tokenize(const std::string& source) {
    std::vector<Token> tokens;
    u32 line = 1;
    bool at_line_start = true;
    usize i = 0;
    while (i < source.size()) {
        const char c = source[i];
        if (c == '#') {
            while (i < source.size() && source[i] != '\n') ++i;
            continue;
        }
        if (c == '\n') {
            ++line;
            at_line_start = true;
            ++i;
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\r') {
            ++i;
            continue;
        }
        Token token;
        token.line = line;
        token.starts_line = at_line_start;
        at_line_start = false;
        if (c == '{' || c == '}' || c == '=' || c == ',') {
            token.text = std::string(1, c);
            ++i;
        } else {
            const usize begin = i;
            while (i < source.size()) {
                const char d = source[i];
                if (d == ' ' || d == '\t' || d == '\r' || d == '\n' || d == '{' || d == '}' ||
                    d == '=' || d == ',' || d == '#') {
                    break;
                }
                ++i;
            }
            token.text = source.substr(begin, i - begin);
        }
        tokens.push_back(token);
    }
    return tokens;
}

bool is_number(const std::string& s) {
    if (s.empty()) return false;
    char* end = nullptr;
    std::strtod(s.c_str(), &end);
    return end != nullptr && *end == '\0';
}

u32 axis_from(const std::string& s) {
    if (s == "x" || s == "X" || s == "0") return 0;
    if (s == "y" || s == "Y" || s == "1") return 1;
    if (s == "z" || s == "Z" || s == "2") return 2;
    return 3;
}

// --- the mouldings ---------------------------------------------------------------------------
//
// The classical orders are not made of shapes, they are made of *sections*: a dozen curves, drawn
// once each in a rectangle, repeated at every scale of the building and either run straight along
// a cornice or turned about an axis to make a base, a bead, a baluster or a dome. So they are
// built here, out of the field operations that already exist, rather than added as new kinds of
// node — there is nothing a cyma can do that an intersection of a box and two ellipses cannot,
// and one fewer node type is one fewer thing that can be wrong about a distance.
//
// Every one of them is given as two opposite corners, exactly like `box`, and the *order of the
// corners is the orientation*: the first corner is in the stone, the second is in the air. Swap
// them and the curve turns over. That is the whole of the interface, and it is why there is no
// `flip=` key: an ovolo that swells toward the top and one that swells toward the bottom are the
// same four numbers written in a different order.

// A point built from the moulding's own three directions: across the face, up it, and along the
// run. Which world axes those are is the `run=` key's business and nothing else's.
Vec3 in_frame(u32 proj, u32 high, u32 run, f64 p, f64 q, f64 r) {
    f64 v[3] = {0, 0, 0};
    v[proj] = p;
    v[high] = q;
    v[run] = r;
    return Vec3{v[0], v[1], v[2]};
}

// An elliptical cylinder lying along the run: the only curve any of these need.
//
// Built as a round cylinder stretched, and stretched up from the *smaller* radius rather than
// down from a unit one. `scale` multiplies the distance it returns by the smallest of its
// factors, so scaling a one-metre cylinder down to a five-centimetre bead would report every
// distance near it at a twentieth of the truth — correct, and slow enough that the sampler would
// crawl round every moulding in the building. Scaling up from the smaller radius makes both
// factors at least one and the distance exact.
u32 elliptic_run(Field& f, u32 proj, u32 high, u32 run, f64 cp, f64 cq, f64 rp, f64 rq, f64 cr,
                 f64 half_run) {
    rp = std::abs(rp);
    rq = std::abs(rq);
    if (rp <= 0.0 || rq <= 0.0) return f.constant(1e30);
    const f64 base = std::min(rp, rq);
    const f64 sp = rp / base;
    const f64 sq = rq / base;
    const u32 round = f.cylinder(in_frame(proj, high, run, cp / sp, cq / sq, cr), base, half_run,
                                 run);
    if (sp == 1.0 && sq == 1.0) return round;
    f64 s[3] = {1.0, 1.0, 1.0};
    s[proj] = sp;
    s[high] = sq;
    return f.scale(round, {s[0], s[1], s[2]});
}

// One moulding, as a solid section between two corners.
//
//   fillet         a plain square band; the thing that separates two curves
//   ovolo          a convex quarter round, full at the first corner's end
//   cavetto        a concave quarter hollow, full at the first corner's end
//   bead/astragal  a half round on a flat back — the moulding the orders call a torus
//   scotia         a deep hollow of two arcs, deepest above the middle, as it is drawn
//   cyma           the S: convex at the first corner's end, hollow at the second's
//   cyma_reversa   the same S turned over
u32 build_moulding(Field& f, const std::string& kind, f64 p0, f64 q0, f64 p1, f64 q1, f64 r0,
                   f64 r1, u32 proj, u32 high, u32 run) {
    const f64 w = p1 - p0;      // outward, away from the face the moulding is stuck to
    const f64 h = q1 - q0;      // along the moulding's height, away from the solid corner
    const f64 rw = std::abs(w);
    const f64 rh = std::abs(h);
    const f64 pm = p0 + w * 0.5;
    const f64 qm = q0 + h * 0.5;
    const f64 sh = (h < 0.0) ? -1.0 : 1.0;
    const f64 cr = (r0 + r1) * 0.5;
    const f64 half_run = std::abs(r1 - r0) * 0.5;

    const u32 rect = f.box(in_frame(proj, high, run, pm, qm, cr),
                           in_frame(proj, high, run, rw * 0.5, rh * 0.5, half_run), 0.0);
    if (kind == "fillet") return rect;

    // The half of the section on one side of a line across it. `sign` of +1 keeps the side the
    // coordinate is smaller on, so passing the section's own sense of "toward the first corner"
    // makes the same expression work whichever way round the corners were written.
    const auto beyond_high = [&](f64 at, f64 sign) {
        return f.plane(in_frame(proj, high, run, 0.0, sign, 0.0), sign * at);
    };
    const auto disc = [&](f64 cp, f64 cq, f64 rp, f64 rq) {
        return elliptic_run(f, proj, high, run, cp, cq, rp, rq, cr, half_run);
    };

    if (kind == "ovolo") {
        return f.intersect({rect, disc(p0, q0, rw, rh)});
    }
    if (kind == "cavetto") {
        return f.subtract({rect, disc(p1, q1, rw, rh)});
    }
    if (kind == "bead" || kind == "astragal") {
        return f.intersect({rect, disc(p0, qm, rw, rh * 0.5)});
    }
    if (kind == "scotia") {
        // Two arcs meeting at the deepest point, which sits above the middle — the lower sweep is
        // the longer one, and that asymmetry is what tells a scotia from a plain hollow at a
        // glance. Both arcs reach the back of the section, so the deepest point is at the first
        // corner's face and the core the moulding is cut into has to stand at least there.
        const f64 deep = q0 + h * 0.6;
        const u32 lower = f.intersect({disc(p1, deep, rw, rh * 0.6), beyond_high(deep, sh)});
        const u32 upper = f.intersect({disc(p1, deep, rw, rh * 0.4), beyond_high(deep, -sh)});
        return f.subtract({rect, lower, upper});
    }
    if (kind == "cyma" || kind == "cyma_reversa") {
        // The S, drawn the way a draughtsman draws it: two arcs of the section's half-width, one
        // centred on the front face and one on the back, both at mid height. That puts the tangent
        // flat where the curve meets the members above and below — so a cyma lies down on a fillet
        // without a kink — and upright where the two arcs meet, which is the steep middle that
        // makes the profile read as an S and not as a bevel.
        //
        // Built with the arcs on the wrong axis first, which gave a curve flat in the middle and
        // steep at the ends: a perfectly smooth S, and the wrong one, and only visible by asking
        // where the section's face is a fifth of the way up.
        const bool reversed = (kind == "cyma_reversa");
        const f64 to_swell = reversed ? -sh : sh;   // toward the end the curve is full at
        const u32 outer = disc(p1, qm, rw * 0.5, rh * 0.5);
        const u32 inner = disc(p0, qm, rw * 0.5, rh * 0.5);
        const u32 swelling =
            f.intersect({f.subtract({rect, outer}), beyond_high(qm, to_swell)});
        const u32 hollowing = f.intersect({rect, inner, beyond_high(qm, -to_swell)});
        return f.unite({swelling, hollowing});
    }
    return 0;
}

bool is_moulding(const std::string& s) {
    return s == "fillet" || s == "ovolo" || s == "cavetto" || s == "bead" || s == "astragal" ||
           s == "scotia" || s == "cyma" || s == "cyma_reversa";
}

// --- branch: everything that forks ------------------------------------------------------------
//
// A trunk that splits into limbs that split again. Trees and their bare winter branches, roots
// breaking a pavement, the scrolled bar of a gate, a candelabrum, a coral, a vein of ore, the
// ribs of a fan vault: one shape, and this repository builds every one of them a capsule at a
// time. `clips/facility/terrace.clip` has four citrus trees and each is a single straight capsule
// with a lumpy ball on top, because eleven limbs written out by hand is eleven lines of arithmetic
// an author has to do in their head and then cannot re-proportion afterwards.
//
// # Why this is a MACRO and not a node
//
// Because `build_moulding` above already settled the argument: "there is nothing a cyma can do
// that an intersection of a box and two ellipses cannot, and one fewer node type is one fewer
// thing that can be wrong about a distance." A branch is a chain of capsules, a capsule is an
// exact distance, and a union of exact distances is exact. Written as a node instead, every
// evaluation would have to REGENERATE the tree — recompute forty rotations to find out that the
// point is nowhere near any of them — where as nodes the boxes do that for nothing.
//
// # What it costs, and it is nodes rather than time
//
// `segments * (count^levels - 1) / (count - 1)` capsules, which is 30 for the defaults and 4372
// for `levels=7 count=4 segments=3`. The parser refuses anything over kMostCapsules and says so
// with the arithmetic, because the failure otherwise is a clip that takes a minute to parse.
//
// Time is bounded by the boxes: the capsules are united into a BALANCED tree rather than the chain
// `Field::unite` builds, so a point outside the tree's box is rejected in one test and a point
// inside it descends about log4(n) levels. A chain would have made every sample near the tree walk
// all n unions in turn, which is the difference between a tree costing what a wall costs and a
// tree costing what the rest of the clip costs.

constexpr usize kMostCapsules = 1500;

struct BranchPlan {
    Vec3 base{0.0, 0.0, 0.0};
    u32 axis = 1;
    f64 length = 2.0;
    f64 radius = 0.08;
    u32 levels = 4;
    u32 count = 2;
    f64 spread = 0.10;    // how far a limb forks off its parent, in turns
    f64 lean = 0.25;      // how strongly a limb is pulled back toward the growth axis, 0 to 1
    f64 shrink = 0.72;    // length multiplier per level
    f64 taper = 0.62;     // radius multiplier per level
    u32 segments = 2;     // capsules per limb, so a limb narrows along its own length
    u32 seed = 1;
};

usize branch_capsules(const BranchPlan& plan) {
    usize limbs = 0;
    usize row = 1;
    for (u32 level = 0; level < plan.levels; ++level) {
        limbs += row;
        if (limbs > kMostCapsules) return limbs * plan.segments;   // already too many
        row *= plan.count;
    }
    return limbs * plan.segments;
}

Vec3 cross_of(Vec3 a, Vec3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

// The same hash the field's own noise uses, so a branch is identical on every machine for the same
// reason the grain is. Keyed on the LIMB's identity rather than on a running counter, so adding a
// level does not reshuffle the limbs that were already there.
f64 branch_unit(u32 id, u32 salt) {
    u32 x = id * 0x9e3779b9u ^ (salt * 0x85ebca6bu);
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return static_cast<f64>(x) * (1.0 / 4294967296.0);
}

void grow_limb(Field& f, const BranchPlan& plan, Vec3 from, Vec3 dir, f64 length, f64 radius,
               u32 level, u32 id, std::vector<u32>& out) {
    if (length <= 0.0 || radius <= 0.0 || out.size() >= kMostCapsules) return;

    const f64 tip_radius = radius * plan.taper;
    const u32 pieces = (plan.segments > 0) ? plan.segments : 1u;
    Vec3 at = from;
    for (u32 s = 0; s < pieces; ++s) {
        const f64 t0 = static_cast<f64>(s) / static_cast<f64>(pieces);
        const f64 t1 = static_cast<f64>(s + 1) / static_cast<f64>(pieces);
        const Vec3 next = from + dir * (length * t1);
        // A capsule carries ONE radius, so each piece takes the mean of its own two ends. The step
        // between pieces is what a taper looks like at a voxel and a half, which is what these are.
        const f64 r0 = radius + (tip_radius - radius) * t0;
        const f64 r1 = radius + (tip_radius - radius) * t1;
        out.push_back(f.capsule(at, next, (r0 + r1) * 0.5));
        at = next;
    }
    if (level + 1 >= plan.levels) return;

    // A frame across the limb, from whichever world axis is least parallel to it — taking the
    // nearest one instead gives a degenerate cross product for a vertical trunk, which is the
    // commonest case there is.
    const Vec3 away = (std::abs(dir.y) < 0.9) ? Vec3{0, 1, 0} : Vec3{1, 0, 0};
    const Vec3 side = normalise(cross_of(dir, away));
    const Vec3 other = normalise(cross_of(dir, side));

    Vec3 grow{0, 0, 0};
    if (plan.axis == 0) grow.x = 1.0;
    else if (plan.axis == 1) grow.y = 1.0;
    else grow.z = 1.0;

    // Turns, like every angle an author writes in this language — `rotate`, `around`, `arc` and
    // `revolve` all take them, so `spread=` does too and nobody has to remember a second unit.
    constexpr f64 kTurn = 6.283185307179586;

    const Vec3 tip = from + dir * length;
    for (u32 c = 0; c < plan.count; ++c) {
        const u32 kid = id * 7919u + c + 1u;
        // Spread evenly round the parent and then jogged, so two limbs never leave at the same
        // bearing and the whole fork never looks turned from a template.
        const f64 spacing = 1.0 / static_cast<f64>(plan.count);
        const f64 azimuth =
            (static_cast<f64>(c) + branch_unit(kid, 1u)) * spacing * kTurn;
        const f64 angle = plan.spread * kTurn * (0.6 + 0.8 * branch_unit(kid, 2u));
        Vec3 out_dir = dir * std::cos(angle) +
                       (side * std::cos(azimuth) + other * std::sin(azimuth)) * std::sin(angle);
        // ...and then pulled back toward the growth axis. This one line is the difference between
        // a fractal and a tree: real limbs turn back toward the light, so a bough leaves its trunk
        // at a wide angle and then rises, which is what an eye reads as growth rather than as
        // geometry.
        out_dir = normalise(out_dir + grow * plan.lean);
        const f64 kid_length = length * plan.shrink * (0.82 + 0.36 * branch_unit(kid, 3u));
        grow_limb(f, plan, tip, out_dir, kid_length, tip_radius, level + 1, kid, out);
    }
}

// A balanced tree of unions rather than the left-leaning chain `Field::unite` builds.
//
// `unite` folds four at a time into a chain, so the box on every node of it is the box round
// everything below — which culls nothing, and a point near a tree of a thousand capsules would
// walk all two hundred and fifty of its unions. Grouped four at a time from the bottom instead,
// the depth is log4(n) and each group's box is tight round four neighbouring limbs, because the
// limbs come out of the walk above in depth-first order and depth-first order is nearly spatial.
u32 unite_tree(Field& f, std::vector<u32> parts) {
    if (parts.empty()) return f.constant(1e30);
    while (parts.size() > 1) {
        std::vector<u32> next;
        next.reserve((parts.size() + 3) / 4);
        for (usize i = 0; i < parts.size(); i += 4) {
            const usize end = std::min(i + 4, parts.size());
            next.push_back(f.unite(std::vector<u32>(parts.begin() + static_cast<isize>(i),
                                                    parts.begin() + static_cast<isize>(end))));
        }
        parts.swap(next);
    }
    return parts[0];
}

// --- the parser -------------------------------------------------------------------------

class Parser {
public:
    Parser(const std::vector<Token>& tokens, Script& script, VoxelTypeTable& types,
           const TagRegistry& tags)
        : tokens_(tokens), script_(script), types_(types), tags_(tags) {}

    void run() {
        while (at_ < tokens_.size()) {
            const usize before = at_;
            statement();
            if (at_ == before) ++at_;   // never spin on a token nothing consumed
        }
    }

private:
    // --- token access ---------------------------------------------------------------------
    bool done() const { return at_ >= tokens_.size(); }

    // PAST THE END READS AS AN EMPTY TOKEN, and this is a fix for a crash rather than a courtesy.
    //
    // `peek()` was the one accessor here that did not test `done()` first -- `take`, `line` and
    // `at_new_statement` all do -- and the caller cannot be relied on to have tested it either,
    // because `at_` moves during the call it is checked around. The way it happened:
    // `block()` enters its loop having checked `!done()`, calls `expression()`, and somewhere
    // below that a nested `block()` hits the depth limit and sets `at_ = tokens_.size()` to
    // abandon the file. `expression()` returns false, and the very next thing `block()` does is
    // `peek().text` for the error message -- one element past the end of the vector.
    //
    // That is a heap READ out of bounds, not a stack overflow, which is why it looked like it did:
    // AddressSanitizer names it exactly, but without ASan whether it crashes depends on what
    // happens to sit after the token array. Nesting 90, 95, 96, 100, 104, 110 and 1000 deep all
    // segfaulted while 80, 120, 128 and 4000 came back clean, and the same input in a different
    // process was fine. D666 recorded it honestly as "seen rather than diagnosed" and left the
    // depth guard in as a bound; the depth guard was right, and it was also what pushed `at_` off
    // the end.
    //
    // Returning an empty token means every reader past the end sees a token with no text, which no
    // branch matches, so the parser reports and stops instead of reading memory it does not own.
    const Token& peek() const {
        static const Token kEnd{};
        return done() ? kEnd : tokens_[at_];
    }
    bool at_new_statement() const { return done() || tokens_[at_].starts_line; }
    std::string take() { return done() ? std::string() : tokens_[at_++].text; }
    u32 line() const { return done() ? (tokens_.empty() ? 0 : tokens_.back().line) : peek().line; }

    void fail(const std::string& message) {
        script_.errors.push_back(ScriptError{line(), message});
    }

    // Everything up to the start of the next line, which is one statement's worth.
    void skip_statement() {
        while (!done() && !tokens_[at_].starts_line) ++at_;
    }

    // --- values -------------------------------------------------------------------------
    //
    // A number, or the name of a parameter. Both are usable anywhere a number is, and that is
    // what lets a clip expose a dial: writing `height` instead of `2.4` changes nothing about
    // how the file reads and everything about whether it can move afterwards.
    bool value(f64& out) {
        if (done() || peek().text == "{" || peek().text == "}") return false;
        const std::string t = peek().text;
        if (is_number(t)) {
            out = std::strtod(t.c_str(), nullptr);
            ++at_;
            return true;
        }
        auto it = parameters_.find(t);
        if (it != parameters_.end()) {
            // Unless it is the left side of a `key=value`, in which case it is a key that
            // happens to share a name with a parameter — which is not a coincidence but the
            // normal case: `run=run` says "the step run is the parameter called run", and it is
            // exactly what an author writes. Without this lookahead the name is swallowed as a
            // positional argument and the equals sign that follows becomes a statement of its
            // own, which is how it announces itself.
            if (at_ + 1 < tokens_.size() && tokens_[at_ + 1].text == "=") return false;
            out = script_.field.get_parameter(t.c_str(), 0.0);
            ++at_;
            return true;
        }
        return false;
    }

    f64 value_or(f64 fallback) {
        f64 v = fallback;
        value(v);
        return v;
    }

    // key=value pairs, gathered before the expression is built so order does not matter.
    struct Keys {
        std::map<std::string, std::vector<f64>> numbers;
        std::map<std::string, std::string> words;

        f64 number(const std::string& key, f64 fallback) const {
            auto it = numbers.find(key);
            return (it != numbers.end() && !it->second.empty()) ? it->second[0] : fallback;
        }
        // One value of a list, for the keys written `stretch=8,1,1`. A key given ONE number means
        // that number on every axis, which is what `size=` already means and what an author writes
        // when they want a grain twice as coarse in every direction.
        f64 number_at(const std::string& key, usize index, f64 fallback) const {
            auto it = numbers.find(key);
            if (it == numbers.end() || it->second.empty()) return fallback;
            if (it->second.size() == 1) return it->second[0];
            return (index < it->second.size()) ? it->second[index] : fallback;
        }
        bool has(const std::string& key) const {
            return numbers.count(key) != 0 || words.count(key) != 0;
        }
        std::string word(const std::string& key, const std::string& fallback) const {
            auto it = words.find(key);
            return (it != words.end()) ? it->second : fallback;
        }
    };

    // Reads `key=value` and `key=a,b,c` until something that is not one.
    void keys_into(Keys& keys) {
        while (!done() && !at_new_statement()) {
            if (at_ + 1 >= tokens_.size() || tokens_[at_ + 1].text != "=") break;
            const std::string key = tokens_[at_].text;
            at_ += 2;   // the name and the equals
            std::vector<f64> numbers;
            std::string word;
            while (true) {
                f64 v = 0.0;
                if (value(v)) {
                    numbers.push_back(v);
                } else if (!done() && peek().text != "," && peek().text != "{" &&
                           peek().text != "}") {
                    word = take();
                } else {
                    break;
                }
                if (!done() && peek().text == ",") {
                    ++at_;
                    continue;
                }
                break;
            }
            if (!numbers.empty()) keys.numbers[key] = numbers;
            if (!word.empty()) keys.words[key] = word;
        }
    }

    // --- expressions -----------------------------------------------------------------------

    struct DepthGuard {
        explicit DepthGuard(u32& counter) : counter_(counter) { ++counter_; }
        ~DepthGuard() { --counter_; }
        DepthGuard(const DepthGuard&) = delete;
        DepthGuard& operator=(const DepthGuard&) = delete;
        u32& counter_;
    };

    // A name already bound by `let`, or a fresh call.
    bool expression(u32& out) {
        if (done()) return false;
        const std::string head = peek().text;
        auto bound = bindings_.find(head);
        if (bound != bindings_.end()) {
            // A bare name, unless it is followed by arguments, in which case it was meant as a
            // call and the author has shadowed a builder — which they are allowed to do.
            ++at_;
            out = bound->second;
            return true;
        }
        return call(out);
    }

    // The children of a `{ ... }` block.
    //
    // How deep braces may nest, and it is here because nothing bounded it.
    //
    // `block` calls `expression` calls `call` calls `block`, once per `{`, and a file whose braces
    // have desynchronised nests one level for every `{` left in it. The clip viewer's baker died
    // this way — SIGSEGV while parsing a fragment on its own, with gdb showing hundreds of frames
    // of exactly that cycle and nothing else on the stack. It was reproducible in that run and it
    // does NOT reproduce from the same file in a fresh process, so what is written down here is
    // what was seen rather than a diagnosis: the recursion had no bound, and a parser whose whole
    // contract is that it collects errors and carries on must not be able to end a process.
    //
    // Sixty-four because nothing anybody writes nests eight deep, so this is a limit only a
    // mistake can reach, and reaching it says which mistake in the author's own terms.
    static constexpr u32 kMaxBlockDepth = 64;

    std::vector<u32> block() {
        std::vector<u32> parts;
        if (done() || peek().text != "{") return parts;
        if (depth_ >= kMaxBlockDepth) {
            fail("blocks nested more than 64 deep -- the braces above this are unbalanced");
            at_ = tokens_.size();   // there is nothing left to say about this file
            return parts;
        }
        const DepthGuard guard(depth_);
        ++at_;
        while (!done() && peek().text != "}") {
            u32 child = 0;
            if (!expression(child)) {
                fail("expected a shape or a name inside the braces, found '" + peek().text + "'");
                ++at_;
                continue;
            }
            parts.push_back(child);
        }
        if (!done() && peek().text == "}") ++at_;
        return parts;
    }

    bool call(u32& out);

    // --- statements --------------------------------------------------------------------------
    void statement();

    const std::vector<Token>& tokens_;
    Script& script_;
    VoxelTypeTable& types_;
    const TagRegistry& tags_;
    usize at_ = 0;
    u32 depth_ = 0;   // how many `{ }` blocks deep the parser is; see kMaxBlockDepth
    std::map<std::string, u32> bindings_;
    std::map<std::string, VoxelTypeId> materials_;
    std::map<std::string, bool> parameters_;
};

bool Parser::call(u32& out) {
    if (done()) return false;
    const std::string head = take();
    Field& f = script_.field;

    // Positional numbers first, then keys. Nearly every shape reads better with its position
    // written plainly and its size named — `box -4 0 -4 4 3 4 round=0.05`.
    std::vector<f64> args;
    f64 v = 0.0;
    while (value(v)) args.push_back(v);
    Keys keys;
    keys_into(keys);
    const auto arg = [&](usize index, f64 fallback) {
        return (index < args.size()) ? args[index] : fallback;
    };

    if (head == "sphere") {
        out = f.sphere({arg(0, 0), arg(1, 0), arg(2, 0)}, keys.number("r", arg(3, 1.0)));
        return true;
    }
    if (head == "box") {
        // Written as two opposite corners, because that is how a room is described. Converted to
        // centre and half extent here so the author never has to.
        const Vec3 lo{arg(0, 0), arg(1, 0), arg(2, 0)};
        const Vec3 hi{arg(3, 1), arg(4, 1), arg(5, 1)};
        const Vec3 centre{(lo.x + hi.x) * 0.5, (lo.y + hi.y) * 0.5, (lo.z + hi.z) * 0.5};
        const Vec3 half{std::abs(hi.x - lo.x) * 0.5, std::abs(hi.y - lo.y) * 0.5,
                        std::abs(hi.z - lo.z) * 0.5};
        out = f.box(centre, half, keys.number("round", 0.0));
        return true;
    }
    if (head == "cylinder") {
        out = f.cylinder({arg(0, 0), arg(1, 0), arg(2, 0)}, keys.number("r", 1.0),
                         keys.number("h", 1.0) * 0.5,
                         axis_from(keys.word("axis", "y")) % 3u);
        return true;
    }
    if (head == "capsule") {
        out = f.capsule({arg(0, 0), arg(1, 0), arg(2, 0)}, {arg(3, 0), arg(4, 1), arg(5, 0)},
                        keys.number("r", 0.25));
        return true;
    }
    if (head == "torus") {
        out = f.torus({arg(0, 0), arg(1, 0), arg(2, 0)}, keys.number("ring", 1.0),
                      keys.number("tube", 0.25), axis_from(keys.word("axis", "y")) % 3u);
        return true;
    }
    if (head == "arc") {
        // A torus that goes part of the way round, with a round cap at each end — the arch ring,
        // the curved handrail, the hoop of a crown. `from` and `to` are turns, measured from the
        // first cross-axis and running the way `around` goes; omit them and it is a whole torus.
        const f64 from = keys.number("from", 0.0);
        out = f.arc({arg(0, 0), arg(1, 0), arg(2, 0)}, keys.number("ring", 1.0),
                    keys.number("tube", 0.25), axis_from(keys.word("axis", "y")) % 3u, from,
                    keys.number("to", from + 1.0));
        return true;
    }
    if (head == "cone") {
        out = f.cone({arg(0, 0), arg(1, 0), arg(2, 0)}, keys.number("r", 1.0),
                     keys.number("h", 1.0), axis_from(keys.word("axis", "y")) % 3u);
        return true;
    }
    if (head == "plane") {
        out = f.plane({arg(0, 0), arg(1, 1), arg(2, 0)}, keys.number("at", arg(3, 0.0)));
        return true;
    }
    if (head == "ellipsoid") {
        out = f.ellipsoid({arg(0, 0), arg(1, 0), arg(2, 0)},
                          {keys.number("rx", arg(3, 1)), keys.number("ry", arg(4, 1)),
                           keys.number("rz", arg(5, 1))});
        return true;
    }
    if (head == "prism") {
        out = f.prism({arg(0, 0), arg(1, 0), arg(2, 0)}, keys.number("r", 1.0),
                      keys.number("h", 1.0) * 0.5,
                      static_cast<u32>(keys.number("sides", 6.0)),
                      axis_from(keys.word("axis", "y")) % 3u, keys.number("turn", 0.0));
        return true;
    }
    if (head == "tetra" || head == "cube" || head == "octa" || head == "dodeca" ||
        head == "icosa") {
        const u32 which = (head == "tetra")    ? 0u
                          : (head == "cube")   ? 1u
                          : (head == "octa")   ? 2u
                          : (head == "dodeca") ? 3u
                                               : 4u;
        out = f.platonic({arg(0, 0), arg(1, 0), arg(2, 0)}, keys.number("r", arg(3, 1.0)), which);
        return true;
    }
    if (head == "wedge") {
        const Vec3 lo{arg(0, 0), arg(1, 0), arg(2, 0)};
        const Vec3 hi{arg(3, 1), arg(4, 1), arg(5, 1)};
        out = f.wedge({(lo.x + hi.x) * 0.5, (lo.y + hi.y) * 0.5, (lo.z + hi.z) * 0.5},
                      {std::abs(hi.x - lo.x) * 0.5, std::abs(hi.y - lo.y) * 0.5,
                       std::abs(hi.z - lo.z) * 0.5},
                      axis_from(keys.word("rise", "y")) % 3u,
                      axis_from(keys.word("run", "z")) % 3u);
        return true;
    }
    if (head == "stairs") {
        const Vec3 lo{arg(0, 0), arg(1, 0), arg(2, 0)};
        const Vec3 hi{arg(3, 1), arg(4, 1), arg(5, 1)};
        out = f.stairs({(lo.x + hi.x) * 0.5, (lo.y + hi.y) * 0.5, (lo.z + hi.z) * 0.5},
                       {std::abs(hi.x - lo.x) * 0.5, std::abs(hi.y - lo.y) * 0.5,
                        std::abs(hi.z - lo.z) * 0.5},
                       keys.number("run", 0.30), keys.number("rise", 0.18));
        return true;
    }

    if (head == "spiral") {
        out = f.spiral({arg(0, 0), arg(1, 0), arg(2, 0)}, keys.number("r", 1.0),
                       keys.number("tighten", 0.7), keys.number("tube", 0.08),
                       keys.number("turns", 2.5), axis_from(keys.word("axis", "z")) % 3u);
        return true;
    }

    // --- the mouldings, as sections ------------------------------------------------------------
    //
    // Two opposite corners like a box, first in the stone and second in the air, and a `run=` that
    // says which way the moulding travels. With four numbers the run is the default axis from one
    // metre back to one metre forward, which is more than any profile needs and is thrown away by
    // the `revolve` that usually follows; with six it is a length of straight cornice.
    if (is_moulding(head)) {
        const u32 run = axis_from(keys.word("run", "z")) % 3u;
        // Across the face, up it, and along it. Height is y unless the moulding runs up y, in
        // which case there is no y left and the section lies flat.
        const u32 proj = (run == 0) ? 2u : 0u;
        const u32 high = (run == 1) ? 2u : 1u;
        f64 p0 = 0, q0 = 0, p1 = 0, q1 = 0, r0 = -1.0, r1 = 1.0;
        if (args.size() >= 6) {
            const f64 low[3] = {arg(0, 0), arg(1, 0), arg(2, 0)};
            const f64 high_[3] = {arg(3, 0), arg(4, 0), arg(5, 0)};
            p0 = low[proj];  q0 = low[high];  r0 = low[run];
            p1 = high_[proj]; q1 = high_[high]; r1 = high_[run];
        } else if (args.size() >= 4) {
            p0 = arg(0, 0); q0 = arg(1, 0); p1 = arg(2, 0); q1 = arg(3, 0);
        } else {
            fail(head + " needs two corners: back-and-bottom first, front-and-top second");
            return false;
        }
        out = build_moulding(f, head, p0, q0, p1, q1, r0, r1, proj, high, run);
        return true;
    }

    // --- everything that forks -----------------------------------------------------------------
    //
    //   let bough = branch 0 12.75 0 h=0.62 r=0.05 levels=5 count=3 spread=0.11 lean=0.30 seed=7
    //
    // A trunk from the point given, growing along `axis`, forking `count` ways `levels` times. See
    // the block above BranchPlan for what it is for and what it costs — briefly, it costs NODES
    // and not time, and the parser refuses a plan that would cost too many of them.
    if (head == "branch") {
        BranchPlan plan;
        plan.base = {arg(0, 0), arg(1, 0), arg(2, 0)};
        plan.axis = axis_from(keys.word("axis", "y")) % 3u;
        plan.length = keys.number("h", 2.0);
        plan.radius = keys.number("r", 0.08);
        plan.levels = static_cast<u32>(std::clamp(keys.number("levels", 4.0), 1.0, 9.0));
        plan.count = static_cast<u32>(std::clamp(keys.number("count", 2.0), 1.0, 6.0));
        plan.spread = keys.number("spread", 0.10);
        plan.lean = std::clamp(keys.number("lean", 0.25), 0.0, 1.0);
        plan.shrink = keys.number("shrink", 0.72);
        plan.taper = keys.number("taper", 0.62);
        plan.segments = static_cast<u32>(std::clamp(keys.number("segments", 2.0), 1.0, 8.0));
        plan.seed = static_cast<u32>(keys.number("seed", 1.0));

        const usize wanted = branch_capsules(plan);
        if (wanted > kMostCapsules) {
            fail("branch levels=" + std::to_string(plan.levels) + " count=" +
                 std::to_string(plan.count) + " segments=" + std::to_string(plan.segments) +
                 " is " + std::to_string(wanted) + " capsules and the limit is " +
                 std::to_string(kMostCapsules) + " -- drop a level or a fork");
            return false;
        }
        Vec3 grow{0, 0, 0};
        if (plan.axis == 0) grow.x = 1.0;
        else if (plan.axis == 1) grow.y = 1.0;
        else grow.z = 1.0;

        std::vector<u32> limbs;
        limbs.reserve(wanted);
        grow_limb(f, plan, plan.base, grow, plan.length, plan.radius, 0u,
                  plan.seed * 2654435761u + 1u, limbs);
        out = unite_tree(f, limbs);
        return true;
    }

    // --- combining ---------------------------------------------------------------------------
    if (head == "union" || head == "difference" || head == "intersection" || head == "add" ||
        head == "multiply" || head == "min" || head == "max") {
        const std::vector<u32> parts = block();
        if (parts.empty()) {
            fail(head + " needs a { } with at least one thing in it");
            return false;
        }
        // Keys may follow the block as well as precede it, exactly as they may for the one-child
        // operations below — which is what `clips/facility/requests/halls.md` asked for and what
        // `clips/facility/BRIEF.md` has always documented:
        //
        //   let name = union { a b c }     # smooth=0.1 rounds the joins
        //
        // Written that way it did not parse. `union { a b } smooth=0.02` swallowed the block, left
        // `smooth` unread, and the next pass took it for a statement — so the error arrived under
        // the word `smooth`, on a later line, saying "unknown statement". Two clips carry a comment
        // explaining the workaround and one of them explains it twice.
        //
        // The one form this could take a key AWAY from is a braceless one-child op wrapping a
        // combining call — `shell union { a b } thickness=0.1`, where the `thickness` now belongs
        // to the union rather than to the shell. Nothing in `clips/` is written that way and the
        // control arm says so; the cure if it ever is, is the brace the rest of the file uses.
        keys_into(keys);
        const f64 smooth = keys.number("smooth", 0.0);
        // `chamfer=` is the flat seam to `smooth=`'s round one, and it is what a mason cuts: the
        // arris of a plinth, the stop of a jamb, the corner of a rusticated block. Both given, the
        // blend wins, because two treatments of one seam is a mistake and the older word is the
        // one already in the clips.
        const f64 chamfer = keys.number("chamfer", 0.0);
        if (head == "union")
            out = (smooth > 0.0)    ? f.smooth_unite(parts, smooth)
                  : (chamfer > 0.0) ? f.chamfer_unite(parts, chamfer)
                                    : f.unite(parts);
        else if (head == "difference")
            out = (smooth > 0.0)    ? f.smooth_subtract(parts, smooth)
                  : (chamfer > 0.0) ? f.chamfer_subtract(parts, chamfer)
                                    : f.subtract(parts);
        else if (head == "intersection")
            out = (smooth > 0.0)    ? f.smooth_intersect(parts, smooth)
                  : (chamfer > 0.0) ? f.chamfer_intersect(parts, chamfer)
                                    : f.intersect(parts);
        else if (head == "add") out = f.add(parts);
        else if (head == "multiply") out = f.multiply(parts);
        else if (head == "min") out = f.minimum(parts);
        else out = f.maximum(parts);
        return true;
    }

    // --- one-child operations ------------------------------------------------------------------
    if (head == "translate" || head == "rotate" || head == "scale" || head == "mirror" ||
        head == "repeat" || head == "scatter" || head == "around" || head == "shell" ||
        head == "round" ||
        head == "revolve" || head == "offset" || head == "twist" || head == "bend" || head == "abs" ||
        head == "negate" || head == "step" || head == "smoothstep" || head == "clamp" ||
        head == "remap" || head == "power" || head == "displace" || head == "blend" ||
        head == "occlusion" || head == "curvature" || head == "facing") {
        std::vector<u32> parts = block();
        if (parts.empty()) {
            // Also allow `shell walls 0.1` without braces, which reads better for one child.
            u32 child = 0;
            if (expression(child)) parts.push_back(child);
        }
        if (parts.empty()) {
            fail(head + " needs something to act on");
            return false;
        }
        // Keys may follow the block as well as precede it.
        keys_into(keys);
        std::vector<f64> more;
        while (value(v)) more.push_back(v);
        for (f64 extra : more) args.push_back(extra);

        const u32 child = parts[0];
        if (head == "translate")
            out = f.translate(child, {arg(0, 0), arg(1, 0), arg(2, 0)});
        else if (head == "rotate")
            out = f.rotate(child, {keys.number("x", arg(0, 0)), keys.number("y", arg(1, 0)),
                                   keys.number("z", arg(2, 0))});
        else if (head == "scale")
            out = f.scale(child, {keys.number("x", arg(0, 1)), keys.number("y", arg(1, 1)),
                                  keys.number("z", arg(2, 1))});
        else if (head == "mirror")
            out = f.mirror(child, axis_from(keys.word("axis", "x")) % 3u);
        else if (head == "repeat")
            out = f.repeat(child,
                           {keys.number("x", 0), keys.number("y", 0), keys.number("z", 0)},
                           {keys.number("nx", 0), keys.number("ny", 0), keys.number("nz", 0)});
        else if (head == "scatter") {
            // `repeat`'s keys, plus the two that stop it looking like a repeat:
            //
            //   let bed = scatter { pebble } x=0.07 z=0.07 nx=24 nz=24 jitter=0.45 turn=0.5
            //
            // `jitter` is a fraction of the cell and moves AND resizes each copy; `turn` is the
            // largest spin either way, in turns, so 0.5 is a free one. Both nought is a `repeat`
            // and becomes one. See Op::Scatter for the cost, which is `repeat`'s.
            out = f.scatter(child,
                            {keys.number("x", 0), keys.number("y", 0), keys.number("z", 0)},
                            {keys.number("nx", 0), keys.number("ny", 0), keys.number("nz", 0)},
                            keys.number("jitter", arg(0, 0.35)), keys.number("turn", 0.0));
        }
        else if (head == "around") {
            // Over an arc rather than the whole circle, `count` copies span it INCLUSIVELY: the
            // first sits on `from` and the last on `to`. Over a whole turn the old spacing stands
            // — n copies in n sectors — because a copy on each end would put two in one place.
            const f64 from = keys.number("from", 0.0);
            out = f.polar_repeat(child, static_cast<u32>(keys.number("count", arg(0, 4.0))),
                                 axis_from(keys.word("axis", "y")) % 3u, from,
                                 keys.number("to", from + 1.0));
        }
        else if (head == "shell")
            out = f.shell(child, keys.number("thickness", arg(0, 0.1)));
        else if (head == "round")
            out = f.round_off(child, keys.number("by", arg(0, 0.05)));
        else if (head == "revolve") {
            // The three numbers, when they are given, are where the axis stands — so a base is
            // drawn once from its own axis outward and then placed under whichever column it
            // belongs to, without a translate round every one.
            //
            // `from` and `to` in turns cut it to an apse, a niche head or a half dome. Left out
            // they are a whole revolution and nothing about the node changes.
            const f64 from = keys.number("from", 0.0);
            out = f.revolve(child, {arg(0, 0), arg(1, 0), arg(2, 0)},
                            axis_from(keys.word("axis", "y")) % 3u, from,
                            keys.number("to", from + 1.0));
        }
        else if (head == "offset")
            out = f.offset(child, keys.number("by", arg(0, 0.0)));
        else if (head == "twist")
            out = f.twist(child, keys.number("turns", arg(0, 0.25)),
                          axis_from(keys.word("axis", "y")) % 3u);
        else if (head == "bend")
            out = f.bend(child, keys.number("turns", arg(0, 0.25)),
                         axis_from(keys.word("axis", "y")) % 3u);
        // --- what the shape is DOING here, which the language could not ask until now ---------
        //
        // `Field` has been able to answer these three since weathering was written, and a clip has
        // never been able to ask. So `weather sea 0.5` could put salt in the hollows and an author
        // could not put moss in them, which is backwards: the five weathers are five opinions and
        // these are the facts they are built out of.
        //
        //   let cavity = occlusion { part_portico } r=0.22    0 in the open, 1 buried
        //   let arris  = curvature { part_portico } r=0.10    + on an edge, - in a corner
        //   let up     = facing    { part_portico } axis=y    + up, - down
        //
        //   paint soot  where=cavity above=0.55
        //   paint worn  where=arris  above=0.30
        //
        // They are the whole of what a surface scanned off a real building has that a modelled one
        // does not: dirt where the rain never reaches, wear where a hand or a shoulder has passed,
        // a wash of pale stone under every sill. And they are EXPENSIVE — an occlusion evaluates
        // its child fourteen times, a curvature seven and a facing six — so name a small shape,
        // not the building, and pair them with `on=` so the rule is only asked where it can fire.
        else if (head == "occlusion") out = f.occlusion(child, keys.number("r", arg(0, 0.15)));
        else if (head == "curvature") out = f.curvature(child, keys.number("r", arg(0, 0.05)));
        else if (head == "facing")
            out = f.facing(child, axis_from(keys.word("axis", "y")) % 3u);
        else if (head == "abs") out = f.absolute(child);
        else if (head == "negate") out = f.negate(child);
        else if (head == "step") out = f.step(child, keys.number("at", arg(0, 0.0)));
        else if (head == "smoothstep")
            out = f.smoothstep(child, keys.number("from", arg(0, 0.0)),
                               keys.number("to", arg(1, 1.0)));
        else if (head == "clamp")
            out = f.clamp_to(child, keys.number("low", arg(0, 0.0)),
                             keys.number("high", arg(1, 1.0)));
        else if (head == "remap")
            out = f.remap(child, keys.number("from", arg(0, -1.0)),
                          keys.number("to", arg(1, 1.0)), keys.number("low", arg(2, 0.0)),
                          keys.number("high", arg(3, 1.0)));
        else if (head == "power") out = f.power(child, keys.number("by", arg(0, 2.0)));
        else if (head == "blend") {
            if (parts.size() < 2) {
                fail("blend needs two things");
                return false;
            }
            out = f.blend(parts[0], parts[1], keys.number("t", arg(0, 0.5)));
        } else {   // displace
            if (parts.size() < 2) {
                fail("displace needs a shape and a pattern");
                return false;
            }
            out = f.displace(parts[0], parts[1],
                             usable_displacement(keys.number("amount", arg(0, 0.05)),
                                                 script_.settings.voxels_per_metre, "displace"));
        }
        return true;
    }

    // --- patterns ---------------------------------------------------------------------------
    if (head == "constant") { out = f.constant(arg(0, 0.0)); return true; }
    if (head == "axis") { out = f.coordinate(axis_from(keys.word("of", "y")) % 3u); return true; }
    if (head == "distance") { out = f.radius({arg(0, 0), arg(1, 0), arg(2, 0)}); return true; }
    if (head == "sine") {
        out = f.sine(axis_from(keys.word("axis", "x")) % 3u, keys.number("period", arg(0, 1.0)),
                     keys.number("phase", 0.0));
        return true;
    }
    if (head == "waves") {
        out = f.waves(axis_from(keys.word("axis", "y")) % 3u, keys.number("a", 1.0),
                      keys.number("b", 1.0), keys.number("phase", 0.0));
        return true;
    }
    // Every grain below takes `stretch=`, which multiplies its feature size along each axis:
    //
    //   let bark  = fbm size=0.06 octaves=4 stretch=1,9,1      runs UP a trunk
    //   let streak= ridged size=0.35 octaves=3 stretch=3,1,3   ...and rain runs DOWN a wall
    //   let riven = cells size=0.09 stretch=4,1,4              slate splits in one plane
    //
    // One number means the same on all three axes. See the block above Op::Sine in field.hpp for
    // why this cannot be had from `scale`, which would divide the pattern's amplitude with it.
    const auto stretch_of = [&]() {
        return Vec3{keys.number_at("stretch", 0, 1.0), keys.number_at("stretch", 1, 1.0),
                    keys.number_at("stretch", 2, 1.0)};
    };
    if (head == "noise") {
        out = f.noise(keys.number("size", arg(0, 1.0)),
                      static_cast<u32>(keys.number("seed", 1.0)), stretch_of());
        return true;
    }
    if (head == "fbm" || head == "ridged") {
        const f64 size = keys.number("size", arg(0, 1.0));
        const u32 octaves = static_cast<u32>(keys.number("octaves", 4.0));
        const f64 gain = keys.number("gain", 0.5);
        const f64 lacunarity = keys.number("lacunarity", 2.0);
        const u32 seed = static_cast<u32>(keys.number("seed", 1.0));
        out = (head == "fbm") ? f.fbm(size, octaves, gain, lacunarity, seed, stretch_of())
                              : f.ridged(size, octaves, gain, lacunarity, seed, stretch_of());
        return true;
    }
    if (head == "rasp") {
        out = f.rasp(keys.number("size", arg(0, 0.05)), keys.number("depth", 1.0),
                     static_cast<u32>(keys.number("seed", 1.0)), stretch_of());
        return true;
    }
    if (head == "cells") {
        out = f.cells(keys.number("size", arg(0, 0.5)),
                      static_cast<u32>(keys.number("seed", 1.0)), stretch_of());
        return true;
    }
    // The seams BETWEEN the cells rather than the cells, which is what a crack is: a branching
    // network that meets itself at junctions and never simply stops. `Field` has had it since the
    // weathering was written and a clip has never been able to say it — so `weather cracks` could
    // craze a wall and an author could not craze a glaze, a plaster, a dry riverbed or a pane.
    //
    //   let craze = cell_edge size=0.12 seed=4
    //   let glaze = displace { pot craze } amount=-0.004
    if (head == "cell_edge" || head == "cracks") {
        out = f.cell_edge(keys.number("size", arg(0, 0.5)),
                          static_cast<u32>(keys.number("seed", 1.0)), stretch_of());
        return true;
    }
    if (head == "checker") {
        const f64 cell = keys.number("size", arg(0, 1.0));
        out = f.checker({keys.number("x", cell), keys.number("y", cell), keys.number("z", cell)});
        return true;
    }
    if (head == "stripes") {
        out = f.stripes(axis_from(keys.word("axis", "y")) % 3u, keys.number("period", arg(0, 1.0)),
                        keys.number("duty", 0.5));
        return true;
    }
    if (head == "bricks") {
        out = f.bricks({keys.number("length", 0.24), keys.number("height", 0.08), 0.0},
                       keys.number("mortar", 0.012),
                       axis_from(keys.word("facing", "z")) % 3u);
        return true;
    }

    fail("unknown shape or pattern '" + head + "'");
    return false;
}

void Parser::statement() {
    if (done()) return;
    const std::string head = take();
    Field& f = script_.field;

    if (head == "metre" || head == "meter") {
        script_.settings.voxels_per_metre = static_cast<i32>(value_or(kVoxelsPerMetre));
        return;
    }
    if (head == "bounds") {
        script_.settings.low = {value_or(0), value_or(0), value_or(0)};
        script_.settings.high = {value_or(1), value_or(1), value_or(1)};
        return;
    }
    if (head == "param") {
        const std::string name = take();
        const f64 initial = value_or(0.0);
        f.parameter(name.c_str(), initial);
        parameters_[name] = true;
        return;
    }
    if (head == "material") {
        const std::string name = take();
        Keys keys;
        keys_into(keys);
        VisualRecord visual;
        auto rgb = keys.numbers.find("rgb");
        if (rgb != keys.numbers.end() && rgb->second.size() >= 3) {
            visual.red = static_cast<u8>(rgb->second[0]);
            visual.green = static_cast<u8>(rgb->second[1]);
            visual.blue = static_cast<u8>(rgb->second[2]);
        }
        visual.roughness = static_cast<u8>(keys.number("rough", 200.0));
        visual.metallic = static_cast<u8>(keys.number("metal", 0.0));
        visual.opacity = static_cast<u8>(keys.number("opacity", 255.0));
        visual.emissive = static_cast<u8>(keys.number("emit", 0.0));
        visual.translucency = static_cast<u8>(keys.number("translucent", 0.0));
        // Beer-Lambert, per metre, one byte a channel. Declared in clips/glass_test.clip since
        // the day it was written and read by nothing until now, so every coloured pane in the
        // repository has been rendering as a clear one.
        auto absorb = keys.numbers.find("absorb");
        if (absorb != keys.numbers.end() && absorb->second.size() >= 3) {
            visual.absorb_red = static_cast<u8>(absorb->second[0]);
            visual.absorb_green = static_cast<u8>(absorb->second[1]);
            visual.absorb_blue = static_cast<u8>(absorb->second[2]);
        }
        visual.flags = static_cast<u8>(keys.number("flags", 0.0));
        // A brushed metal names a world axis, because a world axis is the only direction a voxel
        // face can be given that is still the same direction on the next face round a corner.
        // 1 = x, 2 = y, 3 = z; see VisualFlags.
        visual.flags |= static_cast<u8>(static_cast<u32>(keys.number("brush", 0.0)) & 3u) << 3;
        // Two lobes in one byte, four bits each: enough, because both are a strength and the
        // shape of each is fixed.
        visual.coat = static_cast<u8>(static_cast<u32>(keys.number("lacquer", 0.0)) & 15u) |
                      (static_cast<u8>(static_cast<u32>(keys.number("sheen", 0.0)) & 15u) << 4);
        if (keys.has("ior")) {
            // Written as a refractive index, stored as the offset from vacuum the record uses.
            const f64 index = keys.number("ior", 1.0);
            visual.ior = static_cast<u8>(std::min(255.0, std::max(0.0, (index - 1.0) * 128.0)));
        }
        BehaviourRecord behaviour;
        behaviour.material = static_cast<u32>(script_.material_names.size() + 1);
        (void)tags_;
        const VoxelTypeId type = types_.intern(visual, behaviour);
        // Whether this NAME is new, decided before the map is written, because that is the only
        // thing that says whether the tool's palette gains an entry or replaces one.
        //
        // It cannot be decided from the type id: `behaviour.material` is the count of names seen so
        // far, so re-declaring `granite` mints a DIFFERENT id for an identical-looking material and
        // a de-duplication by id finds nothing to remove.
        const auto known = materials_.find(name);
        const bool first_time = known == materials_.end();
        const VoxelTypeId replaced = first_time ? VoxelTypeId{0} : known->second;
        materials_[name] = type;
        // The name table is indexed by type id, so a report can look one up directly.
        if (script_.material_names.size() <= type) {
            script_.material_names.resize(static_cast<usize>(type) + 1);
        }
        script_.material_names[type] = name;
        // The tool's palette, and it must not gain an entry for a material it already has.
        //
        // A fragment declares what it needs and includes `_contract.clip` to get it, and
        // twenty-two of them do — so twenty-two of these ran for every material on the facility and
        // the palette came out **550 long for 25 materials**. `types_.intern` returns the same id
        // for an identical record, so the duplicates were not new materials in any sense that
        // matters; they were the same twenty-five listed twenty-two times.
        //
        // What that does to Q and E is not that they stop working — each press does step to a
        // different material — it is that the list wraps after 25 of 550 and the count printed
        // beside it is a fiction. The report this was found under was *"changing material with q
        // and e no longer works"*, and a palette whose reported size is twenty-two times its real
        // one is the first thing to eliminate before believing anything else about that key.
        //
        // A name declared again REPLACES its entry rather than adding one, and it has to be a
        // replacement rather than a skip: a fragment is allowed to override a material the contract
        // declared, and the palette should then hold what the fragment said. Position is kept, so
        // the order a player steps through with Q and E is the order the clip declares them in
        // however many times the contract is included.
        //
        // Linear, because a palette is tens of entries and a second container would be one more
        // thing to keep in step with the vector it describes.
        if (first_time) {
            script_.material_types.push_back(type);
        } else {
            const auto at =
                std::find(script_.material_types.begin(), script_.material_types.end(), replaced);
            if (at != script_.material_types.end()) {
                *at = type;
            } else {
                script_.material_types.push_back(type);
            }
        }
        return;
    }
    if (head == "let") {
        const std::string name = take();
        if (!done() && peek().text == "=") ++at_;
        u32 node = 0;
        if (!expression(node)) {
            fail("could not read the right hand side of 'let " + name + "'");
            skip_statement();
            return;
        }
        bindings_[name] = node;
        // Re-binding a name replaces what it means, so the record is replaced rather than added
        // to: a part is whatever its name finally referred to.
        bool replaced = false;
        for (auto& part : script_.parts) {
            if (part.first == name) {
                part.second = node;
                replaced = true;
                break;
            }
        }
        if (!replaced) script_.parts.emplace_back(name, node);
        return;
    }
    if (head == "paint") {
        const std::string material = take();
        auto it = materials_.find(material);
        if (it == materials_.end()) {
            fail("paint refers to a material '" + material + "' that has not been declared");
            skip_statement();
            return;
        }
        Keys keys;
        keys_into(keys);
        PaintRule rule;
        rule.type = it->second;
        // With no test at all the rule covers everything, which is what the first coat is.
        rule.test = f.constant(0.0);
        const std::string where = keys.word("where", "");
        if (!where.empty()) {
            auto bound = bindings_.find(where);
            if (bound == bindings_.end()) {
                fail("paint where=" + where + " does not name anything");
                return;
            }
            rule.test = bound->second;
        }
        // `on=<shape>` says WHERE, as distinct from `where=`, which says what.
        //
        // For a rule keyed on a shape the two are the same thing and this is unnecessary. For one
        // keyed on a pattern it is the difference between a rule that can be settled for a region
        // and one that has to be asked at every voxel in the clip — because a pattern is true or
        // false at a point and says nothing about its neighbourhood, while a shape has a box.
        //
        //   paint moss where=grain above=0.55 on=north_wall
        //
        // reads as "moss where the grain is high, on the north wall", and costs the sampler
        // nothing anywhere else in the building.
        const std::string on = keys.word("on", "");
        if (!on.empty()) {
            auto placed = bindings_.find(on);
            if (placed == bindings_.end()) {
                fail("paint on=" + on + " does not name anything");
                return;
            }
            rule.place = placed->second;
            rule.has_place = true;
        }
        if (keys.has("above")) rule.low = keys.number("above", -1e30);
        if (keys.has("below")) rule.high = keys.number("below", 1e30);
        if (keys.has("facing")) {
            rule.facing_axis = axis_from(keys.word("facing", "y")) % 3u;
            rule.facing_min = keys.number("at", 0.5);
        }
        script_.paint.push_back(rule);
        script_.paint_source.push_back(where.empty() ? std::string("<no where>")
                                                     : "where=" + where);
        return;
    }
    if (head == "weather") {
        const std::string kind = take();
        Keys keys;
        f64 amount = 0.0;
        value(amount);
        keys_into(keys);
        WeatherRequest request;
        request.amount = keys.number("amount", amount);
        request.scale = keys.number("scale", 1.0);
        request.seed = static_cast<u32>(keys.number("seed", 1.0));
        request.level = keys.number("level", 0.0);
        const std::string on = keys.word("on", "");
        if (!on.empty()) {
            auto bound = bindings_.find(on);
            if (bound == bindings_.end()) {
                fail("weather on=" + on + " does not name anything");
                return;
            }
            request.scope = bound->second;
            request.has_scope = true;
        }
        if (kind == "desert") request.kind = Weather::Desert;
        else if (kind == "overgrown") request.kind = Weather::Overgrown;
        else if (kind == "cracks") request.kind = Weather::Cracks;
        else if (kind == "burnt") request.kind = Weather::Burnt;
        else if (kind == "sea") request.kind = Weather::Sea;
        else {
            fail("unknown weathering '" + kind +
                 "' — desert, overgrown, cracks, burnt or sea");
            return;
        }
        script_.weather.push_back(request);
        return;
    }
    if (head == "variation") {
        Keys keys;
        keys_into(keys);
        script_.variation.colour = keys.number("colour", keys.number("color", 0.03));
        script_.variation.roughness = keys.number("rough", 0.05);
        script_.variation.seed = static_cast<u32>(keys.number("seed", 1.0));
        script_.variation.budget = static_cast<u32>(keys.number("budget", 1000000.0));
        const std::string by = keys.word("by", "");
        if (!by.empty()) {
            auto bound = bindings_.find(by);
            if (bound == bindings_.end()) {
                fail("variation by=" + by + " does not name anything");
                return;
            }
            script_.variation.by = bound->second;
            script_.variation.has_by = true;
        }
        return;
    }
    // Move the whole clip: the shape AND every rule that decides its colour.
    //
    // This exists because doing it by hand is a trap that has already been fallen into. A clip is not
    // one field, it is one solid plus a paint rule per material, and each rule is its own expression
    // evaluated at the same world position. Wrapping only the solid in a translate — the obvious
    // thing, and what `let all = translate { ... }` does — moves the stone and leaves the paint
    // exactly where it was, so a building drops three and a half metres and its plinth course,
    // weathering bands and ground line all stay at the height they were drawn for. The geometry is
    // right, the colours are wrong, and it looks like a rendering fault rather than an edit.
    //
    // Written as one statement, that class of mistake cannot be made: there is no way to name the
    // solid without the paint travelling with it.
    if (head == "origin") {
        const f64 dx = value_or(0.0);
        const f64 dy = value_or(0.0);
        const f64 dz = value_or(0.0);
        script_.origin_shift[0] += dx;
        script_.origin_shift[1] += dy;
        script_.origin_shift[2] += dz;
        return;
    }

    if (head == "solid") {
        u32 node = 0;
        if (!expression(node)) {
            fail("solid needs the name of a shape");
            skip_statement();
            return;
        }
        script_.solid = node;
        script_.has_solid = true;
        return;
    }
    if (head == "region") {
        u32 node = 0;
        if (!expression(node)) {
            fail("region needs the name of a shape");
            skip_statement();
            return;
        }
        script_.settings.bounds = node;
        script_.settings.has_bounds = true;
        return;
    }

    fail("unknown statement '" + head + "'");
    skip_statement();
}

// --- weathering ---------------------------------------------------------------------------
//
// Each kind expands into two things: a deformation of the solid, and coats of paint that follow
// the same geometry the deformation followed. They are built here rather than written in the
// clip file because the expansion is long, fiddly and identical every time — an author wants to
// say "half weathered by the sea", not to re-derive what that means from curvature and cavity.

VoxelTypeId make_material(VoxelTypeTable& types, Script& script, const char* name, u8 r, u8 g,
                          u8 b, u8 rough) {
    // Reuse the author's material of the same name when there is one, so a clip that declares
    // its own `moss` keeps it and weathering paints with that rather than inventing a second.
    for (usize i = 0; i < script.material_names.size(); ++i) {
        if (script.material_names[i] == name) return static_cast<VoxelTypeId>(i);
    }
    VisualRecord visual;
    visual.red = r;
    visual.green = g;
    visual.blue = b;
    visual.roughness = rough;
    BehaviourRecord behaviour;
    behaviour.material = static_cast<u32>(script.material_names.size() + 64);
    const VoxelTypeId type = types.intern(visual, behaviour);
    if (script.material_names.size() <= type) {
        script.material_names.resize(static_cast<usize>(type) + 1);
    }
    script.material_names[type] = name;
    return type;
}

// Shift the finished clip so a chosen point in it lands on the world origin.
//
// After weathering rather than before, and after every paint rule has been collected, because the
// whole point is that NOTHING is left behind: whatever the script ended up with — the solid, the
// rules the author wrote, the rules the weathering added — is moved by the same vector, once.
void apply_origin(Script& script) {
    const f64 dx = script.origin_shift[0];
    const f64 dy = script.origin_shift[1];
    const f64 dz = script.origin_shift[2];
    if (dx == 0.0 && dy == 0.0 && dz == 0.0) return;

    Field& f = script.field;
    const Vec3 by{dx, dy, dz};

    if (script.has_solid) script.solid = f.translate(script.solid, by);
    for (PaintRule& rule : script.paint) {
        rule.test = f.translate(rule.test, by);
    }

    // AND THE NAMES THE FILE BOUND, which is the part this was quietly not doing while the comment
    // above claimed nothing was left behind.
    //
    // `script.parts` is how a tool asks for one piece of a clip on its own -- `--part part_dome`,
    // and the clip viewer bakes every fragment of the facility through it. Those entries pointed at
    // the nodes as the author wrote them while the solid, the rules and the BOUNDS all moved 3.50 m,
    // so asking for a part sampled an unmoved shape inside a box that had dropped. It does not
    // fail, it answers: `part_dome` came back an 11.75 x 1.00 x 11.75 m saucer wearing one material
    // instead of six -- the 1.05 m of a 4.15 m dome that happened to still fall inside the moved
    // box, painted by whatever rule was 3.50 m lower. `part_pilasters` reported eleven materials on
    // a part that paints two, the other nine being the site's and the podium's coats arriving from
    // above.
    //
    // Two separate agents measuring two different parts each concluded their fragment was broken
    // before either found the instrument was. The shipped building was never affected -- it is
    // built from `solid`, which moved correctly -- so nothing but the measurements was ever wrong,
    // and that is exactly what makes it worth fixing: every number anybody took through `--part`
    // was taken against the wrong box.
    for (auto& entry : script.parts) {
        entry.second = f.translate(entry.second, by);
    }

    // The bounds are in the same space and have to come along, or the clip is cut where it used to
    // be rather than where it now is.
    script.settings.low.x += dx;
    script.settings.high.x += dx;
    script.settings.low.y += dy;
    script.settings.high.y += dy;
    script.settings.low.z += dz;
    script.settings.high.z += dz;
}

void apply_weather(Script& script, VoxelTypeTable& types) {
    if (script.weather.empty() || !script.has_solid) return;
    Field& f = script.field;

    // Weather appends its coats after every fragment's paint, so the names have to be kept in step
    // here too or the diagnostic starts naming the wrong rule at exactly the point it matters.
    const auto name_new_coats = [&script](usize from, const std::string& what) {
        script.paint_source.resize(from);
        while (script.paint_source.size() < script.paint.size()) {
            script.paint_source.push_back(what);
        }
    };

    for (const WeatherRequest& request : script.weather) {
        // Where this request's coats begin, so the scope can be stamped onto all of them at the
        // end without every one of the dozen push_backs below having to remember to.
        const usize first_coat = script.paint.size();

        const f64 a = std::clamp(request.amount, 0.0, 1.0);
        if (a <= 0.0) continue;
        const f64 s = (request.scale > 0.0) ? request.scale : 1.0;
        const u32 seed = request.seed;

        // --- the scope ------------------------------------------------------------------------
        //
        // Two masks, because the deformation and the paint need opposite senses of the same
        // question, and both are answered from the named shape's own distance.
        //
        // `inside` is one on and within the shape's surface and falls to zero a few centimetres
        // outside it. On, not half — the surface being weathered *is* the shape's surface, and a
        // mask that reached a half there would weather the podium's face at half strength and its
        // interior at full, which is exactly backwards.
        //
        // `outside` is the complement, and it is used to push a coat's test clear out of its own
        // range where the shape is not. Every coat below is a "this value or higher" rule, so
        // subtracting a number larger than anything in the clip disables it as surely as an
        // intersection would and needs no new machinery in the sampler to read.
        const f64 band = 0.06 * s;
        u32 inside = f.constant(1.0);
        u32 elsewhere = f.constant(0.0);
        if (request.has_scope) {
            inside = f.smoothstep(f.negate(request.scope), -band, 0.0);
            elsewhere = f.smoothstep(request.scope, 0.0, band);
        }
        const u32 banish = f.multiply({elsewhere, f.constant(-1e9)});
        const auto only_here = [&](u32 test) {
            return request.has_scope ? f.add({test, banish}) : test;
        };

        // Weathering the SHAPE, not just its value, and only where it was asked for.
        //
        // A weathering deformation is `displace(solid, mask, amount)` where the mask is already
        // zero outside the scope — so the answer was always right. What was wrong was the cost.
        // The mask is built from occlusion and curvature, and each of those samples the field
        // several times over; wrapped round the whole solid, EVERY evaluation anywhere in the
        // clip pays for the occlusion of the entire building, to be multiplied by zero.
        //
        // Measured on the facility, at six voxels to the metre: 2.4 seconds with the weathering
        // fragment missing, 623 seconds with it — for twenty-five per cent more evaluations. Each
        // one had become two hundred and sixty times dearer.
        //
        // So the solid is cut in two at the scope, the displacement is applied to the piece inside
        // it, and the pieces are put back together. The union then has a small box round the
        // expensive branch, and a point anywhere else in the building skips it on that box without
        // ever asking what the occlusion there is. The seam is safe because the cut is made wider
        // than the mask's own falloff plus the amplitude, so the displacement is already nought
        // where the two pieces meet.
        // The mask, with the cheap question that can rule everything out asked FIRST.
        //
        // Every weathering mask is a product, and one of its factors is the scope: nought outside
        // the shape the author named, and the whole point of naming it. The other factors are an
        // occlusion or a curvature, each of which samples the field several times over.
        //
        // Multiply now stops at the first factor that is nought (see Op::Multiply in field.cpp),
        // so putting the scope first means a point outside it costs one smoothstep of a box and
        // nothing else. Written the other way round — and it was, with `inside` trailing at the
        // end of the list — every evaluation anywhere in the clip computed the occlusion of the
        // whole building and then multiplied it by zero.
        //
        // Measured on the facility at six voxels to the metre: 2.4 seconds with the weathering
        // fragment missing entirely, 623 seconds with it, for twenty-five per cent more
        // evaluations. Each one had become two hundred and sixty times dearer, and this is why.
        const auto scoped_mask = [&](std::vector<u32> factors) {
            if (request.has_scope) factors.insert(factors.begin(), inside);
            return f.multiply(factors);
        };
        const auto weather_within = [&](u32 solid, u32 pattern, f64 amount) {
            return f.displace(solid, pattern, usable_displacement(amount, script.settings.voxels_per_metre, "weather"));
        };

        // The three questions every kind asks of the shape.
        const u32 shape = script.solid;
        const u32 cavity = f.occlusion(shape, 0.22 * s);          // 0 exposed, 1 buried
        const u32 edge = f.curvature(shape, 0.10 * s);            // + on an arris, - in a hollow
        const u32 up = f.facing(shape, 1);                        // + up, - down
        const u32 height = f.coordinate(1);

        switch (request.kind) {
            case Weather::Desert: {
                // Sand settles on anything facing up and lodges in every hollow; the wind takes
                // the arrises off. Both follow the shape, so a sill collects and its nose does
                // not — which is the whole reason for doing this from geometry.
                const u32 grit = f.fbm(0.09 * s, 3u, 0.55, 2.3, seed + 3u);
                const u32 scour =
                    scoped_mask({f.smoothstep(edge, 0.2, 1.2), f.constant(a)});
                script.solid = weather_within(script.solid, scour, 0.05 * s);

                const u32 sand = make_material(types, script, "sand", 198, 176, 132, 245);
                const u32 bleach = make_material(types, script, "bleached", 208, 202, 188, 235);

                PaintRule sun_bleach;
                sun_bleach.type = bleach;
                sun_bleach.test = only_here(f.add({f.multiply({up, f.constant(0.6)}),
                                                   f.multiply({grit, f.constant(0.4)})}));
                sun_bleach.low = 0.55 - 0.35 * a;
                script.paint.push_back(sun_bleach);

                PaintRule drift;
                drift.type = sand;
                // Up-facing, plus low down, plus in the hollows: three ways sand arrives, added
                // rather than chosen between, because a low up-facing hollow gets the most.
                drift.test = only_here(f.add({f.multiply({up, f.constant(0.5)}),
                                              f.multiply({cavity, f.constant(0.5)}),
                                              f.smoothstep(f.negate(height),
                                                           -request.level - 1.5 * s,
                                                           -request.level)}));
                drift.low = 1.15 - 0.75 * a;
                script.paint.push_back(drift);
                break;
            }
            case Weather::Overgrown: {
                // Growth wants damp and shelter, so it follows the cavity term and the up-facing
                // one, and it swells the surface slightly where it takes hold.
                const u32 clumps = f.fbm(0.35 * s, 4u, 0.5, 2.1, seed + 11u);
                const u32 where = f.add({f.multiply({cavity, f.constant(0.55)}),
                                         f.multiply({up, f.constant(0.35)}),
                                         f.multiply({clumps, f.constant(0.4)})});
                script.solid =
                    weather_within(script.solid,
                                   scoped_mask({f.smoothstep(where, 0.35, 0.95), f.constant(-a)}),
                                   0.045 * s);

                const u32 moss = make_material(types, script, "moss", 74, 108, 54, 250);
                const u32 lichen = make_material(types, script, "lichen", 138, 148, 108, 248);
                const u32 growing = only_here(where);

                PaintRule pale;
                pale.type = lichen;
                pale.test = growing;
                pale.low = 0.55 - 0.35 * a;
                script.paint.push_back(pale);

                PaintRule green;
                green.type = moss;
                green.test = growing;
                green.low = 0.85 - 0.55 * a;
                script.paint.push_back(green);
                break;
            }
            case Weather::Cracks: {
                // A crack is not a line drawn on a surface, it is the seam between two regions,
                // which is exactly what the distance between the two nearest scattered points
                // gives — and it branches and meets itself for free, because seams do.
                const u32 seams = f.cell_edge(0.55 * s, seed + 17u);
                const u32 wander = f.fbm(0.2 * s, 3u, 0.5, 2.0, seed + 29u);
                // Narrow where the amount is low, opening as it rises.
                const u32 opened =
                    f.smoothstep(f.add({seams, f.multiply({wander, f.constant(0.05 * s)})}),
                                 0.02 * s + 0.06 * s * a, 0.0);
                // Added to the distance, not subtracted from it. Displacement moves a surface by
                // adding to how far away it says it is, so a *positive* value on the seams eats
                // into the solid — which is what a crack is. Negated, the seams stood proud of
                // the face instead, and the block came out bigger than it started.
                script.solid = weather_within(script.solid, scoped_mask({opened}),
                                          0.09 * s * a);

                const u32 dark = make_material(types, script, "fissure", 66, 62, 58, 250);
                PaintRule fissures;
                fissures.type = dark;
                fissures.test = only_here(opened);
                fissures.low = 0.35;
                script.paint.push_back(fissures);
                break;
            }
            case Weather::Burnt: {
                // Heat rounds what it touches and soot collects where nothing washes it off:
                // undersides, and the backs of hollows. So the deformation is a rounding of the
                // arrises and the paint follows the down-facing and the cavity.
                const u32 soot_grain = f.fbm(0.14 * s, 4u, 0.6, 2.4, seed + 41u);
                // Written as a displacement by the scope mask rather than as a `round`, because a
                // round takes the same slice off everything in the clip and there is no version of
                // it that only softens one building's arrises. Unscoped the mask is the constant
                // one and this is exactly the round it replaces.
                script.solid = weather_within(script.solid, inside, -0.02 * s * a);

                const u32 char_ = make_material(types, script, "charred", 44, 40, 38, 252);
                const u32 soot = make_material(types, script, "soot", 28, 26, 25, 254);
                const u32 scorch = make_material(types, script, "scorched", 96, 78, 62, 246);

                PaintRule light;
                light.type = scorch;
                light.test = only_here(f.add({soot_grain, f.multiply({cavity, f.constant(0.5)})}));
                light.low = 0.5 - 0.45 * a;
                script.paint.push_back(light);

                PaintRule mid;
                mid.type = char_;
                mid.test = only_here(f.add({f.multiply({cavity, f.constant(0.7)}),
                                            f.multiply({soot_grain, f.constant(0.5)})}));
                mid.low = 0.75 - 0.55 * a;
                script.paint.push_back(mid);

                PaintRule under;
                under.type = soot;
                under.test =
                    only_here(f.add({f.negate(up), f.multiply({cavity, f.constant(0.8)})}));
                under.low = 1.05 - 0.75 * a;
                script.paint.push_back(under);
                break;
            }
            case Weather::Sea: {
                // A tide line divides the whole thing. Above it, salt and bleaching; below it,
                // barnacles crusting the exposed faces and weed in everything sheltered. The
                // line is `level`, and it is the one weathering that cares where the datum is.
                const u32 crust = f.fbm(0.05 * s, 3u, 0.6, 2.6, seed + 53u);
                const u32 lumps = f.cells(0.08 * s, seed + 59u);
                const u32 below = f.smoothstep(f.negate(height), -request.level,
                                               -request.level + 0.9 * s);

                // Barnacles are added matter, not removed: they stand proud of the surface.
                const u32 growth =
                    scoped_mask({below, f.smoothstep(lumps, 0.045 * s, 0.0), f.constant(-a)});
                script.solid = weather_within(script.solid, growth, 0.05 * s);

                const u32 salt = make_material(types, script, "salt", 206, 204, 196, 250);
                const u32 barnacle = make_material(types, script, "barnacle", 190, 186, 172, 244);
                const u32 weed = make_material(types, script, "weed", 58, 84, 62, 250);

                PaintRule dried;
                dried.type = salt;
                dried.test = only_here(f.add({f.multiply({f.negate(below), f.constant(0.7)}),
                                              f.multiply({crust, f.constant(0.5)})}));
                dried.low = 0.75 - 0.5 * a;
                script.paint.push_back(dried);

                PaintRule shells;
                shells.type = barnacle;
                shells.test = only_here(f.multiply({below, f.smoothstep(lumps, 0.05 * s, 0.0)}));
                shells.low = 0.55 - 0.4 * a;
                script.paint.push_back(shells);

                PaintRule green;
                green.type = weed;
                green.test = only_here(
                    f.multiply({below, f.add({cavity, f.multiply({crust, f.constant(0.4)})})}));
                green.low = 0.7 - 0.45 * a;
                script.paint.push_back(green);
                break;
            }
            default: break;
        }

        // Every coat this request just added is confined to the shape the request named, so say
        // so. The scoping above already makes each coat's TEST false outside that shape, which is
        // correct and is invisible to the sampler: a weathering test is a curvature or an
        // occlusion or a noise, none of which can be settled for a region, so without this the
        // coat is asked at every solid voxel in the clip to discover it is out of range at all
        // but a few of them.
        //
        // Measured on the facility: six such coats, confined by their authors to the steps, a
        // cornice wash and two strips of the north wall — and eight hundred and forty million of
        // the building's nine hundred and twelve million field evaluations.
        if (request.has_scope) {
            for (usize i = first_coat; i < script.paint.size(); ++i) {
                script.paint[i].place = request.scope;
                script.paint[i].has_place = true;
            }
        }
        // Named by their scope rather than by their kind, because the scope is the half an author
        // can do something about and the material name beside it already says which kind it is.
        name_new_coats(first_coat, request.has_scope ? "weather on=<scope>" : "weather everywhere");
    }
}

}  // namespace

Script parse_clip_script(const std::string& text, VoxelTypeTable& types, const TagRegistry& tags) {
    Script script;
    const std::vector<Token> tokens = tokenize(text);
    Parser parser(tokens, script, types, tags);
    parser.run();
    // Weathering is expanded after the whole file is read, because it acts on whatever ends up
    // being the solid and appends its coats after the author's.
    apply_weather(script, types);
    apply_origin(script);

    // The graph is complete now, so the boxes that let a union skip its distant children can be
    // worked out. Done here rather than in the sampler because a Field can be sampled many
    // times and its shape does not change between them.
    script.field.build_bounds();
    if (!script.has_solid && script.errors.empty()) {
        script.errors.push_back(ScriptError{0, "the file never says which shape is the solid"});
    }
    return script;
}

std::string expand_includes(const std::string& path, std::vector<SourceLine>& origin,
                            std::vector<ScriptError>& errors, const std::string& beside) {
    std::vector<std::string> open;   // the include stack, for cycle detection
    std::string out;

    // Recursive, and deliberately textual: an include splices one file's lines into another's
    // before a single token is read, so `include` costs the parser nothing and a fragment is
    // exactly the text it appears to be.
    const std::function<void(const std::string&, u32)> pull = [&](const std::string& file,
                                                                  u32 depth) {
        if (depth > 16) {
            errors.push_back(ScriptError{0, "includes nested more than sixteen deep in '" +
                                                file + "'"});
            return;
        }
        for (const std::string& already : open) {
            if (already == file) {
                errors.push_back(ScriptError{0, "'" + file + "' includes itself"});
                return;
            }
        }
        std::ifstream in(file, std::ios::binary);
        if (!in) {
            errors.push_back(ScriptError{0, "could not open '" + file + "'"});
            return;
        }
        open.push_back(file);

        const std::filesystem::path here = std::filesystem::path(file).parent_path();
        std::string line;
        u32 number = 0;
        while (std::getline(in, line)) {
            ++number;
            if (!line.empty() && line.back() == '\r') line.pop_back();

            // `include "some/other.clip"`, resolved relative to the file doing the including, so
            // a fragment can be moved with its neighbours and still find them.
            usize at = line.find_first_not_of(" \t");
            if (at != std::string::npos && line.compare(at, 7, "include") == 0) {
                usize rest = at + 7;
                const usize quote = line.find('"', rest);
                const usize close = (quote == std::string::npos)
                                        ? std::string::npos
                                        : line.find('"', quote + 1);
                if (quote == std::string::npos || close == std::string::npos) {
                    errors.push_back(ScriptError{number,
                                                 "include needs a quoted file name, in '" + file +
                                                     "'"});
                    continue;
                }
                const std::string named = line.substr(quote + 1, close - quote - 1);
                std::filesystem::path target = std::filesystem::path(named);
                if (target.is_relative()) target = here / target;
                std::string resolved = target.lexically_normal().string();
                std::error_code missing;
                // Next to the file always wins; the folder the game ships is where it looks when
                // there is nothing there (D494). That order is the whole design: a player who
                // copies the facility's parts beside their own world and edits them gets their
                // edits, and one who never copies anything still opens the world.
                if ((!std::filesystem::exists(resolved, missing) || missing) && !beside.empty()) {
                    missing.clear();
                    const std::filesystem::path fallback =
                        (std::filesystem::path(beside) / named).lexically_normal();
                    if (std::filesystem::exists(fallback, missing) && !missing) {
                        resolved = fallback.string();
                    }
                } else if (!beside.empty()) {
                    // A copy beside the world WON, and the game ships a different one. Said out
                    // loud, because the silence is the whole bug.
                    //
                    // Before D494 the facility's parts were copied into the player's worlds
                    // folder. That copying stopped; the copies did not go anywhere. So a shelf
                    // that has been through an upgrade still has a folder of fragments dated
                    // whenever it was made, and "beside always wins" -- which is right, and is
                    // what lets a player edit the parts -- makes that folder the building. Every
                    // fix to the shipped clip since then goes into the game and never into the
                    // world, and from the outside it looks like a world that refuses to change:
                    // reported as arches still barred after the arch was fixed, and reasonably
                    // blamed on a cache, because a frozen world and a stale cache look identical
                    // from the player's chair.
                    //
                    // Not fixed by preferring the shipped file. That would silently throw away
                    // the edits of the player D494 was written for. The fault is that the choice
                    // was invisible, so this makes it visible and leaves the choice alone: an
                    // include that is shadowed says so, by name, with both dates.
                    const std::filesystem::path shipped =
                        (std::filesystem::path(beside) / named).lexically_normal();
                    std::error_code either;
                    if (std::filesystem::exists(shipped, either) && !either &&
                        shipped.lexically_normal() !=
                            std::filesystem::path(resolved).lexically_normal()) {
                        const auto read_all = [](const std::string& from) {
                            std::ifstream in(from, std::ios::binary);
                            return std::string((std::istreambuf_iterator<char>(in)),
                                               std::istreambuf_iterator<char>());
                        };
                        if (read_all(resolved) != read_all(shipped.string())) {
                            WS_LOG_WARN("clip",
                                        "'{}' is being taken from beside the world and NOT from "
                                        "the game's own clips, and the two differ. The copy beside "
                                        "it is what this world is built from; delete it to follow "
                                        "the game's",
                                        named);
                        }
                    }
                }
                // Said HERE, with the line that asked for it and the name it asked for, rather
                // than as "could not open <absolute path>" from inside the recursion. A piece
                // going missing is the one include failure a player can actually cause — by
                // deleting or moving the folder a building is assembled out of — and the sentence
                // has to be the one that tells them that.
                missing.clear();
                if (!std::filesystem::exists(resolved, missing) || missing) {
                    errors.push_back(ScriptError{
                        number, "there is no '" + named + "' beside '" +
                                    std::filesystem::path(file).filename().string() +
                                    "': this file is built out of pieces and one of them is gone"});
                    continue;
                }
                pull(resolved, depth + 1);
                continue;
            }

            out += line;
            out += '\n';
            origin.push_back(SourceLine{file, number});
        }
        open.pop_back();
    };

    pull(std::filesystem::path(path).lexically_normal().string(), 0);
    // A document with a hole in it is not a document, and every caller has to agree about that.
    //
    // Returning the lines that DID load was the old behaviour, and what it produced was a building
    // whose twenty pieces were nineteen: forty cascading complaints about names declared in the
    // file that is gone, then a shape with nothing in it, then an empty sky. The one line that says
    // what actually happened is the first of the forty, which is the same as not saying it.
    //
    // The rule lives HERE rather than in `load_clip_script`, because that is not the only caller —
    // the application splices the world's source itself, so that the cache key covers the whole
    // assembly, and a rule stated in one of two call sites is a rule the other one does not have.
    if (!errors.empty()) {
        out.clear();
        origin.clear();
    }
    return out;
}

Script load_clip_script(const std::string& path, VoxelTypeTable& types, const TagRegistry& tags) {
    std::vector<SourceLine> origin;
    std::vector<ScriptError> trouble;
    const std::string text = expand_includes(path, origin, trouble);
    // A document with a hole in it is not parsed at all.
    //
    // It used to be parsed anyway, on the grounds that the lines that ARE there might still say
    // something. They do not: a building assembled out of twenty pieces, with one of them missing,
    // parses into forty cascading complaints about names that were declared in the file that is
    // gone — and the one line that says what actually happened is the first of the forty, which
    // is the same as not saying it. It then builds to nothing and opens as an empty sky, which
    // reads as the renderer having broken rather than as a file having been deleted.
    //
    // So any trouble at the include level ends it here, and what comes back is the cause on its
    // own. This is `14-ui-style.md`'s rule about refusals applied to a file: a refusal that does
    // not explain itself is indistinguishable from a bug.
    if (!trouble.empty()) {
        Script script;
        script.errors = trouble;
        return script;
    }

    Script script = parse_clip_script(text, types, tags);
    script.errors.insert(script.errors.begin(), trouble.begin(), trouble.end());

    // Errors come back numbered against the spliced text, which is a file nobody wrote. Put them
    // back where they came from — a line number that means nothing is worse than none.
    for (ScriptError& error : script.errors) {
        if (error.line == 0 || error.line > origin.size()) continue;
        const SourceLine& from = origin[error.line - 1];
        error.line = from.line;
        error.message = from.file + ": " + error.message;
    }
    script.sources = std::move(origin);
    return script;
}

}  // namespace forge
}  // namespace ws
