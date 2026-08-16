// paintcheck — how far the raw view's colour is from the colour the sampler actually decided.
//
// # Why this exists
//
// `documentation/24-clip-viewer.md` §1 states the rule the whole site rests on, and it is D204's:
// **two things deriving one world from one description is the failure mode.** The viewer is
// trustworthy because it is not a second reading of the clip language — the baker calls
// `forge::load_clip_script`, `forge::sample` and `forge::despeckle`, the same three calls the game
// makes, and what it draws is what fell out of them.
//
// Painting the ◉ raw view breaks that rule on purpose. The raw view marches the shapes analytically
// and has no voxels, so it cannot read a material out of a sample; it has to EVALUATE THE PAINT
// STACK ITSELF, at the hit point, in GLSL. That is a second implementation of the paint stack, in a
// second language, driven by a second copy of the field graph. It will disagree with the first.
// This says by how much, where, and why — and it is the only instrument that can, because the
// disagreement it is looking for produces a wrong colour and never an error.
//
// # The trap this is written around
//
// CLAUDE.md: *every audit agreeing is not evidence when they all read the same source.* Three
// checks once reported "agrees, perfectly" with 304 visible faults on screen, because all three
// were downstream of the one reader that was wrong. A comparison that reads the exported `PANT` and
// `FLDG` chunks on both sides has exactly that shape: if the export is wrong, both arms are wrong
// together and the tool prints a clean bill.
//
// So the reference arm is **`forge::sample` itself** — the real sampler, the same call `bake_root`
// makes, with the same part handling, the same origin shift and the same despeckle — read back as
// one material per voxel. Nothing about it comes through the export. The other arms are named
// below with where each one comes from, and the header of every report repeats it.
//
// # The arms, and what each one adds
//
// The disagreement is not one thing. It is a stack of five independent differences, and a single
// percentage over the lot of them tells nobody which to fix. So the arms are a LADDER: each adds
// one difference to the one above it, and the step between two rows is that difference's cost.
//
//   sampler        forge::sample, despeckled. The reference. Per voxel, decided by the descent —
//                  box settling, widened bands, the normal taken at the voxel step.
//   walk           the paint stack walked directly at the same voxel centre: rules in order, last
//                  match wins, `Field::eval` in double, the normal by central differences at the
//                  SHADER's step of 2 mm. This is the raw view's own model of painting, in C++,
//                  and the gap to `sampler` is what the descent's machinery is worth.
//   widened        the same walk against `plan_sample`'s widened bands rather than the authored
//                  ones. The sampler tests widened bands — displacement can move a surface, so a
//                  rule's accepted range is grown by the amplitude — and a GLSL port written from
//                  the clip file will not know that. The gap is what the widening is worth.
//   float          the same walk through `Field::mirror_eval_single`: the stack-machine evaluator,
//                  with every point and every answer narrowed to `float` at each node boundary,
//                  which is what a shader carrying `vec3` and `float` between nodes does. Each
//                  node's own arithmetic is still double, so this is a LOWER BOUND on what a real
//                  GLSL evaluator carries. The gap is what single precision is worth, and it is
//                  the "lands on the wrong side of above=0.55 in the fourth decimal" case exactly.
//   surface        the float walk moved to where the RAY actually lands — the voxel centre
//                  projected onto the isosurface of the solid — instead of the voxel centre. The
//                  gap is what "the sampler asks at a lattice point and the marcher asks at the
//                  surface" is worth, and it is the floor: no port can remove it.
//
// The last row is the honest prediction for a correct GLSL port. Everything above it is a bug that
// can still be fixed in the port; the last step is not a bug at all.
//
// # And the export, checked without evaluating it
//
// `--wsc web/data/<id>.wsc` reads the `PANT` and `FLDG` chunks out of a baked file with this
// tool's own reader and checks them against `script.paint` and `script.field` — rule for rule, node
// for node, parameter for parameter. It does not need to know the baker's op numbering: the
// numbering is DERIVED from the data, by seeing which `Op` each exported code lands on, and a code
// that lands on two different ops is the export being wrong and says so. That check has an
// independent source on each side (the forge on one, the file's bytes on the other) and is the one
// that catches an export that drops a rule or shifts a band.
//
// # Cost, which is the other half of the question
//
// The last section counts what one `material_at` costs: how many rules, how many field nodes a
// full stack walk touches, and how many of those are the noise family — the ops that dominate,
// because a `Noise` or an `Fbm` is tens of flops and a hash where a `Translate` is three adds.
// `web/js/features/paintcost.js` turns those counts into a frame-time prediction against a curve
// measured on the card, and this is where its input comes from.
//
// Build it with tools/paintcheck.sh, the way tools/clipcheck.sh builds clipcheck. It is not in the
// CMake build for the same reason clipcheck is not: nothing here needs Vulkan, SDL or Windows.
//
// Usage:
//   paintcheck <file.clip> [--part part_name] [--metre 8] [--max-points 200000]
//              [--wsc web/data/facility_portico.wsc] [--csv out.csv] [--quiet]

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "core/jobs.hpp"
#include "forge/clip_script.hpp"
#include "forge/measure.hpp"
#include "forge/sample.hpp"
#include "world/voxel_type.hpp"
#include "world/tags.hpp"

using namespace ws;
using forge::Field;
using forge::PaintRule;
using forge::Vec3;
// `Op` is deliberately NOT pulled in: `ws::Op` in world/op.hpp is a different type with the same
// name, and the two are ambiguous the moment both headers are included.
using FieldOp = forge::Op;

namespace {

// The shader marches with `h = 0.002` for its gradient (SHAPE_FRAGMENT in web/js/gl.js). The
// sampler takes its facing normal at the voxel step instead. Those are different questions asked of
// the same surface and they answer differently on anything with a grain on it, so the step is a
// property of the arm rather than a constant.
constexpr f64 kShaderNormalStep = 0.002;

constexpr u32 kNoRule = 0xFFFFFFFFu;

void say_error(const forge::Script& script, const forge::ScriptError& error) {
    if (error.line > 0 && error.line <= script.sources.size()) {
        const forge::SourceLine& where = script.sources[error.line - 1];
        std::printf("ERROR  %s:%u: %s\n", where.file.c_str(), where.line, error.message.c_str());
    } else {
        std::printf("ERROR  %s\n", error.message.c_str());
    }
}

// ------------------------------------------------------------------------------------------
// Walking the paint stack the way the raw view has to
// ------------------------------------------------------------------------------------------

// One evaluator, so an arm is a set of flags rather than a copy of the walk.
//
// `single` picks `mirror_eval_single` — the shader-shaped stack walk in float — over `eval`. It
// returns false when it meets an op it does not mirror, and "I could not" must never read the same
// as "the answer is nought" (trap 7), so the failure is carried out rather than swallowed.
struct Evaluator {
    const Field* field = nullptr;
    bool single = false;
    mutable u64 failures = 0;

