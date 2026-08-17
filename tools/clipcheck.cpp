// clipcheck — the clip tool, without a window, a card, or a Vulkan SDK.
//
// `WorldShaper.exe --clip-file ...` already answers every question this answers, and it is the
// tool the brief tells a fragment author to run. It is also linked into the game executable, so
// asking it anything costs an SDL fetch, a Vulkan SDK and a Windows toolchain. That is the right
// trade on the machine the game is built on and the wrong one everywhere else: a person — or an
// agent — writing a `.clip` is editing text that is parsed and sampled by `ws_forge`, which
// depends on nothing but the standard library, and they should not need a graphics stack to find
// out that they typed `cylinder` where they meant `capsule`.
//
// So this is the same four calls the game makes — parse, sample, despeckle, measure — against the
// same libraries, with the report cut down to what an author acts on:
//
//   errors        with the file and line the author actually wrote, through the include splice
//   extent        and the worldbox, which is where the part really is
//   components    1, or a list of what is floating and where
//   materials     including THE RULES THAT PAINTED NOTHING, which is the failure that looks like
//                 success — and it is three different things, `never asked`, `never matched` and
//                 `too coarse`, which the one line this used to print could not tell apart
//   spans         head height and doorway width, on request
//   a slice       when a number will not do
//   THE CUTS      what every `difference` actually removed, and whose matter it was
//
// Build it with tools/clipcheck.sh. It is not part of the CMake build and does not belong in it:
// the game's build already contains this tool, and a second copy of it in the same build system is
// two things to keep in step.
//
// Usage:
//   clipcheck <file.clip> [--part part_name] [--metre 8] [--slice x|y|z@2.0] [--quiet]
//             [--span y@0,0] [--gap x@1.5,-9.0] [--list]
//             [--cuts] [--cut <name>] [--cut-skeleton n]
//
//
// ============================================================================================
// THE CUT AUDIT — why a `difference` needs one at all
// ============================================================================================
//
// Every other number in this file is about what is THERE. A subtraction is the one operation in
// the language whose whole effect is on what is NOT, and so it is the one operation that leaves
// no trace to measure. You see the result; you never see what was taken out of it, or whose it
// was. Two bugs of exactly that shape have already cost this repository a day each:
//
//   D608   every room's void was subtracted from a union that contained the room's furniture, so
//          the sconces, benches and statues standing in those rooms were deleted by the rooms
//          they stood in. Nothing errored. The building measured smaller by a few litres out of
//          three and a half thousand cubic metres, and looked right from outside.
//   the windows' openings are cut 1.80 m deep "which is more than any sane outer wall" — a
//          reasonable thing to write, and a cut that keeps going into whatever happens to stand
//          behind the wall. Whatever that is, it is removed silently.
//
// Both are the same question asked of the wrong thing: not "is my building right" but "what did
// THIS void eat". So:
//
//   --cuts        every subtracted operand in the clip, how much matter it removed, how much of
//                 that only IT removed, and which named parts the matter belonged to.
//   --cut <name>  one of them in full: the box of what it took out of each victim, and a section
//                 drawn through it.
//
// # How it is measured, and why this is not a guess
//
// `Op::Difference` is `max(base, -s1, -s2, ...)`. Matter is removed by operand k at exactly the
// points where the base is inside (< 0) and s_k is inside (< 0). That is not an approximation of
// the subtraction, it IS the subtraction, evaluated per voxel at the same centres the sampler
// asks about. So for every voxel inside some operand's bounding box:
//
//   evaluate each operand whose box contains the point   -> which cuts claim it
//   nothing claims it                                    -> skip, cost is one box test
//   evaluate the base of the difference chain            -> was there anything here to remove
//   base >= 0                                            -> the cut is chewing air; counted apart
//   base <  0                                            -> REMOVED. One cut claiming it = ONLY IT
//
// "Only it" is the number that matters. A void overlapping another void is harmless — the matter
// was going anyway. A void that is the sole reason some matter is missing is the one that can be
// hiding a bug, and it is also the one that would be caught by deleting the void and looking,
// which is what nobody does because the building takes three minutes to sample.
//
// A cut whose removal is ZERO is the mirror-image bug and just as silent: an opening that missed
// its wall leaves no hole, no error and no difference in any other number here. `steps.clip` shipped
// one — `offset { steps_mass } by=-0.045` was meant to shrink a core and grew it, so
// `difference { mass core }` was empty and every joint in the great stair did not exist. Both sides
// sampled to the identical 2,423,674 voxels. This mode reported `steps_joints  REMOVED NOTHING` on
// the tree that had it.
//
// The zero has ONE false alarm and it is worth knowing about: a cut narrower than a voxel removes
// nothing at that resolution and everything at a finer one. So a zero is printed with how thick the
// cut's own box is IN VOXELS whenever that is under two, which is the difference between a bug and
// a `--metre` that is too coarse to see the answer.
//
// # Whose matter was it
//
// A clip binds thousands of names (the facility binds 2538), so "which named part" cannot be a
// scan of all of them. The victims are taken from the ASSEMBLY SKELETON instead: walk down from
// the difference's own base through unions and pass-through transforms, and collect every name
// on the way, stopping where the shape stops being an assembly and starts being a shape. That is
// `part_walls`, `part_stair`, `part_fittings` — the words the manifest is written in. A removed
// voxel is charged to the name with the SMALLEST bounding box that contains it, so the answer is
// the most specific one available rather than "it was in the building".
//
// # What this cannot see, and says so rather than guessing
//
// A cut underneath a `repeat`, a `polar_repeat` or a `revolve` is at a point this cannot map back
// to world coordinates — those fold or re-dimension the space, so one world point is many local
// ones. Such cuts are listed by name under `not audited` and are not silently counted as zero,
// because "I could not" and "it removed nothing" are the two answers that must never be confused
// (trap 7). Translate, rotate, scale and mirror ARE followed, exactly as `Field::eval` does them.
//
// A `smooth_difference` blends rather than cuts, so its removal has no sharp boundary; it is
// audited by the same sign test and flagged, because the number is then a lower bound.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/jobs.hpp"
#include "forge/clip_script.hpp"
#include "forge/measure.hpp"
#include "forge/sample.hpp"
#include "world/voxel_type.hpp"
#include "world/tags.hpp"

using namespace ws;