    f64 at(u32 node, Vec3 p) const {
        if (!single) return field->eval(node, p);
        f64 out = 0.0;
        if (!field->mirror_eval_single(node, p, out)) {
            ++failures;
            return field->eval(node, p);
        }
        return out;
    }

    Vec3 normal(u32 root, Vec3 p, f64 step) const {
        const f64 dx = at(root, {p.x + step, p.y, p.z}) - at(root, {p.x - step, p.y, p.z});
        const f64 dy = at(root, {p.x, p.y + step, p.z}) - at(root, {p.x, p.y - step, p.z});
        const f64 dz = at(root, {p.x, p.y, p.z + step}) - at(root, {p.x, p.y, p.z + -step});
        return forge::normalise({dx, dy, dz});
    }
};

struct Verdict {
    VoxelTypeId type = kAir;
    u32 rule = kNoRule;
};

// The stack, exactly as `paint_solid` in src/forge/sample.cpp applies it at a voxel it has decided
// holds matter: every rule in order, the value tested against the band, the facing tested against
// the normal, LAST MATCH WINS, and a point no rule claimed takes the first rule's material rather
// than air.
//
// `values`, when given, is filled with each rule's evaluated value so the caller can ask afterwards
// how near a rule came to firing. That is the whole diagnosis of a fourth-decimal disagreement and
// it costs nothing here, because the values have already been computed.
Verdict walk_stack(const std::vector<PaintRule>& paint, const Evaluator& eval, u32 root, Vec3 p,
                   f64 normal_step, std::vector<f64>* values) {
    Verdict out;
    Vec3 normal{0, 0, 0};
    bool have_normal = false;
    for (usize i = 0; i < paint.size(); ++i) {
        const PaintRule& rule = paint[i];
        const f64 value = eval.at(rule.test, p);
        if (values != nullptr) (*values)[i] = value;
        if (value < rule.low || value > rule.high) continue;
        if (rule.facing_axis < 3) {
            if (!have_normal) {
                normal = eval.normal(root, p, normal_step);
                have_normal = true;
            }
            const f64 component = (rule.facing_axis == 0)   ? normal.x
                                  : (rule.facing_axis == 1) ? normal.y
                                                            : normal.z;
            if (rule.facing_min >= 0.0) {
                if (component < rule.facing_min) continue;
            } else {
                if (component > rule.facing_min) continue;
            }
        }
        out.type = rule.type;
        out.rule = static_cast<u32>(i);
    }
    if (out.type == kAir && !paint.empty()) {
        out.type = paint.front().type;
        out.rule = 0;
    }
    return out;
}

// How far a value is from the band it had to be inside. Zero when it was inside.
f64 miss_by(const PaintRule& rule, f64 value) {
    if (value < rule.low) return rule.low - value;
    if (value > rule.high) return value - rule.high;
    return 0.0;
}

// ------------------------------------------------------------------------------------------
// The points
// ------------------------------------------------------------------------------------------

// A surface voxel and the face that made it one.
//
// The raw view only ever colours a SURFACE — a ray that hits nothing draws nothing — so an interior
// voxel is not a point either arm has an opinion anybody can see. Comparing over the whole solid
// would dilute every figure by however hollow the clip happens to be.
struct Point {
    Vec3 centre;
    VoxelTypeId reference = kAir;
    // `forge::despeckle` runs after the sample and BEFORE the mesh, in the baker and in the game
    // alike, and it changes a voxel's material to whatever most of its solid neighbours wear. No
    // walk of the paint stack can reproduce that: it is not a function of the point. So a voxel it
    // touched is a voxel the raw view cannot agree with by construction, and it is counted
    // separately rather than left inside a percentage that reads as a port bug.
    bool despeckled = false;
};

// ------------------------------------------------------------------------------------------
// Reading a baked file's chunk directory
// ------------------------------------------------------------------------------------------

// The shared format contract: 208-byte header, `u32 chunkOffset` at 200, `u32 chunkCount` at 204,
// then 16-byte entries of `char fourcc[4]; u32 offset; u32 size; u32 reserved`.
//
// Written out here rather than shared with `web/js/format.js` or `tools/bake_web.cpp` ON PURPOSE.
// A reader that shares its parser with the writer cannot catch the writer being wrong; that is the
// same fault as three audits reading one source, one layer down.
struct Chunk {
    char fourcc[5] = {0, 0, 0, 0, 0};
    u32 offset = 0;
    u32 size = 0;
};

u32 read_u32(const std::vector<u8>& bytes, usize at) {
    if (at + 4 > bytes.size()) return 0;
    return static_cast<u32>(bytes[at]) | (static_cast<u32>(bytes[at + 1]) << 8) |
           (static_cast<u32>(bytes[at + 2]) << 16) | (static_cast<u32>(bytes[at + 3]) << 24);
}
i32 read_i32(const std::vector<u8>& bytes, usize at) {
    const u32 raw = read_u32(bytes, at);
    i32 out = 0;
    std::memcpy(&out, &raw, sizeof(out));
    return out;
}
f32 read_f32(const std::vector<u8>& bytes, usize at) {
    const u32 raw = read_u32(bytes, at);
    f32 out = 0.0f;
    std::memcpy(&out, &raw, sizeof(out));
    return out;
}

bool read_file(const std::string& path, std::vector<u8>& out) {
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) return false;
    std::fseek(file, 0, SEEK_END);
    const long size = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);
    if (size <= 0) {
        std::fclose(file);
        return false;
    }
    out.resize(static_cast<usize>(size));
    const usize got = std::fread(out.data(), 1, out.size(), file);
    std::fclose(file);
    return got == out.size();
}

// What the export check found. Reported as a list of complaints rather than a boolean, because
// "the export disagrees" is not actionable and "rule 37's high band is 0.55 in the file and 0.6 in
// the clip" is.
struct ExportReport {
    bool present = false;
    u32 version = 0;
    u32 rules_file = 0;
    u32 nodes_file = 0;
    std::vector<std::string> complaints;
    std::map<u32, FieldOp> op_map;          // exported code -> the Op every node carrying it turned out to be
    std::vector<std::string> op_clash;  // a code that landed on two different ops
};

void note(ExportReport& report, const std::string& what) {
    if (report.complaints.size() < 40) report.complaints.push_back(what);
}

void check_export(const std::string& path, const forge::Script& script,
                  const std::vector<PaintRule>& paint, ExportReport& report) {
    std::vector<u8> bytes;
    if (!read_file(path, bytes)) {
        note(report, "cannot read " + path);
        return;
    }
    if (bytes.size() < 208 || bytes[0] != 'W' || bytes[1] != 'S' || bytes[2] != 'C' ||
        bytes[3] != 'V') {
        note(report, path + " is not a .wsc");
        return;
    }
    report.version = read_u32(bytes, 4);
    const u32 directory_at = read_u32(bytes, 200);
    const u32 directory_count = read_u32(bytes, 204);
    if (directory_at == 0 || directory_count == 0) {
        note(report, "no chunk directory — this file predates FORMAT_VERSION 3");
        return;
    }
    std::vector<Chunk> chunks;
    for (u32 i = 0; i < directory_count; ++i) {
        const usize at = static_cast<usize>(directory_at) + static_cast<usize>(i) * 16;
        if (at + 16 > bytes.size()) {
            note(report, "chunk directory runs off the end of the file");
            return;
        }
        Chunk chunk;
        for (i32 c = 0; c < 4; ++c) chunk.fourcc[c] = static_cast<char>(bytes[at + static_cast<usize>(c)]);
        chunk.offset = read_u32(bytes, at + 4);
        chunk.size = read_u32(bytes, at + 8);
        if (static_cast<u64>(chunk.offset) + chunk.size > bytes.size()) {
            note(report, std::string("chunk ") + chunk.fourcc + " runs off the end of the file");
            continue;
        }
        chunks.push_back(chunk);
    }
    const auto find = [&chunks](const char* name) -> const Chunk* {
        for (const Chunk& chunk : chunks) {
            if (std::strncmp(chunk.fourcc, name, 4) == 0) return &chunk;
        }
        return nullptr;
    };

    // ---- PANT: one entry a rule, in the clip's own order --------------------------------------
    const Chunk* pant = find("PANT");
    if (pant == nullptr) {
        note(report, "no PANT chunk");
    } else {
        report.present = true;
        const usize base = pant->offset;
        const u32 count = read_u32(bytes, base);
        report.rules_file = count;
        if (count != paint.size()) {
            note(report, "PANT holds " + std::to_string(count) + " rules, the clip has " +
                             std::to_string(paint.size()));
        }
        const u32 shared = std::min<u32>(count, static_cast<u32>(paint.size()));
        for (u32 i = 0; i < shared; ++i) {
            const usize at = base + 4 + static_cast<usize>(i) * 28;
            if (at + 28 > bytes.size()) {
                note(report, "PANT runs off the end at rule " + std::to_string(i));
                break;
            }
            const u32 node = read_u32(bytes, at + 0);
            const f32 below = read_f32(bytes, at + 4);
            const f32 above = read_f32(bytes, at + 8);
            const i32 facing_axis = read_i32(bytes, at + 12);
            const f32 facing_at = read_f32(bytes, at + 16);
            const u32 material = read_u32(bytes, at + 20);
            const PaintRule& rule = paint[i];
            const auto same = [](f64 a, f64 b) {
                if (a <= -1e29 && b <= -1e29) return true;
                if (a >= 1e29 && b >= 1e29) return true;
                return std::fabs(a - b) <= 1e-4 * std::max(1.0, std::fabs(a));
            };
            if (node != rule.test) {
                note(report, "rule " + std::to_string(i) + ": PANT node " + std::to_string(node) +
                                 ", clip node " + std::to_string(rule.test));
            }
            // `below` is the low edge and `above` the high one — the names the interface uses, which
            // are the clip file's words ("above=0.55" is a low edge) and not the struct's.
            if (!same(below, rule.low) || !same(above, rule.high)) {
                note(report, "rule " + std::to_string(i) + ": PANT band [" + std::to_string(below) +
                                 ", " + std::to_string(above) + "], clip band [" +
                                 std::to_string(rule.low) + ", " + std::to_string(rule.high) + "]");
            }
            if (static_cast<u32>(facing_axis < 0 ? 3 : facing_axis) != rule.facing_axis) {
                note(report, "rule " + std::to_string(i) + ": PANT facing axis " +
                                 std::to_string(facing_axis) + ", clip " +
                                 std::to_string(rule.facing_axis));
            }
            if (rule.facing_axis < 3 && !same(facing_at, rule.facing_min)) {
                note(report, "rule " + std::to_string(i) + ": PANT facing at " +
                                 std::to_string(facing_at) + ", clip " +
                                 std::to_string(rule.facing_min));
            }
            if (material != static_cast<u32>(rule.type)) {
                note(report, "rule " + std::to_string(i) + ": PANT material " +
                                 std::to_string(material) + ", clip type " +
                                 std::to_string(static_cast<u32>(rule.type)));
            }
        }
    }

    // ---- FLDG: the field graph, node for node --------------------------------------------------
    //
    // The op codes are the BAKER's numbering and not the enum's — §4a of the viewer document says
    // why, and the shapes view already learned it the hard way. So nothing here assumes a mapping:
    // it is built from the data. Every exported code is recorded against the `Op` of the node it
    // sits at, and a code that lands on two different ops is an export that has lost track of its
    // own numbering. That is a check with an independent source on each side.
    const Chunk* fldg = find("FLDG");
    if (fldg == nullptr) {
        note(report, "no FLDG chunk");
        return;
    }
    report.present = true;
    const usize base = fldg->offset;
    const u32 count = read_u32(bytes, base);
    report.nodes_file = count;
    if (count != script.field.size()) {
        note(report, "FLDG holds " + std::to_string(count) + " nodes, the field has " +
                         std::to_string(script.field.size()));
    }
    const u32 shared = std::min<u32>(count, static_cast<u32>(script.field.size()));
    u32 wrong_children = 0;
    u32 wrong_parameters = 0;
    u32 wrong_box = 0;
    for (u32 i = 0; i < shared; ++i) {
        const usize at = base + 4 + static_cast<usize>(i) * 76;
        if (at + 76 > bytes.size()) {
            note(report, "FLDG runs off the end at node " + std::to_string(i));
            break;
        }
        const u32 op = read_u32(bytes, at + 0);
        const u32 children = read_u32(bytes, at + 4);
        const forge::Node& node = script.field.node(i);
        const auto found = report.op_map.find(op);
        if (found == report.op_map.end()) {
            report.op_map[op] = node.op;
        } else if (found->second != node.op) {
            if (report.op_clash.size() < 8) {
                report.op_clash.push_back("code " + std::to_string(op) + " is both " +
                                          forge::op_name(found->second) + " and " +
                                          forge::op_name(node.op));
            }
        }
        if (children != node.children) ++wrong_children;
        for (u32 c = 0; c < 4 && c < node.children; ++c) {
            if (read_u32(bytes, at + 8 + static_cast<usize>(c) * 4) != node.child[c]) {
                ++wrong_children;
                break;
            }
        }
        for (u32 k = 0; k < 8; ++k) {
            const f32 got = read_f32(bytes, at + 24 + static_cast<usize>(k) * 4);
            const f64 want = node.a[k];
            if (std::fabs(static_cast<f64>(got) - want) > 1e-4 * std::max(1.0, std::fabs(want))) {
                ++wrong_parameters;
                break;
            }
        }
        const Field::Aabb box = script.field.bounds_of(i);
        const f32 lo[3] = {read_f32(bytes, at + 56), read_f32(bytes, at + 60),
                           read_f32(bytes, at + 64)};
        const f32 hi[3] = {read_f32(bytes, at + 68), read_f32(bytes, at + 72), 0.0f};
        // Only the two that fit the 76-byte record are checked; the third high component follows in
        // whatever the real layout turns out to be. A box that is TIGHTER than the field's is the
        // dangerous direction — it culls a rule that should have fired — so that is what is counted.
        (void)hi;
        if (!box.infinite() &&
            (static_cast<f64>(lo[0]) > box.low.x + 1e-3 || static_cast<f64>(lo[1]) > box.low.y + 1e-3 ||
             static_cast<f64>(lo[2]) > box.low.z + 1e-3)) {
            ++wrong_box;
        }
    }
    if (wrong_children > 0) {
        note(report, std::to_string(wrong_children) + " FLDG nodes disagree about their children");
    }
    if (wrong_parameters > 0) {
        note(report, std::to_string(wrong_parameters) + " FLDG nodes disagree about a[]");
    }
    if (wrong_box > 0) {
        note(report, std::to_string(wrong_box) + " FLDG nodes claim a box tighter than the field's");
    }
}