namespace {

// An error carries a line of the SPLICED text, and the author wrote a line of one of twenty-odd
// fragments. Reporting the first is reporting a line number in a file that does not exist.
void say_error(const forge::Script& script, const forge::ScriptError& error) {
    if (error.line > 0 && error.line <= script.sources.size()) {
        const forge::SourceLine& where = script.sources[error.line - 1];
        std::printf("ERROR  %s:%u: %s\n", where.file.c_str(), where.line, error.message.c_str());
    } else if (error.line > 0) {
        std::printf("ERROR  line %u: %s\n", error.line, error.message.c_str());
    } else {
        std::printf("ERROR  %s\n", error.message.c_str());
    }
}

struct Axis {
    u32 axis = 1;
    f64 a = 0.0;
    f64 b = 0.0;
    bool given = false;
};

// The two axes a query about `axis` is positioned by, IN THE ORDER measure.cpp asks for them.
//
// This is not (axis+1)%3, (axis+2)%3, and writing it that way was a real bug that shipped in the
// first version of this file. `other_axes` in measure.cpp returns (0,2) for axis 1, and the cyclic
// form gives (2,0) — so every `--gap y@a,b` converted `a` against the z origin and then indexed x.
// It did not fail. It answered a different question, plausibly, in metres, and one agent measuring
// head height in a crypt spent an afternoon on a scatter of points that reported 0.000 m of air
// where a slice at the same resolution showed open floor.
//
// x and z happen to agree between the two forms, which is exactly why it survived being used.
void query_axes(u32 axis, u32& a, u32& b) {
    if (axis == 0) { a = 1; b = 2; }
    else if (axis == 1) { a = 0; b = 2; }
    else { a = 0; b = 1; }
}


// THE LONGEST CONTIGUOUS RUN OF AIR along one axis, with stone at both ends of it.
//
// This is what an author means by "head height", and neither `span_along` nor `gap_along`
// answers it. Both report from the FIRST empty cell to the LAST, so on any probe whose box is
// taller than the room -- which is every probe, because a clip's bounds are cut to the building
// and not to one storey -- they report the box and flag it BROKEN because the ceiling is in
// between. Measured on the crypt at eight points across the floor, `--gap y@...` said 3.125 m of
// air, BROKEN, everywhere; the room is 2.14 m high. The number was not wrong, it was the answer
// to a different question, and the crypt's own notes record that `--gap` "needs a probe box whose
// top and bottom are inside stone", which nothing tells anybody and no clip can promise.
//
// So: walk the column, keep the longest unbroken run, and say whether stone closed it at each
// end. A run open at one end is a shaft to the sky or to the bottom of the box and is reported as
// such rather than silently counted as headroom. Rule 7 of the brief -- 2.10 m minimum anywhere a
// person can stand -- is checkable in one command with this and was not with either of the others.
struct Clear {
    f64 metres = 0.0;
    bool closed_below = false;
    bool closed_above = false;
    i32 from = 0;
};

Clear longest_clear(const Clip& clip, u32 axis, i32 a, i32 b) {
    u32 pa = 0, pb = 0;
    query_axes(axis, pa, pb);
    i32 at[3]{0, 0, 0};
    at[pa] = a;
    at[pb] = b;
    Clear best;
    if (a < 0 || b < 0 || a >= clip.size[pa] || b >= clip.size[pb]) return best;
    const i32 n = clip.size[axis];
    i32 run = 0;
    for (i32 i = 0; i <= n; ++i) {
        const bool air = (i < n) && (at[axis] = i, clip.at(at[0], at[1], at[2]) == 0);
        if (air) {
            ++run;
            continue;
        }
        if (run > 0 && static_cast<f64>(run) > best.metres) {
            best.metres = static_cast<f64>(run);
            best.from = i - run;
            best.closed_below = (i - run) > 0;
            best.closed_above = (i < n);
        }
        run = 0;
    }
    return best;
}

// "y@1.5,-3.0" — an axis, and where on the other two. Written this way because a span is always
// asked as "how tall is it here", and "here" is two numbers in metres.
Axis parse_axis(const char* text) {
    Axis out;
    if (!text || !*text) return out;
    switch (text[0]) {
        case 'x': out.axis = 0; break;
        case 'y': out.axis = 1; break;
        case 'z': out.axis = 2; break;
        default: return out;
    }
    const char* at = std::strchr(text, '@');
    if (at) {
        out.a = std::atof(at + 1);
        const char* comma = std::strchr(at + 1, ',');
        if (comma) out.b = std::atof(comma + 1);
    }
    out.given = true;
    return out;
}

// ============================================================================================
// The cut audit. See the note at the top of the file for what it answers and how.
// ============================================================================================

using forge::Op;
using forge::Vec3;
using Aabb = forge::Field::Aabb;

constexpr f64 kTurn = 6.283185307179586476925286766559;

// The transform chain from the sampled root down to a node: world in, local out.
//
// Stored as copies of the nodes themselves rather than as a matrix, so that applying it is
// literally the arithmetic `Field::eval` does and cannot drift from it. A matrix would also be
// wrong for `mirror`, which is a fold rather than a linear map.
using Frame = std::vector<forge::Node>;

// A fold is single-valued for the question this asks.
//
// `repeat` and a whole-turn `around` map a world point to exactly one point of the copy it is in
// — a copy lives inside its own cell or its own sector, so the primary fold IS the copy that
// contains the point, and the extra evaluations `eval` does for leaning neighbours only ever
// find something FURTHER away. That makes both exact for an inside/outside test, which is all a
// cut audit ever asks, even though neither is exact for a distance. `twist` and `bend` are plain
// point transforms and exact outright.
//
// A PARTIAL `around` is not: its copies are spaced inclusively over an arc and a point off the
// end of the arc folds onto the nearest end rather than into its own sector. That is a different
// question and it is refused rather than approximated.
bool partial_sweep(f64 span) { return span > 0.0 && span < 1.0; }

bool frame_op_supported(const forge::Node& n) {
    switch (n.op) {
        case Op::Translate:
        case Op::Rotate:
        case Op::Scale:
        case Op::Mirror:
        case Op::Repeat:
        case Op::Twist:
        case Op::Bend: return true;
        case Op::PolarRepeat: return !partial_sweep(n.a[3]);
        default: return false;
    }
}

Vec3 rotate_forward(Vec3 p, const f64* a) {
    const f64 cx = std::cos(-a[0] * kTurn), sx = std::sin(-a[0] * kTurn);
    const f64 cy = std::cos(-a[1] * kTurn), sy = std::sin(-a[1] * kTurn);
    const f64 cz = std::cos(-a[2] * kTurn), sz = std::sin(-a[2] * kTurn);
    Vec3 q = p;
    q = {q.x, q.y * cx - q.z * sx, q.y * sx + q.z * cx};
    q = {q.x * cy + q.z * sy, q.y, -q.x * sy + q.z * cy};
    q = {q.x * cz - q.y * sz, q.x * sz + q.y * cz, q.z};
    return q;
}

// The same three turns undone, in the opposite order. Used only to carry a BOX back out to world
// coordinates; the point itself always travels the forward way.
Vec3 rotate_back(Vec3 q, const f64* a) {
    const f64 cx = std::cos(a[0] * kTurn), sx = std::sin(a[0] * kTurn);
    const f64 cy = std::cos(a[1] * kTurn), sy = std::sin(a[1] * kTurn);
    const f64 cz = std::cos(a[2] * kTurn), sz = std::sin(a[2] * kTurn);
    Vec3 p = q;
    p = {p.x * cz - p.y * sz, p.x * sz + p.y * cz, p.z};
    p = {p.x * cy + p.z * sy, p.y, -p.x * sy + p.z * cy};
    p = {p.x, p.y * cx - p.z * sx, p.y * sx + p.z * cx};
    return p;
}

f64 axis_value(Vec3 p, u32 axis) { return axis == 0 ? p.x : (axis == 1 ? p.y : p.z); }
void set_axis(Vec3& p, u32 axis, f64 v) {
    if (axis == 0) p.x = v;
    else if (axis == 1) p.y = v;
    else p.z = v;
}

Vec3 to_local(const Frame& frame, Vec3 p) {
    for (const forge::Node& n : frame) {
        switch (n.op) {
            case Op::Translate: p = p - Vec3{n.a[0], n.a[1], n.a[2]}; break;
            case Op::Rotate: p = rotate_forward(p, n.a); break;
            case Op::Scale: {
                const f64 sx = n.a[0] != 0.0 ? n.a[0] : 1.0;
                const f64 sy = n.a[1] != 0.0 ? n.a[1] : 1.0;
                const f64 sz = n.a[2] != 0.0 ? n.a[2] : 1.0;
                p = {p.x / sx, p.y / sy, p.z / sz};
                break;
            }
            case Op::Mirror: {
                const u32 axis = static_cast<u32>(n.a[0]) % 3;
                set_axis(p, axis, std::abs(axis_value(p, axis)));
                break;
            }
            case Op::Repeat: {
                for (u32 axis = 0; axis < 3; ++axis) {
                    const f64 period = n.a[axis];
                    if (period <= 0.0) continue;
                    const f64 limit = n.a[3 + axis];
                    const f64 value = axis_value(p, axis);
                    f64 cell = std::round(value / period);
                    if (limit > 0.0) cell = std::max(-limit, std::min(limit, cell));
                    set_axis(p, axis, value - period * cell);
                }
                break;
            }
            case Op::PolarRepeat: {
                const u32 count = std::max(1u, static_cast<u32>(n.a[0]));
                const u32 axis = static_cast<u32>(n.a[1]) % 3;
                u32 u = 0, v = 0;
                query_axes(axis, u, v);
                const f64 ux = axis_value(p, u), vy = axis_value(p, v);
                const f64 sector = kTurn / static_cast<f64>(count);
                f64 angle = std::atan2(vy, ux);
                angle -= sector * std::round(angle / sector);
                const f64 r = std::hypot(ux, vy);
                set_axis(p, u, std::cos(angle) * r);
                set_axis(p, v, std::sin(angle) * r);
                break;
            }
            case Op::Twist:
            case Op::Bend: {
                const u32 axis = static_cast<u32>(n.a[1]) % 3;
                u32 u = 0, v = 0;
                query_axes(axis, u, v);
                const f64 along = (n.op == Op::Twist) ? axis_value(p, axis) : axis_value(p, u);
                const f64 angle = -n.a[0] * kTurn * along;
                const f64 c = std::cos(angle), s = std::sin(angle);
                const f64 ux = axis_value(p, u), vy = axis_value(p, v);
                set_axis(p, u, ux * c - vy * s);
                set_axis(p, v, ux * s + vy * c);
                break;
            }
            default: break;
        }
    }
    return p;
}

// A node's own box, carried back out to the coordinates the sample is indexed in. Conservative
// everywhere it cannot be exact: a rotation takes the box round the eight moved corners, and a
// mirror takes the box round both sides of the fold.
Aabb to_world(const Frame& frame, Aabb box) {
    for (usize i = frame.size(); i-- > 0;) {
        if (box.infinite()) return box;
        const forge::Node& n = frame[i];
        switch (n.op) {
            case Op::Translate: {
                const Vec3 by{n.a[0], n.a[1], n.a[2]};
                box.low = box.low + by;
                box.high = box.high + by;
                break;
            }
            case Op::Rotate: {
                Aabb out;
                out.low = {1e30, 1e30, 1e30};
                out.high = {-1e30, -1e30, -1e30};
                for (u32 c = 0; c < 8; ++c) {
                    const Vec3 corner{(c & 1) ? box.high.x : box.low.x,
                                      (c & 2) ? box.high.y : box.low.y,
                                      (c & 4) ? box.high.z : box.low.z};
                    const Vec3 w = rotate_back(corner, n.a);
                    out.low = {std::min(out.low.x, w.x), std::min(out.low.y, w.y),
                               std::min(out.low.z, w.z)};
                    out.high = {std::max(out.high.x, w.x), std::max(out.high.y, w.y),
                                std::max(out.high.z, w.z)};
                }
                box = out;
                break;
            }
            case Op::Scale: {
                const f64 s[3]{n.a[0] != 0.0 ? n.a[0] : 1.0, n.a[1] != 0.0 ? n.a[1] : 1.0,
                               n.a[2] != 0.0 ? n.a[2] : 1.0};
                f64 lo[3]{box.low.x, box.low.y, box.low.z};
                f64 hi[3]{box.high.x, box.high.y, box.high.z};
                for (u32 axis = 0; axis < 3; ++axis) {
                    const f64 a = lo[axis] * s[axis], b = hi[axis] * s[axis];
                    lo[axis] = std::min(a, b);
                    hi[axis] = std::max(a, b);
                }
                box.low = {lo[0], lo[1], lo[2]};
                box.high = {hi[0], hi[1], hi[2]};
                break;
            }
            case Op::Mirror: {
                const u32 axis = static_cast<u32>(n.a[0]) % 3;
                const f64 reach = std::max(std::abs(axis_value(box.low, axis)),
                                           std::abs(axis_value(box.high, axis)));
                set_axis(box.low, axis, -reach);
                set_axis(box.high, axis, reach);
                break;
            }
            // Where the copies of one shape reach: its own box slid out to the last cell the
            // limit allows. An UNLIMITED repeat reaches everywhere, and says so — a box that
            // lied here would silently drop every voxel outside it from the audit.
            case Op::Repeat: {
                for (u32 axis = 0; axis < 3; ++axis) {
                    const f64 period = n.a[axis];
                    if (period <= 0.0) continue;
                    const f64 limit = n.a[3 + axis];
                    if (limit <= 0.0) {
                        set_axis(box.low, axis, -1e30);
                        set_axis(box.high, axis, 1e30);
                        continue;
                    }
                    set_axis(box.low, axis, axis_value(box.low, axis) - period * limit);
                    set_axis(box.high, axis, axis_value(box.high, axis) + period * limit);
                }
                break;
            }
            // Turned all the way round, so the envelope is the ring the box sweeps out. Taken as
            // the square about that ring, which is loose and cannot be wrong.
            case Op::PolarRepeat:
            case Op::Twist:
            case Op::Bend: {
                const u32 axis = static_cast<u32>(n.a[1]) % 3;
                u32 u = 0, v = 0;
                query_axes(axis, u, v);
                f64 reach = 0.0;
                for (u32 c = 0; c < 4; ++c) {
                    const f64 a = (c & 1) ? axis_value(box.high, u) : axis_value(box.low, u);
                    const f64 b = (c & 2) ? axis_value(box.high, v) : axis_value(box.low, v);
                    reach = std::max(reach, std::hypot(a, b));
                }
                // The two cross axes both go round, and a box straddling the axis has its nearest
                // point ON the axis, so the envelope is the full square either way. The third
                // axis is untouched by all three of these ops.
                set_axis(box.low, u, -reach);
                set_axis(box.high, u, reach);
                set_axis(box.low, v, -reach);
                set_axis(box.high, v, reach);
                break;
            }
            default: break;
        }
    }
    return box;
}

bool inside_box(const Aabb& box, Vec3 p) {
    return p.x >= box.low.x && p.x <= box.high.x && p.y >= box.low.y && p.y <= box.high.y &&
           p.z >= box.low.z && p.z <= box.high.z;
}

f64 box_volume(const Aabb& box) {
    if (box.infinite()) return 1e60;
    const f64 dx = std::max(0.0, box.high.x - box.low.x);
    const f64 dy = std::max(0.0, box.high.y - box.low.y);
    const f64 dz = std::max(0.0, box.high.z - box.low.z);
    return dx * dy * dz;
}

// A box grown one voxel at a time, in the clip's own indices.
struct VoxelBox {
    bool any = false;
    i32 low[3]{0, 0, 0};
    i32 high[3]{0, 0, 0};
    void add(i32 x, i32 y, i32 z) {
        const i32 p[3]{x, y, z};
        if (!any) {
            for (u32 i = 0; i < 3; ++i) low[i] = high[i] = p[i];
            any = true;
            return;
        }
        for (u32 i = 0; i < 3; ++i) {
            low[i] = std::min(low[i], p[i]);
            high[i] = std::max(high[i], p[i]);
        }
    }
    void merge(const VoxelBox& other) {
        if (!other.any) return;
        if (!any) { *this = other; return; }
        for (u32 i = 0; i < 3; ++i) {
            low[i] = std::min(low[i], other.low[i]);
            high[i] = std::max(high[i], other.high[i]);
        }
    }
};

// One subtracted operand of one `difference`.
struct Cut {
    u32 node = 0;         // the operand
    u32 owner = 0;        // the difference it hangs off
    u32 slot = 0;         // which operand of that difference, 1-based among the subtracted ones
    u32 base = 0;         // the uncut matter the whole chain of differences carves
    u32 group = 0;        // which audit pass it belongs to (one per base + frame)
    bool smooth = false;
    bool audited = true;  // false when the frame cannot be inverted
    const char* refused = "";
    std::string name;
    Frame frame;
    Aabb world_box;
};

// A name the audit can charge removed matter to.
struct Victim {
    std::string name;
    u32 node = 0;
    Frame frame;
    Aabb world_box;
    f64 volume = 0.0;
};

// What one cut turned out to have done. Indices line up with the cut list.
struct Tally {
    u64 removed = 0;    // base had matter here and this cut took it
    u64 only = 0;       // ... and no other cut in the same difference would have
    u64 on_air = 0;     // this cut covers the point but there was nothing there to remove
    VoxelBox removed_box;
    VoxelBox only_box;
    std::vector<u64> victim;        // per victim, over `removed`
    std::vector<u64> victim_only;   // per victim, over `only`
    std::vector<VoxelBox> victim_box;
    u64 unattributed = 0;