// ------------------------------------------------------------------------------------------
// What one material_at costs
// ------------------------------------------------------------------------------------------

struct Cost {
    u64 nodes = 0;       // node evaluations one walk of the whole stack performs, per point
    u64 noise = 0;       // of those, the noise family, counting an fbm's octaves
    u64 evaluations = 0; // and how many of those a shader would call a texture-free hash
};

bool is_noise(FieldOp op) {
    return op == FieldOp::Noise || op == FieldOp::Fbm || op == FieldOp::Ridged || op == FieldOp::Rasp ||
           op == FieldOp::Cells || op == FieldOp::CellEdge;
}

// `eval` is a recursive descent with no memoisation, so a node reached by two parents is evaluated
// twice and the count has to have the same shape. It is bounded by a visit cap because a repeat
// under a mirror under a repeat can multiply out further than anybody wants counted.
void walk_cost(const Field& field, u32 node, Cost& cost, u32 depth, u64 cap) {
    if (cost.nodes >= cap || depth > 64) return;
    const forge::Node& n = field.node(node);
    ++cost.nodes;
    if (is_noise(n.op)) {
        // An fbm walks its octaves; a[1] is how many. The rest of the family is one lattice hash.
        const u64 octaves = (n.op == FieldOp::Fbm || n.op == FieldOp::Ridged || n.op == FieldOp::Rasp)
                                ? static_cast<u64>(std::max(1.0, n.a[1]))
                                : 1;
        cost.noise += octaves;
    }
    // The three that ask their child at more than one point, from `mirror_eval`'s own note.
    u32 repeats = 1;
    if (n.op == FieldOp::Curvature) repeats = 7;
    else if (n.op == FieldOp::Occlusion) repeats = 14;
    else if (n.op == FieldOp::Facing) repeats = 6;
    for (u32 c = 0; c < n.children && c < 4; ++c) {
        for (u32 r = 0; r < repeats; ++r) walk_cost(field, n.child[c], cost, depth + 1, cap);
    }
}

// ------------------------------------------------------------------------------------------
// Arms
// ------------------------------------------------------------------------------------------

struct Arm {
    const char* name = "";
    const char* source = "";
    bool single = false;      // float, through the shader-shaped stack walk
    bool widened = false;     // the sampler's grown bands rather than the authored ones
    bool surface = false;     // at the marched hit point rather than the voxel centre
    f64 normal_step = kShaderNormalStep;
};

struct Tally {
    u64 points = 0;
    u64 disagree = 0;
    u64 unlanded = 0;               // surface arms only: points the projection would not converge
    // How far the projection actually moved a point, in voxels. Reported because the whole surface
    // arm rests on it: half a voxel is the honest offset between where the sampler asks and where a
    // ray lands, and anything much over one voxel would be this tool's Newton step overshooting
    // rather than a property of the clip. A figure nobody can sanity-check is a figure.
    f64 moved_total = 0.0;
    f64 moved_worst = 0.0;
    std::map<u64, u64> confusion;   // (reference << 32 | arm) -> count
    std::map<u32, u64> blamed;      // the arm's winning rule at a disagreement
    u64 disagree_despeckled = 0;    // of those, at a voxel forge::despeckle had already changed

    // WHY a point disagreed, and the two answers are completely different faults.
    //
    // `overpaint` is a rule of the reference material that DID match here and was then painted over
    // by a later one that the sampler did not let fire. `near_miss` is no rule of the reference
    // material matching at all. Lumping them together — which the first version of this did, by
    // asking only "how far was the nearest candidate from its band" and getting nought for a rule
    // that fired and lost — reported 86% of `sampler.clip`'s disagreements as fourth-decimal
    // rounding when they are nothing of the kind.
    //
    // Each is bucketed by a MARGIN. For an overpaint it is how far inside its band the winning rule
    // was, so a tiny number means the winner only just fired; for a near miss it is how far outside
    // the best candidate was. Under 1e-4 in either is the "wrong side of above=0.55 in the fourth
    // decimal" case; over 1e-2 is a difference of substance.
    u64 overpaint[4] = {0, 0, 0, 0};
    u64 near_miss[4] = {0, 0, 0, 0};
    u64 no_candidate = 0;
};

void merge(Tally& into, const Tally& from) {
    into.points += from.points;
    into.disagree += from.disagree;
    into.unlanded += from.unlanded;
    into.no_candidate += from.no_candidate;
    into.disagree_despeckled += from.disagree_despeckled;
    into.moved_total += from.moved_total;
    into.moved_worst = std::max(into.moved_worst, from.moved_worst);
    for (i32 i = 0; i < 4; ++i) into.near_miss[i] += from.near_miss[i];
    for (i32 i = 0; i < 4; ++i) into.overpaint[i] += from.overpaint[i];
    for (const auto& entry : from.confusion) into.confusion[entry.first] += entry.second;
    for (const auto& entry : from.blamed) into.blamed[entry.first] += entry.second;
}

i32 bucket(f64 margin) {
    if (margin < 1e-4) return 0;
    if (margin < 1e-3) return 1;
    if (margin < 1e-2) return 2;
    return 3;
}

// `material_names` is indexed BY VOXEL TYPE ID, with [0] for air — it is NOT parallel to
// `material_types`, which is the declaration order. They are two vectors of nearly the same length
// full of the same words and they line up for exactly none of their entries: on `sampler.clip`,
// names[1] is "stone" and types[1] is 2, the id of "pale". `forge::report` indexes by type and so
// does this; walking `material_types` to find a name instead printed the wrong material against
// every count, which reads as a paint bug rather than a lookup one.
std::string type_name(const forge::Script& script, VoxelTypeId type) {
    if (type == kAir) return "air";
    if (static_cast<usize>(type) < script.material_names.size() &&
        !script.material_names[type].empty()) {
        return script.material_names[type];
    }
    return "type " + std::to_string(static_cast<u32>(type));
}

std::string rule_source(const forge::Script& script, u32 rule) {
    if (rule == kNoRule) return "(none)";
    const std::string prefix = "#" + std::to_string(rule) + " ";
    if (rule < script.paint_source.size()) return prefix + script.paint_source[rule];
    return prefix;
}

}  // namespace