    // Only filled in for a cut that removed nothing. See `probe_finer`.
    i32 finer = 0;                  // the multiple of --metre the probe used; 0 = not probed
    bool finer_found = false;
    Vec3 finer_at{0, 0, 0};
};

// A cut that removed nothing gets asked again, FINER.
//
// A cut narrower than a voxel removes nothing at that resolution and everything at a finer one,
// and the two are the same report. `steps_cope_joints` on the facility is a 0.0375 m rind: zero at
// metre 8 where a voxel is 0.125 m, 0.031 m3 at metre 32. `steps_joints` with its `offset` sign
// backwards is zero at every resolution there is. Telling those apart is the whole worth of the
// zero, so it is measured rather than reasoned about from the cut's bounding box — which is the
// intersection of two large boxes for `steps_cope_joints` and says nothing about how thin the cut is.
//
// Stops at the first point it finds, so the cheap answer (there IS something, your metre is too
// coarse) costs almost nothing and the expensive one is the case worth paying for.
struct Probe {
    i32 factor = 0;
    bool found = false;
    Vec3 at{0, 0, 0};
};

Probe probe_finer(const forge::Field& field, const Cut& cut, u32 base, const Frame& base_frame,
                  i32 per, u64 budget) {
    Probe out;
    if (cut.world_box.infinite()) return out;
    const f64 span[3]{cut.world_box.high.x - cut.world_box.low.x,
                      cut.world_box.high.y - cut.world_box.low.y,
                      cut.world_box.high.z - cut.world_box.low.z};
    for (const i32 factor : {4, 2}) {
        const f64 step = 1.0 / static_cast<f64>(per * factor);
        f64 points = 1.0;
        for (u32 axis = 0; axis < 3; ++axis) points *= std::floor(span[axis] / step) + 1.0;
        if (points > static_cast<f64>(budget)) continue;
        out.factor = factor;
        const i32 n[3]{static_cast<i32>(span[0] / step) + 1, static_cast<i32>(span[1] / step) + 1,
                       static_cast<i32>(span[2] / step) + 1};
        for (i32 k = 0; k < n[2]; ++k) {
            for (i32 j = 0; j < n[1]; ++j) {
                for (i32 i = 0; i < n[0]; ++i) {
                    const Vec3 p{cut.world_box.low.x + (static_cast<f64>(i) + 0.5) * step,
                                 cut.world_box.low.y + (static_cast<f64>(j) + 0.5) * step,
                                 cut.world_box.low.z + (static_cast<f64>(k) + 0.5) * step};
                    if (field.eval(cut.node, to_local(cut.frame, p)) >= 0.0) continue;
                    if (field.eval(base, to_local(base_frame, p)) >= 0.0) continue;
                    out.found = true;
                    out.at = p;
                    return out;
                }
            }
        }
        return out;
    }
    return out;
}

void merge_tally(Tally& into, const Tally& from) {
    into.removed += from.removed;
    into.only += from.only;
    into.on_air += from.on_air;
    into.unattributed += from.unattributed;
    into.removed_box.merge(from.removed_box);
    into.only_box.merge(from.only_box);
    for (usize i = 0; i < from.victim.size(); ++i) {
        into.victim[i] += from.victim[i];
        into.victim_only[i] += from.victim_only[i];
        into.victim_box[i].merge(from.victim_box[i]);
    }
}

// Walking the graph to find every subtraction, and keeping track of two things on the way down.
//
// The FRAME, so a cut deep under a translate can still be reported in the coordinates a person
// types. And the POLARITY: a difference inside a subtracted operand does not remove matter from
// the clip, it puts some back — `void_windows = difference { windows_cuts part_windows }` is the
// idiom that stops a window's own opening eating the window's own frame — so counting it as a cut
// would report the exact opposite of what it does.
struct Walk {
    std::vector<Cut> cuts;
    usize restores = 0;      // differences found inside a subtracted operand
    usize reshared = 0;      // nodes reached by more than one path; the first frame was kept
};

u32 chain_base(const forge::Field& field, u32 difference) {
    // `difference { a b c d e }` folds into a chain of nodes of four children each, and a nested
    // `difference` in the first position means the same thing anyway, since max is associative.
    // Either way the matter being carved is at the bottom of the chain.
    u32 at = field.node(difference).child[0];
    for (u32 guard = 0; guard < 4096 && field.node(at).op == Op::Difference; ++guard) {
        at = field.node(at).child[0];
    }
    return at;
}

void walk_for_cuts(const forge::Field& field, u32 at, const Frame& frame, bool frame_exact,
                   const char* refused, int polarity, std::vector<u8>& seen, Walk& out) {
    if (at >= field.size()) return;
    const u8 bit = polarity > 0 ? 1 : 2;
    if (seen[at] & bit) { ++out.reshared; return; }
    seen[at] = static_cast<u8>(seen[at] | bit);

    const forge::Node& n = field.node(at);
    switch (n.op) {
        case Op::Difference:
        case Op::SmoothDifference: {
            walk_for_cuts(field, n.child[0], frame, frame_exact, refused, polarity, seen, out);
            for (u32 i = 1; i < n.children; ++i) {
                if (polarity > 0) {
                    Cut cut;
                    cut.node = n.child[i];
                    cut.owner = at;
                    cut.slot = i;
                    cut.base = (n.op == Op::Difference) ? chain_base(field, at) : n.child[0];
                    cut.smooth = (n.op == Op::SmoothDifference);
                    cut.frame = frame;
                    cut.audited = frame_exact;
                    cut.refused = refused;
                    cut.world_box = to_world(frame, field.bounds_of(n.child[i]));
                    out.cuts.push_back(std::move(cut));
                } else {
                    ++out.restores;
                }
                walk_for_cuts(field, n.child[i], frame, frame_exact, refused, -polarity, seen, out);
            }
            return;
        }
        case Op::Translate:
        case Op::Rotate:
        case Op::Scale:
        case Op::Mirror:
        case Op::Repeat:
        case Op::PolarRepeat:
        case Op::Twist:
        case Op::Bend: {
            if (!frame_op_supported(n)) {
                walk_for_cuts(field, n.child[0], frame, false, "under a partial around", polarity,
                              seen, out);
                return;
            }
            Frame next = frame;
            next.push_back(n);
            walk_for_cuts(field, n.child[0], next, frame_exact, refused, polarity, seen, out);
            return;
        }
        // A profile turned about an axis is asked in two dimensions, so a world point has no one
        // place in it. The child is still walked, so a cut underneath one is FOUND and named — it
        // simply cannot be placed, and says so.
        case Op::Revolve:
            walk_for_cuts(field, n.child[0], frame, false, "under a revolve", polarity, seen, out);
            return;
        // The pattern of a displacement is a value and not a shape; walking into it would find
        // "cuts" in a noise expression.
        case Op::Displace:
            walk_for_cuts(field, n.child[0], frame, frame_exact, refused, polarity, seen, out);
            return;
        case Op::Union:
        case Op::SmoothUnion:
        case Op::Intersection:
        case Op::SmoothIntersection:
        case Op::Shell:
        case Op::Round:
        case Op::Offset:
            for (u32 i = 0; i < n.children; ++i) {
                walk_for_cuts(field, n.child[i], frame, frame_exact, refused, polarity, seen, out);
            }
            return;
        default: return;   // a primitive or a pattern: nothing below it is a subtraction
    }
}

// The assembly skeleton under one node: every name reachable through unions and pass-through
// transforms, stopping where the clip stops being an assembly and starts being a shape.
//
// This is what makes "whose matter was it" answerable at all. The facility binds 2538 names and
// almost all of them are mouldings and profiles; the ones a report can use are the ones the
// manifest is written in, and those are exactly the ones a walk of the unions reaches.
void collect_skeleton(const forge::Field& field, u32 at, const Frame& frame, bool frame_exact,
                      const std::unordered_map<u32, std::string>& names, u32 depth, u32 max_depth,
                      std::vector<u8>& seen, std::vector<Victim>& out) {
    if (at >= field.size() || depth > max_depth || out.size() >= 512) return;
    if (seen[at]) return;
    seen[at] = 1;

    const auto found = names.find(at);
    if (found != names.end() && depth > 0 && frame_exact) {
        Victim v;
        v.name = found->second;
        v.node = at;
        v.frame = frame;
        v.world_box = to_world(frame, field.bounds_of(at));
        v.volume = box_volume(v.world_box);
        out.push_back(std::move(v));
    }

    const forge::Node& n = field.node(at);
    switch (n.op) {
        case Op::Union:
        case Op::SmoothUnion:
            for (u32 i = 0; i < n.children; ++i) {
                collect_skeleton(field, n.child[i], frame, frame_exact, names, depth + 1,
                                 max_depth, seen, out);
            }
            return;
        // A carved thing is still an assembly of the thing it was carved from.
        case Op::Difference:
        case Op::SmoothDifference:
        case Op::Displace:
        case Op::Shell:
        case Op::Round:
        case Op::Offset:
            collect_skeleton(field, n.child[0], frame, frame_exact, names, depth + 1, max_depth,
                             seen, out);
            return;
        case Op::Translate:
        case Op::Rotate:
        case Op::Scale:
        case Op::Mirror:
        case Op::Repeat:
        case Op::PolarRepeat:
        case Op::Twist:
        case Op::Bend: {
            if (!frame_op_supported(n)) {
                collect_skeleton(field, n.child[0], frame, false, names, depth + 1, max_depth,
                                 seen, out);
                return;
            }
            Frame next = frame;
            next.push_back(n);
            collect_skeleton(field, n.child[0], next, frame_exact, names, depth + 1, max_depth,
                             seen, out);
            return;
        }
        case Op::Revolve:
            collect_skeleton(field, n.child[0], frame, false, names, depth + 1, max_depth, seen,
                             out);
            return;
        default: return;
    }
}

// A section through what a cut did, which a number cannot show: whether the over-cut is a shaving
// off a face or a hole clean through something.
//
//   #  matter that survived        X  removed, and this cut is the only reason
//   o  removed, another cut would have taken it anyway
//   .  air that was never anything
std::string cut_slice_text(const Clip& clip, const std::vector<u8>& mask, u8 bit, u32 axis, i32 at,
                           i32 step) {
    std::string out;
    if (clip.empty() || at < 0 || at >= clip.size[axis]) return out;
    if (step < 1) step = 1;
    u32 axis_a = 0, axis_b = 0;
    query_axes(axis, axis_a, axis_b);
    const i32 width = clip.size[axis_a];
    const i32 height = clip.size[axis_b];
    for (i32 b = height - 1; b >= 0; b -= step) {
        for (i32 a = 0; a < width; a += step) {
            i32 solid = 0, only = 0, shared = 0, total = 0;
            for (i32 db = 0; db < step && b - db >= 0; ++db) {
                for (i32 da = 0; da < step && a + da < width; ++da) {
                    i32 coord[3];
                    coord[axis] = at;
                    coord[axis_a] = a + da;
                    coord[axis_b] = b - db;
                    const usize index = clip.index(coord[0], coord[1], coord[2]);
                    ++total;
                    const u8 m = mask[index];
                    if (m & bit) {
                        if (m & 0x80) ++shared; else ++only;
                    } else if (clip.voxels[index] != 0) {
                        ++solid;
                    }
                }
            }
            if (total == 0) continue;
            // The cut wins every tie. A shaving one voxel thick that a scaled-down section
            // rounded away is exactly the thing this is drawn to find.
            if (only > 0) out += 'X';
            else if (shared > 0) out += 'o';
            else if (solid > 0) out += (solid * 2 >= total) ? '#' : '+';
            else out += '.';
        }
        out += '\n';
    }
    return out;
}

// ============================================================================================
// WHICH PAINT RULES ACTUALLY PAINTED SOMETHING
// ============================================================================================
//
// `SampleResult::rule_evaluations` counts how often each rule was EVALUATED, because
// `rule_cost[i]` is incremented immediately after `field.eval` and before the band is tested. So
// the line this file used to print as "never fired" meant "never reached", and a rule that is
// asked at four million voxels and matches none of them was reported as having fired. That is
// exactly the dead rule the line exists to catch: `surface.clip`'s `paint granite` has no voxel
// to land on anywhere in the facility and the report was silent about it.
//
// The two are different faults with different causes and both are worth saying:
//
//   never reached   the rule's box was culled everywhere, or the region it names is empty. The
//                   rule costs nothing and does nothing.
//   never matched   the rule was asked, at every solid voxel it could apply to, and its band
//                   never contained the answer. This one costs the whole build and does nothing.
//
// Counting matches where they happen would be one line in `src/forge/sample.cpp` beside the
// existing `rule_cost` increment, and that file belongs to another agent, so this is measured
// from outside instead: re-test the FINISHED clip's solid voxels against the rules the descent
// actually used, and stop as soon as every rule has matched once.
//
// # Why re-testing is faithful and which way it errs
//
// The per-voxel test in `paint_solid` is `value in [low, high]`, with an optional facing test, on
// the rules of `SamplePlan::widened` — not on the author's rules, which are widened by whatever a
// displacement can move a surface. So the plan is asked for here as well, and the same widened
// rule is tested. `PaintRule::place` never appears in that test at all; it is only ever a bounding
// box the descent culls with, so it is applied here as a box test too.
//
// Where the two cannot be identical, this one is deliberately the MORE PERMISSIVE:
//
//   the box cull      is a containment test grown by half a voxel diagonal rather than the
//                     descent's own box-against-box distance
//   the facing test   is NOT a gate here. A box that settles to "this rule applies" paints
//                     without consulting facing at all — see `state[i] == 1` in `paint_solid` —
//                     so gating on it here could call a rule dead that the sampler paints with.
//                     Facing is reported separately instead.
//
// Permissive in that direction means this can MISS a dead rule. It cannot invent one, which is the
// error that matters: a report that names a live rule as dead sends somebody to delete it.
struct RuleLife {
    bool matched = false;         // its band contained the answer at some solid voxel
    bool matched_facing = false;  // ... and the facing test passed there too

    // Only filled in for a rule that matched nothing. See `probe_rule_finer`.
    i32 finer = 0;                // the multiple of --metre the probe used; 0 = not probed
    bool finer_found = false;
    bool outside = false;         // its shape does not reach the sampled box at all
    bool gave_up = false;         // the finer sweep ran out of time; NOT checked
    Vec3 finer_at{0, 0, 0};
};

bool near_box(const Aabb& box, Vec3 p, f64 reach) {
    return p.x >= box.low.x - reach && p.x <= box.high.x + reach && p.y >= box.low.y - reach &&
           p.y <= box.high.y + reach && p.z >= box.low.z - reach && p.z <= box.high.z + reach;
}

// The cull the descent applies before it ever evaluates a rule: the rule's box, and the pieces its
// zone is really made of. Factored out because the probe below has to apply exactly the same one —
// a probe more permissive than the scan would call a rule alive on a point the sampler never
// offered it.
bool rule_reaches(const forge::SamplePlan& plan, usize i, Vec3 p, f64 reach) {
    if (i < plan.rule_box.size() && !near_box(plan.rule_box[i], p, reach)) return false;
    if (i + 1 < plan.rule_piece_at.size() && !plan.rule_piece.empty()) {
        const u32 from = plan.rule_piece_at[i];
        const u32 to = plan.rule_piece_at[i + 1];
        if (from != to) {
            for (u32 q = from; q < to; ++q) {
                if (near_box(plan.rule_piece[q], p, reach)) return true;
            }
            return false;
        }
    }
    return true;
}

std::vector<RuleLife> rules_that_painted(const forge::Field& field, const forge::SamplePlan& plan,
                                         const Clip& clip, const i64* origin, i32 per) {
    std::vector<RuleLife> life(plan.widened.size());
    if (plan.widened.empty() || clip.empty()) return life;
    const f64 voxel = 1.0 / static_cast<f64>(per);
    const f64 reach = voxel * 0.8660254037844386;   // half the diagonal of one voxel
    usize outstanding = life.size();

    for (i32 z = 0; z < clip.size[2] && outstanding > 0; ++z) {
        const f64 pz = (static_cast<f64>(origin[2] + z) + 0.5) * voxel;
        for (i32 y = 0; y < clip.size[1] && outstanding > 0; ++y) {
            const f64 py = (static_cast<f64>(origin[1] + y) + 0.5) * voxel;
            for (i32 x = 0; x < clip.size[0] && outstanding > 0; ++x) {
                if (clip.at(x, y, z) == 0) continue;
                const f64 px = (static_cast<f64>(origin[0] + x) + 0.5) * voxel;
                const Vec3 p{px, py, pz};
                Vec3 normal{0, 0, 0};
                bool have_normal = false;
                for (usize i = 0; i < plan.widened.size(); ++i) {
                    if (life[i].matched && life[i].matched_facing) continue;
                    const forge::PaintRule& rule = plan.widened[i];
                    if (!rule_reaches(plan, i, p, reach)) continue;
                    const f64 value = field.eval(rule.test, p);
                    if (value < rule.low || value > rule.high) continue;
                    if (!life[i].matched) {
                        life[i].matched = true;
                        if (rule.facing_axis >= 3) {
                            life[i].matched_facing = true;
                            --outstanding;
                            continue;
                        }
                    }
                    if (rule.facing_axis < 3) {
                        if (!have_normal) {
                            normal = field.normal_at(plan.root, p, voxel);
                            have_normal = true;
                        }
                        const f64 component = (rule.facing_axis == 0)   ? normal.x
                                              : (rule.facing_axis == 1) ? normal.y
                                                                        : normal.z;
                        const bool passes = (rule.facing_min >= 0.0) ? (component >= rule.facing_min)
                                                                     : (component <= rule.facing_min);
                        if (passes) {
                            life[i].matched_facing = true;
                            --outstanding;
                        }
                    }
                }
            }
        }
    }
    return life;
}

// A rule that matched nothing gets asked again, FINER — the same treatment the cut audit gives a
// zero, and for the same reason.
//
// A rule keyed to a shape THINNER THAN A VOXEL matches nothing at that resolution and everything at
// a finer one, and without asking twice the two are one line. `windows_glass` is a 0.045 m pane,
// 0.36 of a voxel at metre 8: it matches nothing there, and at metre 32 — which is what the contract
// asks for and what the building ships at — `--part part_windows` paints 300,268 voxels of glass.
// Reporting that as a dead rule is a report somebody acts on, and the action is to delete a pane.
//
// `surface.clip`'s `paint granite where=surface_datum` is the other kind: its shape is buried inside
// the podium and the crypt's void has since eaten the podium, so there is no voxel for it at any
// resolution. That one is worth finding.
//
// Order matters for what this costs. The rule's own test is a small subtree and the SOLID is the
// whole building, so the band is asked first and the matter only where the band already matched —
// a genuinely dead rule therefore never evaluates the building at all.
struct RuleProbe {
    i32 factor = 0;
    bool found = false;
    bool outside = false;
    bool gave_up = false;
    Vec3 at{0, 0, 0};
};

// A point budget is not enough on its own, and finding that out cost two runs.
//
// A weathering coat is keyed on an occlusion or a curvature, and those evaluate their child
// fourteen and seven times — of the whole building. A sweep of a building-sized box at four times
// the metre is then not slow, it is unbounded, and the first version of this sat on the facility
// past ten minutes without printing. So the probe carries a DEADLINE, and a rule it ran out of time
// on is reported as not checked rather than as clear.
//
// And a budget on its own gets the interesting case WRONG rather than slow. `windows_glass` is one
// 0.045 m pane, but there is a window on every bay of every elevation, so the rule's box is the
// whole building: 585 million points at metre 32, over any budget worth setting, so the rule the
// owner asked about came back "not re-asked" — honest, and no use.
//
// What rescues it is that the rule's test is usually a DISTANCE. `SamplePlan::rule_slack` already
// says which rules those are (anything under `kInfiniteSlack`), and for one of them the value at a
// point bounds how far away the band can possibly be. So the sweep MARCHES: out in the open air
// twelve metres from the nearest pane, the answer is twelve metres and the row skips almost all of
// itself. The same 585 million points become a few million real evaluations, and a pane thinner
// than a voxel is found in under a second.
//
// Rules keyed on a pattern — a noise says nothing about the next point along — cannot be marched
// and keep the point budget, which is the same distinction `plan_sample` itself is built on.
RuleProbe probe_rule_finer(const forge::Field& field, const forge::SamplePlan& plan, usize i,
                           const forge::SampleSettings& settings, u64 budget, f64 deadline_s) {
    RuleProbe out;
    // The rule's box cut to the box that was actually sampled. Matter exists nowhere else, so a
    // rule whose shape does not meet it cannot paint at any resolution — and that is a different
    // sentence from "it matched nothing", so it gets one.
    Aabb box = (i < plan.rule_box.size()) ? plan.rule_box[i] : Aabb{};
    const f64 lo[3]{std::max(box.low.x, settings.low.x), std::max(box.low.y, settings.low.y),
                    std::max(box.low.z, settings.low.z)};
    const f64 hi[3]{std::min(box.high.x, settings.high.x), std::min(box.high.y, settings.high.y),
                    std::min(box.high.z, settings.high.z)};
    for (u32 axis = 0; axis < 3; ++axis) {
        if (hi[axis] < lo[axis]) { out.outside = true; return out; }
    }
    const f64 span[3]{hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]};