int main(int argc, char** argv) {
    std::string path;
    std::string part;
    std::string wsc;
    std::string csv;
    std::string detail;
    i32 metre = 0;
    u64 max_points = 200000;
    bool quiet = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--part" && i + 1 < argc) part = argv[++i];
        else if (arg == "--metre" && i + 1 < argc) metre = std::atoi(argv[++i]);
        else if (arg == "--wsc" && i + 1 < argc) wsc = argv[++i];
        else if (arg == "--csv" && i + 1 < argc) csv = argv[++i];
        else if (arg == "--detail" && i + 1 < argc) detail = argv[++i];
        else if (arg == "--max-points" && i + 1 < argc) max_points = std::strtoull(argv[++i], nullptr, 10);
        else if (arg == "--quiet") quiet = true;
        else if (!arg.empty() && arg[0] != '-') path = arg;
    }
    if (path.empty()) {
        std::printf("usage: paintcheck <file.clip> [--part name] [--metre n] [--max-points n]\n"
                    "                  [--wsc file.wsc] [--csv out.csv] [--detail arm] [--quiet]\n");
        return 2;
    }

    VoxelTypeTable types;
    TagRegistry tags;
    forge::Script script = forge::load_clip_script(path, types, tags);
    for (const forge::ScriptError& error : script.errors) say_error(script, error);
    if (!script.errors.empty()) return 1;
    if (!script.has_solid) {
        std::printf("ERROR  no `solid` statement\n");
        return 1;
    }

    // ---- the root and the settings, exactly as `bake_root` in tools/bake_web.cpp builds them ----
    //
    // Not approximately. A comparison against the site's own voxels has to be against the site's own
    // voxels: the origin shift, the intersection with the building's solid, the rebuilt bounds, the
    // box narrowed to the part and the bounds mask turned off. §3 of the viewer document is the
    // record of what each of those was worth when it was missing.
    forge::SampleSettings settings = script.settings;
    u32 root = script.solid;
    const bool is_part = !part.empty();
    if (is_part) {
        u32 piece = 0;
        if (!script.part(part, piece)) {
            std::printf("ERROR  no part called '%s'\n", part.c_str());
            return 1;
        }
        root = piece;
        const f64* shift = script.origin_shift;
        if (shift[0] != 0.0 || shift[1] != 0.0 || shift[2] != 0.0) {
            root = script.field.translate(root, Vec3{shift[0], shift[1], shift[2]});
        }
        root = script.field.intersect({root, script.solid});
        script.field.build_bounds();
        const Field::Aabb box = script.field.bounds_of(root);
        if (!box.infinite()) {
            const f64 margin = 0.25;
            settings.low = {std::max(settings.low.x, box.low.x - margin),
                            std::max(settings.low.y, box.low.y - margin),
                            std::max(settings.low.z, box.low.z - margin)};
            settings.high = {std::min(settings.high.x, box.high.x + margin),
                             std::min(settings.high.y, box.high.y + margin),
                             std::min(settings.high.z, box.high.z + margin)};
        }
        settings.has_bounds = false;
    }
    if (metre > 0) settings.voxels_per_metre = metre;
    settings.count_rule_cost = false;

    const f64 span[3] = {settings.high.x - settings.low.x, settings.high.y - settings.low.y,
                         settings.high.z - settings.low.z};
    if (span[0] <= 0.0 || span[1] <= 0.0 || span[2] <= 0.0) {
        std::printf("ERROR  empty bounds\n");
        return 1;
    }

    JobSystem jobs;

    // ---- ARM ONE: forge::sample. The reference, and the only arm with an independent source. ----
    const forge::SampleResult built =
        forge::sample(script.field, root, script.paint, settings, &jobs);
    Clip clip = built.clip;
    const Clip raw_clip = built.clip;   // before despeckle, so a voxel it moved can be told apart
    forge::despeckle(clip);
    if (clip.empty()) {
        std::printf("ERROR  sampled to nothing\n");
        return 1;
    }

    const forge::SamplePlan plan = forge::plan_sample(script.field, root, script.paint);
    const f64 voxel = 1.0 / static_cast<f64>(settings.voxels_per_metre);
    const f64 centre_shift = settings.sample_at_centre ? 0.5 : 0.0;

    // ---- the points: every voxel of the surface, thinned to a cap ------------------------------
    std::vector<Point> points;
    {
        u64 surface_total = 0;
        std::vector<Point> all;
        for (i32 z = 0; z < clip.size[2]; ++z) {
            for (i32 y = 0; y < clip.size[1]; ++y) {
                for (i32 x = 0; x < clip.size[0]; ++x) {
                    if (clip.at(x, y, z) == kAir) continue;
                    bool exposed = false;
                    const i32 step[6][3] = {{1, 0, 0},  {-1, 0, 0}, {0, 1, 0},
                                            {0, -1, 0}, {0, 0, 1},  {0, 0, -1}};
                    for (const auto& d : step) {
                        const i32 nx = x + d[0], ny = y + d[1], nz = z + d[2];
                        if (nx < 0 || ny < 0 || nz < 0 || nx >= clip.size[0] || ny >= clip.size[1] ||
                            nz >= clip.size[2]) {
                            exposed = true;
                            break;
                        }
                        if (clip.at(nx, ny, nz) == kAir) {
                            exposed = true;
                            break;
                        }
                    }
                    if (!exposed) continue;
                    ++surface_total;
                    Point point;
                    point.centre = {(static_cast<f64>(built.origin_voxel[0] + x) + centre_shift) * voxel,
                                    (static_cast<f64>(built.origin_voxel[1] + y) + centre_shift) * voxel,
                                    (static_cast<f64>(built.origin_voxel[2] + z) + centre_shift) * voxel};
                    point.reference = clip.at(x, y, z);
                    point.despeckled = raw_clip.at(x, y, z) != point.reference;
                    all.push_back(point);
                }
            }
        }
        // Thinned by a fixed stride and not by a random draw: the same clip must give the same
        // figure twice, or the tool cannot say whether a change moved anything (CLAUDE.md, and the
        // rule that one window is not a measurement).
        const u64 stride = (max_points > 0 && all.size() > max_points)
                               ? (all.size() + max_points - 1) / max_points
                               : 1;
        for (usize i = 0; i < all.size(); i += stride) points.push_back(all[i]);
        if (!quiet) {
            std::printf("clip          %s%s%s\n", path.c_str(), is_part ? "  part " : "",
                        part.c_str());
            std::printf("sampled       %d x %d x %d at %d/m, %llu surface voxels, %llu compared\n",
                        clip.size[0], clip.size[1], clip.size[2], settings.voxels_per_metre,
                        static_cast<unsigned long long>(surface_total),
                        static_cast<unsigned long long>(points.size()));
        }
    }
    if (points.empty()) {
        std::printf("ERROR  no surface voxels\n");
        return 1;
    }

    // ---- what the shader will have to carry ------------------------------------------------
    {
        Cost total;
        u64 worst = 0;
        usize worst_rule = 0;
        std::vector<Cost> per_rule(script.paint.size());
        for (usize i = 0; i < script.paint.size(); ++i) {
            walk_cost(script.field, script.paint[i].test, per_rule[i], 0, 200000);
            total.nodes += per_rule[i].nodes;
            total.noise += per_rule[i].noise;
            if (per_rule[i].nodes > worst) {
                worst = per_rule[i].nodes;
                worst_rule = i;
            }
        }
        Cost normal_cost;
        walk_cost(script.field, root, normal_cost, 0, 200000);
        u32 facing_rules = 0;
        for (const PaintRule& rule : script.paint) {
            if (rule.facing_axis < 3) ++facing_rules;
        }
        const u64 per_pixel = total.nodes + (facing_rules > 0 ? normal_cost.nodes * 6 : 0);
        if (!quiet) {
            std::printf("stack         %zu rules; one walk touches %llu field nodes and %llu "
                        "noise octaves\n",
                        script.paint.size(), static_cast<unsigned long long>(total.nodes),
                        static_cast<unsigned long long>(total.noise));
            std::printf("              worst rule %llu nodes: %s\n",
                        static_cast<unsigned long long>(worst),
                        rule_source(script, static_cast<u32>(worst_rule)).c_str());
            std::printf("              %u rules ask for a normal = 6 more walks of the solid "
                        "(%llu nodes each)\n",
                        facing_rules, static_cast<unsigned long long>(normal_cost.nodes));
            // This is the number web/js/features/paintcost.js takes as its input, and the one the
            // phone question is decided on. Printed as a machine-readable line as well, so the
            // benchmark can be driven from the tool rather than from a number typed in twice.
            std::printf("              per pixel, worst case: %llu node evaluations, %llu of them "
                        "noise\n",
                        static_cast<unsigned long long>(per_pixel),
                        static_cast<unsigned long long>(total.noise));
            std::printf("COST %s|%s|%zu|%llu|%llu\n", path.c_str(), part.c_str(),
                        script.paint.size(), static_cast<unsigned long long>(per_pixel),
                        static_cast<unsigned long long>(total.noise));
        }
    }

    // ---- the ladder ------------------------------------------------------------------------
    const Arm arms[] = {
        {"walk", "Field::eval, double, voxel centre", false, false, false, kShaderNormalStep},
        {"walk+voxelN", "Field::eval, double, normal at the voxel step", false, false, false, voxel},
        {"widened", "plan_sample's grown bands", false, true, false, kShaderNormalStep},
        {"float", "mirror_eval_single, f32 at every node boundary", true, false, false,
         kShaderNormalStep},
        {"surface(f64)", "double, at the marched hit point", false, false, true, kShaderNormalStep},
        {"surface", "float, at the marched hit point", true, false, true, kShaderNormalStep},
    };
    constexpr usize kArms = sizeof(arms) / sizeof(arms[0]);

    std::vector<Tally> tallies(kArms);
    u64 mirror_failures = 0;
    {
        std::vector<Tally> per_thread(kArms * 16);
        std::atomic<u64> failures{0};
        std::atomic<u32> next_slot{0};
        jobs.parallel_for(points.size(), 1024, [&](usize begin, usize end) {
            const u32 slot = next_slot.fetch_add(1) % 16;
            Evaluator plain{&script.field, false, 0};
            Evaluator narrow{&script.field, true, 0};
            std::vector<f64> values(script.paint.size());
            for (usize i = begin; i < end; ++i) {
                const Point& point = points[i];
                for (usize a = 0; a < kArms; ++a) {
                    const Arm& arm = arms[a];
                    Tally& tally = per_thread[a * 16 + slot];
                    const Evaluator& eval = arm.single ? narrow : plain;
                    const std::vector<PaintRule>& rules = arm.widened ? plan.widened : script.paint;
                    Vec3 where = point.centre;
                    if (arm.surface) {
                        // Onto the isosurface, the way a march lands: three Newton steps along the
                        // gradient. A point that will not converge is COUNTED and left out rather
                        // than compared at wherever it drifted to.
                        bool landed = false;
                        for (i32 step = 0; step < 4; ++step) {
                            const f64 d = eval.at(root, where);
                            if (std::fabs(d) < voxel * 0.05) {
                                landed = true;
                                break;
                            }
                            if (std::fabs(d) > voxel * 2.0) break;
                            const Vec3 n = eval.normal(root, where, kShaderNormalStep);
                            where = where - n * d;
                        }
                        if (!landed) {
                            ++tally.unlanded;
                            continue;
                        }
                        const Vec3 delta = where - point.centre;
                        const f64 moved = forge::length(delta) / voxel;
                        tally.moved_total += moved;
                        tally.moved_worst = std::max(tally.moved_worst, moved);
                    }
                    ++tally.points;
                    const Verdict got = walk_stack(rules, eval, root, where, arm.normal_step, &values);
                    if (got.type == point.reference) continue;
                    ++tally.disagree;
                    if (point.despeckled) ++tally.disagree_despeckled;
                    tally.confusion[(static_cast<u64>(point.reference) << 32) |
                                    static_cast<u64>(got.type)] += 1;
                    tally.blamed[got.rule] += 1;
                    // Which of the two faults it was. A rule of the reference material that DID
                    // match and then lost is an ordering difference — some later rule fired here
                    // that the sampler did not let fire — and the margin worth reporting is the
                    // WINNER's, because that is what only just happened. A reference material with
                    // no rule matching at all is a near miss, and the margin is the nearest
                    // candidate's.
                    bool matched = false;
                    f64 best = 1e30;
                    for (usize r = 0; r < rules.size(); ++r) {
                        if (rules[r].type != point.reference) continue;
                        const f64 miss = miss_by(rules[r], values[r]);
                        if (miss <= 0.0) matched = true;
                        best = std::min(best, miss);
                    }
                    if (matched) {
                        // How far INSIDE its band the winner was; a hair means it only just fired.
                        f64 inside = 1e30;
                        if (got.rule < rules.size()) {
                            const PaintRule& winner = rules[got.rule];
                            const f64 value = values[got.rule];
                            if (winner.low > -1e29) inside = std::min(inside, value - winner.low);
                            if (winner.high < 1e29) inside = std::min(inside, winner.high - value);
                            if (inside > 1e29) inside = 1e30;   // an unbounded rule always fires
                        }
                        ++tally.overpaint[bucket(inside)];
                    } else if (best > 1e29) {
                        ++tally.no_candidate;
                    } else {
                        ++tally.near_miss[bucket(best)];
                    }
                }
            }
            failures.fetch_add(narrow.failures);
        });
        mirror_failures = failures.load();
        for (usize a = 0; a < kArms; ++a) {
            for (u32 s = 0; s < 16; ++s) merge(tallies[a], per_thread[a * 16 + s]);
        }
    }

    // ---- the report ------------------------------------------------------------------------
    std::printf("\nAGREEMENT   reference arm: forge::sample + forge::despeckle (the baker's own "
                "call)\n");
    if (mirror_failures > 0) {
        FieldOp missing = FieldOp::Constant;
        const bool covered = script.field.mirror_covers(root, &missing);
        std::printf("            the float arm fell back to double %llu times%s%s\n",
                    static_cast<unsigned long long>(mirror_failures),
                    covered ? "" : " — mirror_eval does not know ",
                    covered ? "" : forge::op_name(missing));
    }
    std::printf("            %-13s %-46s %8s %10s %9s\n", "arm", "second source", "points", "wrong",
                "disagree");
    for (usize a = 0; a < kArms; ++a) {
        const Tally& tally = tallies[a];
        const f64 share = tally.points > 0
                              ? 100.0 * static_cast<f64>(tally.disagree) / static_cast<f64>(tally.points)
                              : 0.0;
        std::printf("            %-13s %-46s %8llu %10llu %8.3f%%   %llu of them despeckled\n",
                    arms[a].name, arms[a].source,
                    static_cast<unsigned long long>(tally.points),
                    static_cast<unsigned long long>(tally.disagree), share,
                    static_cast<unsigned long long>(tally.disagree_despeckled));
        if (arms[a].surface) {
            std::printf("            %-13s moved %.2f voxels on average, %.2f at worst; %llu would "
                        "not land and were left out\n",
                        "", tally.points > 0 ? tally.moved_total / static_cast<f64>(tally.points) : 0.0,
                        tally.moved_worst, static_cast<unsigned long long>(tally.unlanded));
        }
    }

    // The headline arm is the last one: it is the whole ladder, and it is the closest thing this
    // tool has to "what a correct GLSL port will actually draw". `--detail` picks another, which is
    // how a step of the ladder gets taken apart once the ladder says which step is the expensive
    // one.
    usize headline = kArms - 1;
    if (!detail.empty()) {
        for (usize a = 0; a < kArms; ++a) {
            if (detail == arms[a].name) headline = a;
        }
    }
    {
        const Tally& tally = tallies[headline];
        std::printf("\nWHERE       %s, by material\n", arms[headline].name);
        std::map<VoxelTypeId, std::pair<u64, u64>> by_material;   // reference -> (points, disagreements)
        for (const Point& point : points) by_material[point.reference].first += 1;
        for (const auto& entry : tally.confusion) {
            by_material[static_cast<VoxelTypeId>(entry.first >> 32)].second += entry.second;
        }
        std::vector<std::pair<VoxelTypeId, std::pair<u64, u64>>> sorted(by_material.begin(),
                                                                       by_material.end());
        std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) {
            const f64 sa = a.second.first > 0 ? static_cast<f64>(a.second.second) /
                                                    static_cast<f64>(a.second.first)
                                              : 0.0;
            const f64 sb = b.second.first > 0 ? static_cast<f64>(b.second.second) /
                                                    static_cast<f64>(b.second.first)
                                              : 0.0;
            return sa > sb;
        });
        std::printf("            %-24s %10s %10s %9s\n", "material", "surface", "wrong", "share");
        for (usize i = 0; i < sorted.size() && i < 20; ++i) {
            const f64 share = sorted[i].second.first > 0
                                  ? 100.0 * static_cast<f64>(sorted[i].second.second) /
                                        static_cast<f64>(sorted[i].second.first)
                                  : 0.0;
            std::printf("            %-24s %10llu %10llu %8.3f%%\n",
                        type_name(script, sorted[i].first).c_str(),
                        static_cast<unsigned long long>(sorted[i].second.first),
                        static_cast<unsigned long long>(sorted[i].second.second), share);
        }

        std::vector<std::pair<u64, u64>> pairs;
        for (const auto& entry : tally.confusion) pairs.push_back({entry.second, entry.first});
        std::sort(pairs.rbegin(), pairs.rend());
        std::printf("\n            the worst confusions\n");
        for (usize i = 0; i < pairs.size() && i < 10; ++i) {
            const VoxelTypeId want = static_cast<VoxelTypeId>(pairs[i].second >> 32);
            const VoxelTypeId got = static_cast<VoxelTypeId>(pairs[i].second & 0xFFFFFFFFull);
            std::printf("            %8llu  sampler says %-18s raw says %s\n",
                        static_cast<unsigned long long>(pairs[i].first),
                        type_name(script, want).c_str(), type_name(script, got).c_str());
        }

        std::vector<std::pair<u64, u32>> blame;
        for (const auto& entry : tally.blamed) blame.push_back({entry.second, entry.first});
        std::sort(blame.rbegin(), blame.rend());
        std::printf("\nWHY         the rule the raw walk let win, at a disagreement\n");
        for (usize i = 0; i < blame.size() && i < 8; ++i) {
            std::printf("            %8llu  %s\n", static_cast<unsigned long long>(blame[i].first),
                        rule_source(script, blame[i].second).c_str());
        }
        const u64 total = std::max<u64>(1, tally.disagree);
        const char* bands[4] = {"by <1e-4", "by <1e-3", "by <1e-2", "by more"};
        u64 overpaint_total = 0, near_total = 0;
        for (i32 i = 0; i < 4; ++i) {
            overpaint_total += tally.overpaint[i];
            near_total += tally.near_miss[i];
        }
        std::printf("\n            OVERPAINTED  %llu (%.2f%%): a rule of the sampler's material did "
                    "fire and a later one\n            painted over it. The margin is how far "
                    "INSIDE its band the winner was —\n            a hair means it only just "
                    "fired.\n",
                    static_cast<unsigned long long>(overpaint_total),
                    100.0 * static_cast<f64>(overpaint_total) / static_cast<f64>(total));
        for (i32 i = 0; i < 4; ++i) {
            std::printf("              inside %-12s %8llu  %6.2f%%\n", bands[i],
                        static_cast<unsigned long long>(tally.overpaint[i]),
                        100.0 * static_cast<f64>(tally.overpaint[i]) / static_cast<f64>(total));
        }
        std::printf("\n            NEAR MISS    %llu (%.2f%%): no rule of the sampler's material "
                    "matched at all. The\n            margin is how far outside its band the "
                    "nearest one was.\n",
                    static_cast<unsigned long long>(near_total),
                    100.0 * static_cast<f64>(near_total) / static_cast<f64>(total));
        for (i32 i = 0; i < 4; ++i) {
            std::printf("              missed %-12s %8llu  %6.2f%%\n", bands[i],
                        static_cast<unsigned long long>(tally.near_miss[i]),
                        100.0 * static_cast<f64>(tally.near_miss[i]) / static_cast<f64>(total));
        }
        std::printf("              %-19s %8llu  %6.2f%%   no rule of that material exists\n",
                    "no candidate", static_cast<unsigned long long>(tally.no_candidate),
                    100.0 * static_cast<f64>(tally.no_candidate) / static_cast<f64>(total));
    }

    // ---- the export, when there is one -----------------------------------------------------
    if (!wsc.empty()) {
        ExportReport report;
        check_export(wsc, script, script.paint, report);
        std::printf("\nEXPORT      %s\n", wsc.c_str());
        std::printf("            version %u, PANT %u rules, FLDG %u nodes (the clip has %zu and "
                    "%zu)\n",
                    report.version, report.rules_file, report.nodes_file, script.paint.size(),
                    script.field.size());
        if (!report.op_map.empty()) {
            std::printf("            %zu op codes in use, derived from the data\n",
                        report.op_map.size());
        }
        for (const std::string& clash : report.op_clash) {
            std::printf("            CLASH  %s\n", clash.c_str());
        }
        if (report.complaints.empty()) {
            std::printf("            agrees with the clip, rule for rule and node for node\n");
        }
        for (const std::string& complaint : report.complaints) {
            std::printf("            %s\n", complaint.c_str());
        }
    }

    if (!csv.empty()) {
        std::FILE* file = std::fopen(csv.c_str(), "wb");
        if (file != nullptr) {
            std::fprintf(file, "clip,part,metre,arm,points,disagree,share\n");
            for (usize a = 0; a < kArms; ++a) {
                const Tally& tally = tallies[a];
                const f64 share =
                    tally.points > 0
                        ? static_cast<f64>(tally.disagree) / static_cast<f64>(tally.points)
                        : 0.0;
                std::fprintf(file, "%s,%s,%d,%s,%llu,%llu,%.6f\n", path.c_str(), part.c_str(),
                             settings.voxels_per_metre, arms[a].name,
                             static_cast<unsigned long long>(tally.points),
                             static_cast<unsigned long long>(tally.disagree), share);
            }
            std::fclose(file);
        }
    }
    return 0;
}