    // Whether the rule's test is a distance, and so whether a row of it can be marched rather than
    // walked. Taken from the plan, not guessed: this is the same question the sampler settles a
    // whole box with, answered by the same number.
    const f64 slack = (i < plan.rule_slack.size()) ? plan.rule_slack[i] : forge::Field::kInfiniteSlack;
    const bool metric = slack < forge::Field::kInfiniteSlack;

    for (const i32 factor : {4, 2}) {
        const f64 step = 1.0 / static_cast<f64>(settings.voxels_per_metre * factor);
        if (!metric) {
            f64 points = 1.0;
            for (u32 axis = 0; axis < 3; ++axis) points *= std::floor(span[axis] / step) + 1.0;
            if (points > static_cast<f64>(budget)) continue;
        }
        out.factor = factor;
        const f64 reach = step * 0.8660254037844386;
        const forge::PaintRule& rule = plan.widened[i];
        const i32 n[3]{static_cast<i32>(span[0] / step) + 1, static_cast<i32>(span[1] / step) + 1,
                       static_cast<i32>(span[2] / step) + 1};
        const auto started = std::chrono::steady_clock::now();
        u64 since_clock = 0;
        for (i32 k = 0; k < n[2]; ++k) {
            for (i32 j = 0; j < n[1]; ++j) {
                for (i32 a = 0; a < n[0]; ++a) {
                    if (++since_clock >= 1024) {
                        since_clock = 0;
                        if (std::chrono::duration<f64>(std::chrono::steady_clock::now() - started)
                                .count() > deadline_s) {
                            out.gave_up = true;
                            return out;
                        }
                    }
                    const Vec3 p{lo[0] + (static_cast<f64>(a) + 0.5) * step,
                                 lo[1] + (static_cast<f64>(j) + 0.5) * step,
                                 lo[2] + (static_cast<f64>(k) + 0.5) * step};
                    if (!rule_reaches(plan, i, p, reach)) continue;
                    const f64 value = field.eval(rule.test, p);
                    if (value < rule.low || value > rule.high) {
                        // How far this row can jump without stepping over the band. A distance
                        // changes by at most one metre per metre travelled, plus whatever
                        // displacement can hide, so anything nearer than that cannot match either.
                        if (metric) {
                            const f64 away = (value > rule.high) ? (value - rule.high)
                                                                 : (rule.low - value);
                            const f64 clear = away - slack;
                            if (clear > step) {
                                // Clamped to the row. A band open at one end makes `away` 1e30,
                                // which is a correct "the rest of this row cannot match" and an
                                // undefined conversion to int if it is taken literally.
                                const f64 cells = std::min(clear / step,
                                                           static_cast<f64>(n[0] - a));
                                a += static_cast<i32>(cells) - 1;
                            }
                        }
                        continue;
                    }
                    if (field.eval(plan.root, p) >= 0.0) continue;   // no matter here to paint
                    out.found = true;
                    out.at = p;
                    return out;
                }
            }
        }
        return out;
    }
    return out;
}

// One base, one frame, and every cut that carves it. The voxel pass runs once per group rather
// than once per cut, because the answer "which cuts claim this point" is one question.
struct Group {
    u32 base = 0;
    Frame frame;
    std::vector<u32> cuts;        // indices into the cut list
    std::vector<Victim> victims;
    Aabb reach;                   // the union of its cuts' boxes
};

std::string frame_key(const Frame& frame) {
    std::string key;
    char buffer[192];
    for (const forge::Node& n : frame) {
        std::snprintf(buffer, sizeof buffer, "%u:%g,%g,%g,%g|", static_cast<u32>(n.op), n.a[0],
                      n.a[1], n.a[2], n.a[3]);
        key += buffer;
    }
    return key;
}

std::string metres_box(const VoxelBox& box, const i64* origin, f64 per) {
    if (!box.any) return "(nothing)";
    char buffer[160];
    std::snprintf(buffer, sizeof buffer, "%.2f %.2f %.2f .. %.2f %.2f %.2f m",
                  static_cast<f64>(origin[0] + box.low[0]) / per,
                  static_cast<f64>(origin[1] + box.low[1]) / per,
                  static_cast<f64>(origin[2] + box.low[2]) / per,
                  static_cast<f64>(origin[0] + box.high[0] + 1) / per,
                  static_cast<f64>(origin[1] + box.high[1] + 1) / per,
                  static_cast<f64>(origin[2] + box.high[2] + 1) / per);
    return buffer;
}

void run_cut_audit(const forge::Script& script, const forge::SampleResult& built, const Clip& raw,
                   JobSystem& jobs, const std::string& focus_name, u32 skeleton_depth) {
    const forge::Field& field = script.field;
    const f64 per = static_cast<f64>(script.settings.voxels_per_metre);
    const f64 litre = 1.0 / (per * per * per);

    // The names, AND the nodes they had before `origin` moved them.
    //
    // `apply_origin` re-binds every entry of `script.parts` to a fresh `translate` wrapping the node
    // the author bound — which is what makes `--part part_dome` sample the dome inside its own box.
    // The SOLID is wrapped the same way, so the graph this walks is the unwrapped one, and looking
    // the names up as they now stand finds none of it: the first run after that fix came back with
    // 252 cuts, every one of them called `box#798` and `union#3674`, and not a single victim named.
    // A tool whose whole output is names had lost all of them and still printed a full table.
    //
    // So both are mapped, and the wrapper is recognised by being a translate BY EXACTLY the origin
    // shift rather than by position, so a clip with no `origin` is unaffected and a part the author
    // themselves wrapped in a translate is not silently unwrapped.
    std::unordered_map<u32, std::string> names;
    for (const auto& entry : script.parts) {
        names.emplace(entry.second, entry.first);
        const forge::Node& n = field.node(entry.second);
        if (n.op == Op::Translate && n.children == 1 && n.a[0] == script.origin_shift[0] &&
            n.a[1] == script.origin_shift[1] && n.a[2] == script.origin_shift[2]) {
            names.emplace(n.child[0], entry.first);
        }
    }

    Walk walk;
    {
        std::vector<u8> seen(field.size(), 0);
        walk_for_cuts(field, script.solid, Frame{}, true, "", 1, seen, walk);
    }
    for (Cut& cut : walk.cuts) {
        const auto found = names.find(cut.node);
        if (found != names.end()) {
            cut.name = found->second;
        } else {
            char buffer[64];
            std::snprintf(buffer, sizeof buffer, "%s#%u", forge::op_name(field.node(cut.node).op),
                          cut.node);
            cut.name = buffer;
        }
    }

    if (walk.cuts.empty()) {
        std::printf("cuts          none — nothing in this expression subtracts anything\n");
        return;
    }

    // --- group the cuts by what they carve --------------------------------------------------
    std::vector<Group> groups;
    {
        std::unordered_map<std::string, u32> by_key;
        for (usize i = 0; i < walk.cuts.size(); ++i) {
            Cut& cut = walk.cuts[i];
            if (!cut.audited) continue;
            const std::string key = std::to_string(cut.base) + "@" + frame_key(cut.frame);
            auto found = by_key.find(key);
            if (found == by_key.end()) {
                Group group;
                group.base = cut.base;
                group.frame = cut.frame;
                found = by_key.emplace(key, static_cast<u32>(groups.size())).first;
                groups.push_back(std::move(group));
            }
            cut.group = found->second;
            groups[cut.group].cuts.push_back(static_cast<u32>(i));
        }
    }
    for (Group& group : groups) {
        std::vector<u8> seen(field.size(), 0);
        collect_skeleton(field, group.base, group.frame, true, names, 0, skeleton_depth, seen,
                         group.victims);
        std::sort(group.victims.begin(), group.victims.end(),
                  [](const Victim& a, const Victim& b) { return a.volume < b.volume; });
        group.reach.low = {1e30, 1e30, 1e30};
        group.reach.high = {-1e30, -1e30, -1e30};
        for (u32 index : group.cuts) {
            const Aabb& box = walk.cuts[index].world_box;
            group.reach.low = {std::min(group.reach.low.x, box.low.x),
                               std::min(group.reach.low.y, box.low.y),
                               std::min(group.reach.low.z, box.low.z)};
            group.reach.high = {std::max(group.reach.high.x, box.high.x),
                                std::max(group.reach.high.y, box.high.y),
                                std::max(group.reach.high.z, box.high.z)};
        }
    }

    // --- which cuts, if any, the caller wants drawn -------------------------------------------
    std::vector<u32> focus;
    if (!focus_name.empty()) {
        for (usize i = 0; i < walk.cuts.size(); ++i) {
            if (walk.cuts[i].name == focus_name) focus.push_back(static_cast<u32>(i));
        }
        if (focus.empty()) {
            std::printf("ERROR  no cut called '%s' — the names are in the --cuts table\n",
                        focus_name.c_str());
        }
        if (focus.size() > 7) focus.resize(7);
    }
    std::vector<u8> mask;
    if (!focus.empty()) mask.assign(raw.voxels.size(), 0);
    std::unordered_map<u32, u32> focus_bit;
    for (usize i = 0; i < focus.size(); ++i) focus_bit[focus[i]] = static_cast<u32>(1u << i);

    // --- the pass ------------------------------------------------------------------------------
    std::vector<Tally> tally(walk.cuts.size());
    for (usize i = 0; i < walk.cuts.size(); ++i) {
        const usize victims = walk.cuts[i].audited ? groups[walk.cuts[i].group].victims.size() : 0;
        tally[i].victim.assign(victims + 1, 0);
        tally[i].victim_only.assign(victims + 1, 0);
        tally[i].victim_box.assign(victims + 1, VoxelBox{});
    }
    std::mutex merge_lock;
    const auto started = std::chrono::steady_clock::now();
    u64 base_evaluations = 0;

    for (usize g = 0; g < groups.size(); ++g) {
        const Group& group = groups[g];
        if (group.cuts.empty() || group.reach.high.x < group.reach.low.x) continue;
        i32 lo[3], hi[3];
        bool any = true;
        const f64 reach_low[3]{group.reach.low.x, group.reach.low.y, group.reach.low.z};
        const f64 reach_high[3]{group.reach.high.x, group.reach.high.y, group.reach.high.z};
        for (u32 axis = 0; axis < 3; ++axis) {
            const f64 a = reach_low[axis] * per - static_cast<f64>(built.origin_voxel[axis]) - 0.5;
            const f64 b = reach_high[axis] * per - static_cast<f64>(built.origin_voxel[axis]) - 0.5;
            lo[axis] = std::max(0, static_cast<i32>(std::ceil(a)));
            hi[axis] = std::min(raw.size[axis] - 1, static_cast<i32>(std::floor(b)));
            if (hi[axis] < lo[axis]) any = false;
        }
        if (!any) continue;

        const usize slabs = static_cast<usize>(hi[2] - lo[2] + 1);
        std::atomic<u64> base_calls{0};
        jobs.parallel_for(slabs, 1, [&](usize begin, usize end) {
            std::vector<Tally> local(walk.cuts.size());
            for (u32 index : group.cuts) {
                local[index].victim.assign(group.victims.size() + 1, 0);
                local[index].victim_only.assign(group.victims.size() + 1, 0);
                local[index].victim_box.assign(group.victims.size() + 1, VoxelBox{});
            }
            std::vector<u32> claimed;
            claimed.reserve(group.cuts.size());
            u64 calls = 0;
            for (usize slab = begin; slab < end; ++slab) {
                const i32 z = lo[2] + static_cast<i32>(slab);
                const f64 pz = (static_cast<f64>(built.origin_voxel[2] + z) + 0.5) / per;
                for (i32 y = lo[1]; y <= hi[1]; ++y) {
                    const f64 py = (static_cast<f64>(built.origin_voxel[1] + y) + 0.5) / per;
                    for (i32 x = lo[0]; x <= hi[0]; ++x) {
                        const f64 px = (static_cast<f64>(built.origin_voxel[0] + x) + 0.5) / per;
                        const Vec3 p{px, py, pz};
                        claimed.clear();
                        for (u32 index : group.cuts) {
                            const Cut& cut = walk.cuts[index];
                            if (!inside_box(cut.world_box, p)) continue;
                            if (field.eval(cut.node, to_local(cut.frame, p)) < 0.0) {
                                claimed.push_back(index);
                            }
                        }
                        if (claimed.empty()) continue;

                        // Was there anything here to take? One evaluation of the uncut matter,
                        // and it is the expensive one, which is why it is asked last.
                        ++calls;
                        if (field.eval(group.base, to_local(group.frame, p)) >= 0.0) {
                            for (u32 index : claimed) ++local[index].on_air;
                            continue;
                        }

                        // Whose was it: the smallest named box that actually contains the point.
                        usize victim = group.victims.size();
                        u32 tried = 0;
                        for (usize v = 0; v < group.victims.size(); ++v) {
                            const Victim& candidate = group.victims[v];
                            if (!inside_box(candidate.world_box, p)) continue;
                            if (++tried > 32) break;
                            if (field.eval(candidate.node, to_local(candidate.frame, p)) < 0.0) {
                                victim = v;
                                break;
                            }
                        }

                        const bool alone = claimed.size() == 1;
                        for (u32 index : claimed) {
                            Tally& t = local[index];
                            ++t.removed;
                            t.removed_box.add(x, y, z);
                            ++t.victim[victim];
                            t.victim_box[victim].add(x, y, z);
                            if (victim == group.victims.size()) ++t.unattributed;
                            if (alone) {
                                ++t.only;
                                t.only_box.add(x, y, z);
                                ++t.victim_only[victim];
                            }
                        }
                        if (!mask.empty()) {
                            u8 bits = alone ? 0 : 0x80;
                            for (u32 index : claimed) {
                                const auto found = focus_bit.find(index);
                                if (found != focus_bit.end()) bits |= static_cast<u8>(found->second);
                            }
                            if (bits & 0x7F) {
                                mask[raw.index(x, y, z)] = bits;
                            }
                        }
                    }
                }
            }
            base_calls.fetch_add(calls, std::memory_order_relaxed);
            std::lock_guard<std::mutex> guard(merge_lock);
            for (u32 index : group.cuts) merge_tally(tally[index], local[index]);
        });
        base_evaluations += base_calls.load();
    }
    // Ask the zeroes again, finer, so "too coarse to see it" and "it really is empty" are two
    // different lines rather than the same one.
    std::vector<u32> zeroes;
    for (usize i = 0; i < walk.cuts.size(); ++i) {
        if (walk.cuts[i].audited && tally[i].removed == 0) zeroes.push_back(static_cast<u32>(i));
    }
    if (!zeroes.empty()) {
        jobs.parallel_for(zeroes.size(), 1, [&](usize begin, usize end) {
            for (usize n = begin; n < end; ++n) {
                const u32 index = zeroes[n];
                const Cut& cut = walk.cuts[index];
                const Probe probe = probe_finer(field, cut, groups[cut.group].base,
                                                groups[cut.group].frame,
                                                script.settings.voxels_per_metre, 8000000);
                tally[index].finer = probe.factor;
                tally[index].finer_found = probe.found;
                tally[index].finer_at = probe.at;
            }
        });
    }
    const f64 seconds = std::chrono::duration<f64>(std::chrono::steady_clock::now() - started).count();

    // --- the report ------------------------------------------------------------------------------
    usize unaudited = 0;
    for (const Cut& cut : walk.cuts) { if (!cut.audited) ++unaudited; }
    std::printf("cuts          %zu subtracted operands, %zu carved bases, %zu restores inside "
                "them\n",
                walk.cuts.size(), groups.size(), walk.restores);
    std::printf("              removed = matter this cut took; only-it = matter NOTHING ELSE "
                "would have\n");

    std::vector<u32> order;
    for (usize i = 0; i < walk.cuts.size(); ++i) {
        if (walk.cuts[i].audited) order.push_back(static_cast<u32>(i));
    }
    std::sort(order.begin(), order.end(), [&](u32 a, u32 b) {
        if (tally[a].removed != tally[b].removed) return tally[a].removed > tally[b].removed;
        return walk.cuts[a].name < walk.cuts[b].name;
    });

    std::printf("  %-24s %-20s %9s %9s  %s\n", "cut", "carves", "removed", "only-it", "out of");
    for (u32 index : order) {
        const Cut& cut = walk.cuts[index];
        const Tally& t = tally[index];
        const auto base_name = names.find(cut.base);
        char carves[32];
        if (base_name != names.end()) {
            std::snprintf(carves, sizeof carves, "%.19s", base_name->second.c_str());
        } else {
            std::snprintf(carves, sizeof carves, "%.13s#%u",
                          forge::op_name(field.node(cut.base).op), cut.base);
        }
        std::string victims;
        if (t.removed > 0) {
            std::vector<usize> best;
            for (usize v = 0; v < t.victim.size(); ++v) { if (t.victim[v] > 0) best.push_back(v); }
            std::sort(best.begin(), best.end(),
                      [&](usize a, usize b) { return t.victim[a] > t.victim[b]; });
            const Group& group = groups[cut.group];
            for (usize n = 0; n < best.size() && n < 4; ++n) {
                char buffer[96];
                const char* who = best[n] < group.victims.size() ? group.victims[best[n]].name.c_str()
                                                                 : "(unnamed)";
                std::snprintf(buffer, sizeof buffer, " %s %.2f", who,
                              static_cast<f64>(t.victim[best[n]]) * litre);
                victims += buffer;
            }
            if (best.size() > 4) victims += " ...";
        }
        const char* flag = "";
        char thin[160];
        thin[0] = '\0';
        if (t.removed == 0 && t.finer_found) {
            std::snprintf(thin, sizeof thin,
                          "  (but it DOES cut at metre %d, near %.2f %.2f %.2f — thinner than a "
                          "voxel here, not a fault)",
                          script.settings.voxels_per_metre * t.finer, t.finer_at.x, t.finer_at.y,
                          t.finer_at.z);
        } else if (t.removed == 0 && t.finer > 0) {
            std::snprintf(thin, sizeof thin, "  (and nothing at metre %d either)",
                          script.settings.voxels_per_metre * t.finer);
        } else if (t.removed == 0 && cut.audited) {
            // Not "it is fine". NOT ASKED — its box is unbounded or too big to sweep finely — and
            // saying so is the difference between a checked zero and an unchecked one.
            std::snprintf(thin, sizeof thin, "  (%s, so NOT re-asked finer)",
                          cut.world_box.infinite() ? "its box is unbounded" : "its box is too big");
        }
        if (t.removed == 0 && t.on_air == 0) flag = "  <-- REMOVED NOTHING, and covers no matter";
        else if (t.removed == 0) flag = "  <-- REMOVED NOTHING, it is all cutting air";
        else if (t.only == 0) flag = "  <-- nothing of its own: every voxel another cut took too";
        std::printf("  %-24s %-20s %9.3f %9.3f  %s%s%s%s\n", cut.name.c_str(), carves,
                    static_cast<f64>(t.removed) * litre, static_cast<f64>(t.only) * litre,
                    victims.c_str(), cut.smooth ? "  (smooth: a lower bound)" : "", flag, thin);
    }
    if (unaudited > 0) {
        std::printf("  not audited  %zu cut%s whose place cannot be mapped back to these "
                    "coordinates:\n", unaudited, unaudited == 1 ? "" : "s");
        usize shown = 0;
        for (const Cut& cut : walk.cuts) {
            if (cut.audited || shown++ >= 8) continue;
            std::printf("               %-24s %s\n", cut.name.c_str(), cut.refused);
        }
    }
    usize widest = 0;
    for (const Group& group : groups) widest = std::max(widest, group.victims.size());
    std::printf("audit         %.2f s, %llu evaluations of the uncut matter, up to %zu names to "
                "charge it to\n", seconds, static_cast<unsigned long long>(base_evaluations),
                widest);
    if (walk.reshared > 0) {
        std::printf("              %zu subtrees are reached by more than one path; the first "
                    "was used\n", walk.reshared);
    }

    // --- and one cut in full ----------------------------------------------------------------------
    for (usize f = 0; f < focus.size(); ++f) {
        const u32 index = focus[f];
        const Cut& cut = walk.cuts[index];
        const Tally& t = tally[index];
        std::printf("\ncut           %s   (operand %u of the %s at node %u)\n", cut.name.c_str(),
                    cut.slot, forge::op_name(field.node(cut.owner).op), cut.owner);
        if (!cut.audited) {
            std::printf("              NOT AUDITED — %s\n", cut.refused);
            continue;
        }
        const Group& group = groups[cut.group];
        std::printf("  carves      %s\n",
                    names.count(group.base) ? names.at(group.base).c_str() : "an unnamed shape");
        std::printf("  its box     %.2f %.2f %.2f .. %.2f %.2f %.2f m\n", cut.world_box.low.x,
                    cut.world_box.low.y, cut.world_box.low.z, cut.world_box.high.x,
                    cut.world_box.high.y, cut.world_box.high.z);
        std::printf("  removed     %.3f m3 (%llu voxels)  %s\n", static_cast<f64>(t.removed) * litre,
                    static_cast<unsigned long long>(t.removed),
                    metres_box(t.removed_box, built.origin_voxel, per).c_str());
        std::printf("  only it     %.3f m3 (%llu voxels)  %s\n", static_cast<f64>(t.only) * litre,
                    static_cast<unsigned long long>(t.only),
                    metres_box(t.only_box, built.origin_voxel, per).c_str());
        std::printf("  cut air     %.3f m3 of its volume covered nothing at all\n",
                    static_cast<f64>(t.on_air) * litre);

        std::vector<usize> best;
        for (usize v = 0; v < t.victim.size(); ++v) { if (t.victim[v] > 0) best.push_back(v); }
        std::sort(best.begin(), best.end(),
                  [&](usize a, usize b) { return t.victim[a] > t.victim[b]; });
        std::printf("  out of      %zu named part%s\n", best.size(), best.size() == 1 ? "" : "s");
        for (usize n = 0; n < best.size() && n < 12; ++n) {
            const usize v = best[n];
            std::printf("    %-22s %8.3f m3 (%6.3f only it)  %s\n",
                        v < group.victims.size() ? group.victims[v].name.c_str() : "(unnamed)",
                        static_cast<f64>(t.victim[v]) * litre,
                        static_cast<f64>(t.victim_only[v]) * litre,
                        metres_box(t.victim_box[v], built.origin_voxel, per).c_str());
        }

        if (!t.removed_box.any || mask.empty()) continue;
        // A section across the axis the cut is thinnest along, because that is the way a person
        // reads an opening — and through the FULLEST plane of it rather than the middle of its box.
        //
        // The middle is the obvious choice and it is wrong on the first real case: `void_windows`
        // spans y -0.75 to 5.00 because it holds two storeys of windows, and the midpoint at y=2.00
        // is the blank band of wall BETWEEN them. The section came out with not one voxel of cut in
        // it, which reads exactly like a cut that did nothing.
        u32 axis = 0;
        i32 thinnest = t.removed_box.high[0] - t.removed_box.low[0];
        for (u32 a = 1; a < 3; ++a) {
            const i32 span = t.removed_box.high[a] - t.removed_box.low[a];
            if (span < thinnest) { thinnest = span; axis = a; }
        }
        const u8 bit = static_cast<u8>(1u << f);
        i32 at = (t.removed_box.low[axis] + t.removed_box.high[axis]) / 2;
        {
            std::vector<u64> per_plane(static_cast<usize>(raw.size[axis]), 0);
            for (i32 z = 0; z < raw.size[2]; ++z) {
                for (i32 y = 0; y < raw.size[1]; ++y) {
                    for (i32 x = 0; x < raw.size[0]; ++x) {
                        if (mask[raw.index(x, y, z)] & bit) {
                            const i32 coord[3]{x, y, z};
                            ++per_plane[static_cast<usize>(coord[axis])];
                        }
                    }
                }
            }
            u64 best = 0;
            for (usize i = 0; i < per_plane.size(); ++i) {
                if (per_plane[i] > best) { best = per_plane[i]; at = static_cast<i32>(i); }
            }
        }
        u32 da = 0, db = 0;
        query_axes(axis, da, db);
        const i32 step = std::max(1, std::max(raw.size[da], raw.size[db]) / 110);
        std::printf("  section     %c = %.3f m, the fullest plane of the cut.  X only this cut,  "
                    "o also another,  # matter that stayed\n", "xyz"[axis],
                    static_cast<f64>(built.origin_voxel[axis] + at) / per);
        std::printf("%s", cut_slice_text(raw, mask, bit, axis, at, step).c_str());
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::string path;
    std::string part;
    i32 metre = 0;
    Axis slice;
    Axis span;
    Axis gap;
    bool quiet = false;
    bool list = false;
    bool cuts = false;
    std::string cut;
    u32 skeleton = 6;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--cuts") {
            cuts = true;
        } else if (arg == "--cut" && i + 1 < argc) {
            cut = argv[++i];
            cuts = true;
        } else if (arg == "--cut-skeleton" && i + 1 < argc) {
            skeleton = static_cast<u32>(std::atoi(argv[++i]));
            cuts = true;
        } else if (arg == "--part" && i + 1 < argc) {
            part = argv[++i];
        } else if (arg == "--metre" && i + 1 < argc) {
            metre = std::atoi(argv[++i]);
        } else if (arg == "--slice" && i + 1 < argc) {
            slice = parse_axis(argv[++i]);
        } else if (arg == "--span" && i + 1 < argc) {
            span = parse_axis(argv[++i]);
        } else if (arg == "--gap" && i + 1 < argc) {
            gap = parse_axis(argv[++i]);
        } else if (arg == "--quiet") {
            quiet = true;
        } else if (arg == "--list") {
            list = true;
        } else if (!arg.empty() && arg[0] != '-') {
            path = arg;
        }
    }
    if (path.empty()) {
        std::printf("usage: clipcheck <file.clip> [--part name] [--metre n] [--list]\n"
                    "                 [--slice y@2.0] [--span y@0,0] [--gap x@1.5,-9] [--quiet]\n"
                    "                 [--cuts] [--cut <name>] [--cut-skeleton n]\n");
        return 2;
    }

    VoxelTypeTable types;
    TagRegistry tags;
    forge::Script script = forge::load_clip_script(path, types, tags);
    for (const forge::ScriptError& error : script.errors) say_error(script, error);
    if (!script.errors.empty()) return 1;

    if (list) {
        std::printf("parts (%zu)\n", script.parts.size());
        for (const auto& entry : script.parts) std::printf("  %s\n", entry.first.c_str());
        if (!script.has_solid) return 1;
    }
    if (!script.has_solid) {
        std::printf("ERROR  no `solid` statement — nothing says what the matter is\n");
        return 1;
    }

    if (metre > 0) script.settings.voxels_per_metre = metre;

    if (!part.empty()) {
        u32 piece = 0;
        if (!script.part(part, piece)) {
            std::printf("ERROR  no part called '%s'\n", part.c_str());
            std::printf("parts ");
            for (const auto& entry : script.parts) std::printf(" %s", entry.first.c_str());
            std::printf("\n");
            return 1;
        }
        script.solid = piece;
    }

    // Count how often each rule was reached. Off by default in the sampler, and rightly — it is an
    // atomic increment beside every paint evaluation — but this tool exists to say WHICH rule did
    // nothing, and without it `rule_evaluations` comes back empty and the whole diagnostic below
    // was silently a no-op. It printed nothing on the facility for as long as it has existed.
    script.settings.count_rule_cost = true;

    JobSystem jobs;
    const forge::SampleResult built =
        forge::sample(script.field, script.solid, script.paint, script.settings, &jobs);

    // The game despeckles before it measures (D610) and so does this, or the numbers are about a
    // clip nobody will ever see.
    Clip clip = built.clip;
    forge::despeckle(clip);

    const forge::Measurement m = forge::measure(clip, script.settings.voxels_per_metre);
    const f64 per = static_cast<f64>(script.settings.voxels_per_metre);

    if (!quiet) std::printf("%s", forge::report(m, &script.material_names).c_str());

    if (!m.extent.any) {
        std::printf("EMPTY  the sample contains no matter at all%s\n",
                    part.empty() ? "" : " — check the part name and its bounds");
        return 1;
    }

    const f64 low[3] = {static_cast<f64>(built.origin_voxel[0] + m.extent.low[0]) / per,
                        static_cast<f64>(built.origin_voxel[1] + m.extent.low[1]) / per,
                        static_cast<f64>(built.origin_voxel[2] + m.extent.low[2]) / per};
    const f64 high[3] = {static_cast<f64>(built.origin_voxel[0] + m.extent.high[0] + 1) / per,
                         static_cast<f64>(built.origin_voxel[1] + m.extent.high[1] + 1) / per,
                         static_cast<f64>(built.origin_voxel[2] + m.extent.high[2] + 1) / per};
    std::printf("worldbox      %.3f %.3f %.3f   %.3f %.3f %.3f  m\n", low[0], low[1], low[2],
                high[0], high[1], high[2]);

    // Is it one thing? The brief's first test of "done", and the one that catches a part that
    // does not touch what it stands on.
    const forge::Connectivity joined = forge::connectivity(clip);
    std::printf("components    %llu (largest %llu voxels, %llu floating)\n",
                static_cast<unsigned long long>(joined.components),
                static_cast<unsigned long long>(joined.largest),
                static_cast<unsigned long long>(joined.floating_voxels));
    if (joined.components > 1) {
        usize shown = 0;
        for (const forge::Island& island : joined.islands) {
            if (island.voxels == joined.largest) continue;
            if (shown++ >= 6) break;
            std::printf("  floating    %llu voxels at %.2f %.2f %.2f m\n",
                        static_cast<unsigned long long>(island.voxels),
                        static_cast<f64>(built.origin_voxel[0] + island.low[0]) / per,
                        static_cast<f64>(built.origin_voxel[1] + island.low[1]) / per,
                        static_cast<f64>(built.origin_voxel[2] + island.low[2]) / per);
        }
    }

    // A paint rule that paints nothing is a coat somebody wrote and nothing wears. It produces no
    // error and is invisible in every other number here — and there are TWO ways to be one.
    //
    // This block used to print `never fired` off `rule_evaluations`, which counts evaluations and
    // not matches (the counter is incremented before the band is tested), so the one word covered
    // one of the two faults and quietly excused the other. Both are named now, and neither is
    // called "fired".
    {
        const std::string only_part =
            part.empty() ? std::string() : std::string(" (one part only — most belong to other "
                                                       "fragments)");
        const usize rules = script.paint.size();
        std::vector<usize> unreached;
        for (usize i = 0; i < built.rule_evaluations.size(); ++i) {
            if (built.rule_evaluations[i] == 0) unreached.push_back(i);
        }
        const auto note_for = [&](usize) -> std::string { return {}; };
        const auto say_with = [&](const char* label, const std::vector<usize>& which,
                                  const char* what, const auto& note) {
            if (which.empty()) return;
            std::printf("%-13s %zu of %zu rules %s%s\n", label, which.size(), rules, what,
                        only_part.c_str());
            for (usize n = 0; n < which.size() && n < 40; ++n) {
                std::printf("              %-44s%s\n",
                            which[n] < script.paint_source.size()
                                ? script.paint_source[which[n]].c_str()
                                : "?",
                            note(which[n]).c_str());
            }
            if (which.size() > 40) std::printf("              ... and %zu more\n", which.size() - 40);
        };
        const auto say = [&](const char* label, const std::vector<usize>& which, const char* what) {
            say_with(label, which, what, note_for);
        };
        say("never asked", unreached, "were never EVALUATED — culled everywhere, cost nothing");

        // And the other half: asked everywhere and matched nowhere. Measured by re-testing the
        // finished clip, because the sampler counts evaluations and not matches; see
        // `rules_that_painted` for exactly how faithful that is and which way it errs.
        const forge::SamplePlan plan = forge::plan_sample(script.field, script.solid, script.paint);
        std::vector<RuleLife> life =
            rules_that_painted(script.field, plan, built.clip, built.origin_voxel,
                               script.settings.voxels_per_metre);
        std::vector<usize> unmatched;
        std::vector<usize> facing_only;
        for (usize i = 0; i < life.size(); ++i) {
            if (built.rule_evaluations.size() > i && built.rule_evaluations[i] == 0) continue;
            if (!life[i].matched) unmatched.push_back(i);
            else if (!life[i].matched_facing) facing_only.push_back(i);
        }

        // And the same second question the cut audit asks of a zero: is this a dead rule or a
        // resolution? Asked in parallel over the rules, because a rule that really is dead pays a
        // full sweep of its own box and those are the ones worth waiting for.
        jobs.parallel_for(unmatched.size(), 1, [&](usize begin, usize end) {
            for (usize n = begin; n < end; ++n) {
                const usize i = unmatched[n];
                const RuleProbe probe =
                    probe_rule_finer(script.field, plan, i, script.settings, 8000000, 6.0);
                life[i].finer = probe.factor;
                life[i].finer_found = probe.found;
                life[i].outside = probe.outside;
                life[i].gave_up = probe.gave_up;
                life[i].finer_at = probe.at;
            }
        });

        // Split, because they are two different findings and only one of them is a fault. Saying
        // them in one list is what sent a `windows_glass` that paints 300,268 voxels at the
        // resolution the building ships at to the owner as a dead rule.
        std::vector<usize> dead;
        std::vector<usize> too_coarse;
        std::vector<usize> unchecked;
        for (const usize i : unmatched) {
            if (life[i].finer_found) too_coarse.push_back(i);
            else if (life[i].gave_up || (life[i].finer == 0 && !life[i].outside)) unchecked.push_back(i);
            else dead.push_back(i);
        }
        // How often the sampler actually asked it. A rule asked four million times that matched
        // nothing is a finding whatever the resolution, and it is the number that separates a rule
        // the probe could not afford to sweep from one that was never in play.
        const auto asked = [&](usize i) -> std::string {
            if (i >= built.rule_evaluations.size()) return {};
            char buffer[64];
            std::snprintf(buffer, sizeof buffer, "asked %llu times",
                          static_cast<unsigned long long>(built.rule_evaluations[i]));
            return buffer;
        };
        char what[224];
        std::snprintf(what, sizeof what,
                      "matched NO VOXEL, and none when re-asked at up to metre %d either — these "
                      "paint nothing at any resolution asked",
                      script.settings.voxels_per_metre * 4);
        say_with("never matched", dead, what, [&](usize i) -> std::string {
            return life[i].outside ? "its shape does not reach the sampled box at all" : asked(i);
        });
        std::snprintf(what, sizeof what,
                      "match nothing at metre %d but DO at a finer one — real geometry thinner than "
                      "a voxel, NOT a fault", script.settings.voxels_per_metre);
        say_with("too coarse", too_coarse, what, [&](usize i) -> std::string {
            char buffer[96];
            std::snprintf(buffer, sizeof buffer, "paints at metre %d, near %.2f %.2f %.2f",
                          script.settings.voxels_per_metre * life[i].finer, life[i].finer_at.x,
                          life[i].finer_at.y, life[i].finer_at.z);
            return buffer;
        });
        say_with("not re-asked", unchecked,
                 "matched nothing, and the finer sweep of their boxes ran out of budget or time — "
                 "NOT checked, rather than cleared",
                 asked);
        say("facing never", facing_only,
            "matched their band but never their facing=; the sampler may still paint them where a "
            "whole box settles");
    }

    if (span.given) {
        u32 pa = 0, pb = 0;
        query_axes(span.axis, pa, pb);
        const i32 a = static_cast<i32>(span.a * per) - built.origin_voxel[pa];
        const i32 b = static_cast<i32>(span.b * per) - built.origin_voxel[pb];
        const forge::Span s = forge::span_along(clip, span.axis, a, b);
        std::printf("span          %.3f m of matter along %c (%s)\n",
                    static_cast<f64>(s.length()) / per, "xyz"[span.axis],
                    s.contiguous ? "unbroken" : "BROKEN — there is a gap in it");
    }
    if (gap.given) {
        u32 pa = 0, pb = 0;
        query_axes(gap.axis, pa, pb);
        const i32 a = static_cast<i32>(gap.a * per) - built.origin_voxel[pa];
        const i32 b = static_cast<i32>(gap.b * per) - built.origin_voxel[pb];
        const forge::Span s = forge::gap_along(clip, gap.axis, a, b);
        std::printf("gap           %.3f m of air along %c (%s)\n",
                    static_cast<f64>(s.length()) / per, "xyz"[gap.axis],
                    s.contiguous ? "clear" : "BROKEN — there is matter in it");
        const Clear clear = longest_clear(clip, gap.axis, a, b);
        std::printf("clear         %.3f m, the longest unbroken run, %s%s\n",
                    clear.metres / per,
                    clear.closed_below && clear.closed_above
                        ? "closed at both ends"
                        : (clear.closed_below ? "OPEN at the far end" : "OPEN at the near end"),
                    (clear.closed_below && clear.closed_above && gap.axis == 1 &&
                     clear.metres / per < 2.10)
                        ? "  -- UNDER THE 2.10 m HEAD HEIGHT"
                        : "");
    }
    // What every `difference` in this expression actually removed, and whose it was. Last of the
    // numbers, because it is the one that takes a while and the one somebody asks for by name.
    if (cuts) run_cut_audit(script, built, built.clip, jobs, cut, skeleton);

    if (slice.given) {
        const i32 at = static_cast<i32>(slice.a * per) - built.origin_voxel[slice.axis];
        // The step comes from the axes the picture is DRAWN on, not from the one held fixed.
        //
        // Taking it from `slice.axis` was the second bug of the same family: on a 57 m orangery,
        // `--slice x@0` is a section 9 m by 11 m -- about 290 by 360 voxels, which fits -- and it
        // came out downsampled seventeen times at every resolution, because the step was computed
        // from the 1901 voxels of LENGTH the slice had just discarded. The section was unreadable
        // and the only way round it was to slice the building the other way.
        u32 da = 0, db = 0;
        query_axes(slice.axis, da, db);
        const i32 size = std::max(m.size[da], m.size[db]);
        const i32 step = std::max(1, size / 110);
        std::printf("%s", forge::slice_text(clip, slice.axis, at, step).c_str());
    }

    return 0;
}
