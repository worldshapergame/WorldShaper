// R12: the op numbers on the card are the op numbers on the CPU.
//
// # Why a test, when it is a list of sixty-seven constants
//
// Because the list is a SECOND COPY of `enum class ws::forge::Op`, it lives in a file no debugger
// can stop inside, and nothing about getting it wrong looks like getting it wrong. Insert one
// enumerator in the middle of the C++ enum — which is an ordinary thing to do, the enum's own header
// says "the order is not meaningful" — and every op after it shifts by one on the card and by
// nothing on the host. The building then comes out made of the wrong shapes, silently, with no
// compile error and no assertion, and the first symptom is a photograph.
//
// D204's rule is one constant in one place. Where a constant genuinely cannot be in one place —
// and a GLSL `#define` cannot be a C++ enumerator — the rule this project falls back to is the one
// `test_sun_confidence.cpp` uses: read the other copy out of its own file and hold the two
// together. That is what this does.
//
// The RECORD LAYOUTS are the other thing the two sides have to agree about, and they fail the same
// silent way: a `vec3` in a std430 buffer aligns to sixteen, so a record written with one has a
// hole in the middle that the C++ struct does not, and every field after the hole is read from the
// wrong offset — a field full of plausible garbage rather than an error. Those are asserted where
// a mistake stops the build instead, as `static_assert` on each struct in `src/gpu/field_gpu.hpp`,
// because this suite links no Vulkan and cannot see them. That is deliberate; the reason is in
// CMakeLists beside WS_TEST_SOURCES_HEADLESS.

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

#include "forge/field.hpp"
#include "forge/sample.hpp"

using namespace ws;

namespace {

// Every `#define WS_NAME <n>u` in the shader header whose name starts with `prefix`.
std::map<std::string, u32> shader_defines(const char* prefix) {
    std::map<std::string, u32> out;
    const std::string path = std::string(WS_SHADER_SOURCE_DIR) + "/field_types.glsl";
    std::ifstream file(path);
    REQUIRE_MESSAGE(file.good(), "cannot open " << path);
    std::string line;
    while (std::getline(file, line)) {
        std::istringstream words(line);
        std::string hash;
        std::string name;
        std::string value;
        if (!(words >> hash >> name >> value)) continue;
        if (hash != "#define") continue;
        if (name.rfind(prefix, 0) != 0) continue;
        if (!value.empty() && value.back() == 'u') value.pop_back();
        try {
            // `0x...` as well as decimal, because the accelerator's word is written in hex where an
            // op number is written in decimal, and a base-10 read of `0x2000` is nought.
            out[name] = static_cast<u32>(std::stoul(value, nullptr, 0));
        } catch (...) {
            // A define whose value is not a number is not one of these.
        }
    }
    return out;
}

// Every `#define WS_OP_NAME <n>u` in the shader header, by name.
std::map<std::string, u32> shader_op_numbers() { return shader_defines("WS_OP_"); }

// Every `inline constexpr u32 kName = <n>;` in `src/gpu/field_gpu.hpp`, by name.
//
// Read out of the file rather than included, because this suite links no Vulkan and that header
// reaches `gpu/device.hpp` — which is deliberate and is the reason in CMakeLists beside
// WS_TEST_SOURCES_HEADLESS. Reading the other copy out of its own file is the same fallback the op
// numbers above use and `test_sun_confidence.cpp` before them.
std::map<std::string, u32> host_constants() {
    std::map<std::string, u32> out;
    const std::string path = std::string(WS_SHADER_SOURCE_DIR) + "/../src/gpu/field_gpu.hpp";
    std::ifstream file(path);
    REQUIRE_MESSAGE(file.good(), "cannot open " << path);
    std::string line;
    while (std::getline(file, line)) {
        std::istringstream words(line);
        std::string a, b, c, name, equals, value;
        if (!(words >> a >> b >> c >> name >> equals >> value)) continue;
        if (a != "inline" || b != "constexpr" || c != "u32" || equals != "=") continue;
        while (!value.empty() && (value.back() == ';' || value.back() == 'u')) value.pop_back();
        try {
            out[name] = static_cast<u32>(std::stoul(value, nullptr, 0));
        } catch (...) {
        }
    }
    return out;
}

// The five-comparator sorting network `field_walk.glsl` uses to put a union's children in
// ascending box distance, written here in C++ so the claim it rests on can be checked.
//
// The claim is that it produces the SAME sequence as `Field::eval`'s stable insertion sort. A
// sorting network is not stable in general; this one is made total by comparing (distance, slot)
// lexicographically, and the slots are distinct, so there is exactly one sorted arrangement and
// every correct sort finds it. Ties are not an edge case here — every child that carries no box
// has a distance of exactly nought — so if this were wrong it would be wrong constantly and
// quietly, in the magnitudes rather than the signs (D644's four hundred voxels of moss).
std::array<u32, 4> network_order(std::array<f64, 4> key) {
    std::array<u32, 4> slot{0, 1, 2, 3};
    const auto cx = [&](u32 a, u32 b) {
        if (key[a] > key[b] || (key[a] == key[b] && slot[a] > slot[b])) {
            std::swap(key[a], key[b]);
            std::swap(slot[a], slot[b]);
        }
    };
    cx(0, 1);
    cx(2, 3);
    cx(0, 2);
    cx(1, 3);
    cx(1, 2);
    return slot;
}

// `Field::eval`'s own insertion sort over the same keys, transcribed from field.cpp.
std::array<u32, 4> eval_order(const std::array<f64, 4>& away, u32 children) {
    std::array<u32, 4> order{0, 1, 2, 3};
    for (u32 i = 1; i < children; ++i) {
        const u32 keyed = order[i];
        const f64 key_away = away[keyed];
        u32 j = i;
        while (j > 0 && away[order[j - 1]] > key_away) {
            order[j] = order[j - 1];
            --j;
        }
        order[j] = keyed;
    }
    return order;
}

}  // namespace

TEST_CASE("the card's op numbers are the CPU's") {
    const std::map<std::string, u32> shader = shader_op_numbers();
    REQUIRE_MESSAGE(!shader.empty(), "field_types.glsl defined no WS_OP_ constants at all");

    // Written out rather than generated, deliberately. A generator that derived this list from the
    // enum would agree with the enum by construction and prove nothing; the point is that a human
    // wrote the shader's list and a human wrote this one, and the enum has to match both.
    const std::pair<const char*, forge::Op> pairs[] = {
        {"WS_OP_CONSTANT", forge::Op::Constant},
        {"WS_OP_PARAMETER", forge::Op::Parameter},
        {"WS_OP_COORDINATE", forge::Op::Coordinate},
        {"WS_OP_RADIUS", forge::Op::Radius},
        {"WS_OP_SPHERE", forge::Op::Sphere},
        {"WS_OP_BOX", forge::Op::Box},
        {"WS_OP_CYLINDER", forge::Op::Cylinder},
        {"WS_OP_CAPSULE", forge::Op::Capsule},
        {"WS_OP_TORUS", forge::Op::Torus},
        {"WS_OP_ARC", forge::Op::Arc},
        {"WS_OP_CONE", forge::Op::Cone},
        {"WS_OP_PLANE", forge::Op::Plane},
        {"WS_OP_ELLIPSOID", forge::Op::Ellipsoid},
        {"WS_OP_PRISM", forge::Op::Prism},
        {"WS_OP_PLATONIC", forge::Op::Platonic},
        {"WS_OP_WEDGE", forge::Op::Wedge},
        {"WS_OP_STAIRS", forge::Op::Stairs},
        {"WS_OP_REVOLVE", forge::Op::Revolve},
        {"WS_OP_SPIRAL", forge::Op::Spiral},
        {"WS_OP_UNION", forge::Op::Union},
        {"WS_OP_INTERSECTION", forge::Op::Intersection},
        {"WS_OP_DIFFERENCE", forge::Op::Difference},
        {"WS_OP_SMOOTH_UNION", forge::Op::SmoothUnion},
        {"WS_OP_SMOOTH_DIFFERENCE", forge::Op::SmoothDifference},
        {"WS_OP_SMOOTH_INTERSECTION", forge::Op::SmoothIntersection},
        {"WS_OP_CHAMFER_UNION", forge::Op::ChamferUnion},
        {"WS_OP_CHAMFER_DIFFERENCE", forge::Op::ChamferDifference},
        {"WS_OP_CHAMFER_INTERSECTION", forge::Op::ChamferIntersection},
        {"WS_OP_TRANSLATE", forge::Op::Translate},
        {"WS_OP_ROTATE", forge::Op::Rotate},
        {"WS_OP_SCALE", forge::Op::Scale},
        {"WS_OP_MIRROR", forge::Op::Mirror},
        {"WS_OP_REPEAT", forge::Op::Repeat},
        {"WS_OP_POLAR_REPEAT", forge::Op::PolarRepeat},
        {"WS_OP_SCATTER", forge::Op::Scatter},
        {"WS_OP_SHELL", forge::Op::Shell},
        {"WS_OP_ROUND", forge::Op::Round},
        {"WS_OP_OFFSET", forge::Op::Offset},
        {"WS_OP_DISPLACE", forge::Op::Displace},
        {"WS_OP_TWIST", forge::Op::Twist},
        {"WS_OP_BEND", forge::Op::Bend},
        {"WS_OP_SINE", forge::Op::Sine},
        {"WS_OP_WAVES", forge::Op::Waves},
        {"WS_OP_NOISE", forge::Op::Noise},
        {"WS_OP_FBM", forge::Op::Fbm},
        {"WS_OP_RIDGED", forge::Op::Ridged},
        {"WS_OP_RASP", forge::Op::Rasp},
        {"WS_OP_CELLS", forge::Op::Cells},
        {"WS_OP_CELL_EDGE", forge::Op::CellEdge},
        {"WS_OP_CURVATURE", forge::Op::Curvature},
        {"WS_OP_OCCLUSION", forge::Op::Occlusion},
        {"WS_OP_FACING", forge::Op::Facing},
        {"WS_OP_CHECKER", forge::Op::Checker},
        {"WS_OP_STRIPES", forge::Op::Stripes},
        {"WS_OP_BRICKS", forge::Op::Bricks},
        {"WS_OP_ADD", forge::Op::Add},
        {"WS_OP_MULTIPLY", forge::Op::Multiply},
        {"WS_OP_MIN", forge::Op::Min},
        {"WS_OP_MAX", forge::Op::Max},
        {"WS_OP_BLEND", forge::Op::Blend},
        {"WS_OP_REMAP", forge::Op::Remap},
        {"WS_OP_ABS", forge::Op::Abs},
        {"WS_OP_NEGATE", forge::Op::Negate},
        {"WS_OP_STEP", forge::Op::Step},
        {"WS_OP_SMOOTHSTEP", forge::Op::Smoothstep},
        {"WS_OP_CLAMP", forge::Op::Clamp},
        {"WS_OP_POWER", forge::Op::Power},
    };

    for (const auto& [name, op] : pairs) {
        const auto found = shader.find(name);
        REQUIRE_MESSAGE(found != shader.end(), "field_types.glsl has no " << name);
        CHECK_MESSAGE(found->second == static_cast<u32>(op),
                      name << " is " << found->second << " on the card and "
                           << static_cast<u32>(op) << " on the CPU");
    }

    // And the count, which is what catches an op ADDED to the enum and not to the shader — the one
    // direction the loop above cannot see, because it only walks names the test already knows.
    const auto count = shader.find("WS_OP_COUNT");
    REQUIRE_MESSAGE(count != shader.end(), "field_types.glsl has no WS_OP_COUNT");
    CHECK_MESSAGE(count->second == static_cast<u32>(std::size(pairs)),
                  "WS_OP_COUNT is " << count->second << " and this test knows "
                                    << std::size(pairs) << " ops");
}

TEST_CASE("the accelerator's word means the same thing on the card and on the host") {
    // R12's cull packs what the host knows about a node's boxes into the high bits of `children`,
    // and the layout is written twice: `#define WS_NODE_*` in shaders/field_types.glsl and
    // `inline constexpr u32 kNode*` in src/gpu/field_gpu.hpp. Getting one of them wrong is not a
    // compile error and does not look like anything: the card would cull a different set of
    // subtrees from the CPU, which is a quietly different building. Same fault as the op numbers
    // above, same fallback — read both copies out of their own files and hold them together.
    const std::map<std::string, u32> shader = shader_defines("WS_NODE_");
    const std::map<std::string, u32> host = host_constants();
    REQUIRE_MESSAGE(!shader.empty(), "field_types.glsl defined no WS_NODE_ constants at all");
    REQUIRE_MESSAGE(!host.empty(), "field_gpu.hpp defined no inline constexpr u32 constants");

    const std::pair<const char*, const char*> pairs[] = {
        {"WS_NODE_COUNT_MASK", "kNodeCountMask"},
        {"WS_NODE_BOUNDED", "kNodeBounded"},
        {"WS_NODE_CHILD_0", "kNodeChild0"},
        {"WS_NODE_CULLABLE", "kNodeCullable"},
    };
    for (const auto& [card, cpu] : pairs) {
        const auto on_card = shader.find(card);
        const auto on_cpu = host.find(cpu);
        REQUIRE_MESSAGE(on_card != shader.end(), "field_types.glsl has no " << card);
        REQUIRE_MESSAGE(on_cpu != host.end(), "field_gpu.hpp has no " << cpu);
        CHECK_MESSAGE(on_card->second == on_cpu->second,
                      card << " is " << on_card->second << " on the card and " << cpu << " is "
                           << on_cpu->second << " on the host");
    }

    // And the layout has to hold what it is asked to hold. Four children at most, each with a bit
    // of its own above the count's byte, and the sort flag above those — all inside one word with
    // nothing overlapping. A count mask one bit short, or a child bit landing on the flag, is the
    // same silent class of fault as a wrong constant.
    const u32 mask = shader.at("WS_NODE_COUNT_MASK");
    const u32 bounded = shader.at("WS_NODE_BOUNDED");
    const u32 child0 = shader.at("WS_NODE_CHILD_0");
    const u32 cullable = shader.at("WS_NODE_CULLABLE");
    CHECK(mask >= 4u);                       // a node holds four children at most
    CHECK((mask & bounded) == 0u);
    CHECK((mask & cullable) == 0u);
    CHECK((bounded & cullable) == 0u);
    for (u32 slot = 0; slot < 4; ++slot) {
        const u32 bit = child0 << slot;
        CHECK_MESSAGE((bit & mask) == 0u, "child bit " << slot << " lands on the count");
        CHECK_MESSAGE((bit & bounded) == 0u, "child bit " << slot << " lands on WS_NODE_BOUNDED");
        CHECK_MESSAGE((bit & cullable) == 0u, "child bit " << slot << " lands on WS_NODE_CULLABLE");
    }
}

TEST_CASE("the card's sorting network puts a union's children in `Field::eval`'s order") {
    // The card cannot run `Field::eval`'s insertion sort — data-dependent indices are scratch
    // memory there where five constant-index comparisons stay in registers — so it sorts the four
    // children with a network instead. That is only allowed if the two produce the SAME sequence,
    // because the order decides which subtrees the cull skips and therefore what the answer is:
    // D644 measured the same cull reordered as 58 differing points and 0.139 m.
    //
    // Exhaustive over three distinct distances in every arrangement, which is 81 cases and covers
    // every tie pattern there is — and ties are the ordinary case, since every child with no box
    // scores exactly nought.
    const f64 values[3] = {0.0, 1.0, 2.0};
    u32 checked = 0;
    u32 ties = 0;
    for (u32 a = 0; a < 3; ++a) {
        for (u32 b = 0; b < 3; ++b) {
            for (u32 c = 0; c < 3; ++c) {
                for (u32 d = 0; d < 3; ++d) {
                    const std::array<f64, 4> away{values[a], values[b], values[c], values[d]};
                    if (a == b || a == c || a == d || b == c || b == d || c == d) ++ties;
                    for (u32 children = 1; children <= 4; ++children) {
                        // The shader gives a slot past the count a huge key so it sorts to the end,
                        // where the walk stops before reading it. Everything below the count must
                        // then agree with the CPU exactly.
                        std::array<f64, 4> keys = away;
                        for (u32 i = children; i < 4; ++i) keys[i] = 3.0e38;
                        const std::array<u32, 4> card = network_order(keys);
                        const std::array<u32, 4> cpu = eval_order(away, children);
                        for (u32 i = 0; i < children; ++i) {
                            CHECK_MESSAGE(card[i] == cpu[i],
                                          "slot " << i << " of " << children << " children: the "
                                                  << "card says " << card[i] << " and Field::eval "
                                                  << "says " << cpu[i]);
                        }
                        ++checked;
                    }
                }
            }
        }
    }
    // Nought cases checked would pass every assertion above, which is trap 15 exactly.
    CHECK(checked == 81 * 4);
    CHECK(ties > 0);
}

TEST_CASE("the accelerator's word describes the boxes the field actually has") {
    // The rule, and it is the whole of the host's half: the low byte is the child count, a bit says
    // this node's own box is finite, a bit per slot says that CHILD's is, and the sort flag says
    // there is more than one child and something to sort by.
    //
    // **Nothing here is a new bound.** The bits are read off `Field::bounds_of` — the same boxes
    // that were already being uploaded, under-stating primitives and all. A card that could reject
    // a subtree the CPU cannot is a card building a second world, and D646 built the sound version
    // of that idea and measured it at 45x for a byte-identical building.
    forge::Field f;
    const u32 near_box = f.box({-10, 0, 0}, {1, 1, 1});
    const u32 far_box = f.box({10, 0, 0}, {1, 1, 1});
    const u32 grain = f.fbm(1.0, 3, 0.5, 2.0, 7);      // a pattern: everywhere, so no box
    const u32 pair = f.unite({near_box, far_box});
    const u32 with_grain = f.unite({near_box, far_box, grain});
    const u32 carved = f.subtract({near_box, far_box});
    f.build_bounds();

    // The premise first, because every assertion below is vacuous if the field turns out to be all
    // bounded or all not. D675 counted 923 of the estate's 18,250 nodes carrying no box; this clip
    // needs at least one of each for the test to be about anything.
    REQUIRE_FALSE(f.bounds_of(near_box).infinite());
    REQUIRE(f.bounds_of(grain).infinite());

    const auto word_of = [&](u32 at) {
        const forge::Node& n = f.node(at);
        u32 word = n.children & 0xFFu;
        if (!f.bounds_of(at).infinite()) word |= 0x100u;
        u32 bounded_children = 0;
        const u32 slots = (n.children < 4u) ? n.children : 4u;
        for (u32 c = 0; c < slots; ++c) {
            if (n.child[c] >= f.size()) continue;
            if (f.bounds_of(n.child[c]).infinite()) continue;
            word |= 0x200u << c;
            ++bounded_children;
        }
        if (n.children > 1u && bounded_children > 0) word |= 0x2000u;
        return word;
    };

    // Two boxes ten metres apart: both bounded, the union bounded, and worth sorting. This is the
    // case the whole step is for — unsorted, a point standing in the far box evaluates the near one
    // in full before it holds a number small enough to reject anything (D638).
    CHECK((word_of(pair) & 0xFFu) == 2u);
    CHECK((word_of(pair) & 0x100u) != 0u);
    CHECK((word_of(pair) & 0x200u) != 0u);
    CHECK((word_of(pair) & 0x400u) != 0u);
    CHECK((word_of(pair) & 0x2000u) != 0u);

    // Add a grain and the union loses its own box — a pattern is everywhere — but the two boxed
    // children keep theirs and the sort still has something to sort by. An ancestor of an unbounded
    // node cannot be bounded, and that is why a quarter of the estate's tree can never be culled.
    CHECK((word_of(with_grain) & 0xFFu) == 3u);
    CHECK((word_of(with_grain) & 0x100u) == 0u);
    CHECK((word_of(with_grain) & 0x200u) != 0u);
    CHECK((word_of(with_grain) & 0x400u) != 0u);
    CHECK((word_of(with_grain) & 0x800u) == 0u);   // the grain
    CHECK((word_of(with_grain) & 0x2000u) != 0u);

    // A difference is not sorted — its children are not interchangeable — but its carves carry
    // boxes, which is what lets the walk skip a cutter the point is nowhere near.
    CHECK((word_of(carved) & 0xFFu) == 2u);
    CHECK((word_of(carved) & 0x400u) != 0u);

    // And the count has to fit the byte it is packed into, for every node of a real expression.
    // Four is the most `Field::combine` ever writes — a wider union is folded into a chain — and if
    // that ever stopped being true the count would run into the accelerator's bits and the walk
    // would read a child index that is not a child index.
    for (usize i = 0; i < f.size(); ++i) {
        CHECK(f.node(static_cast<u32>(i)).children <= 4u);
    }
}

TEST_CASE("a node is eight voxels a side, which is what makes the dispatch flat") {
    // 512 cells a node is written into `shaders/sample_field.comp` as WS_NODE_CELLS and into
    // `FieldSampler::kNodeCells`, and it is what lets one dispatch cover a whole batch with a flat
    // index and no prefix sum over the boxes. If the leaf level ever moves, both have to move with
    // it — and neither would fail to compile.
    //
    // The RECORD sizes are the other thing the two sides must agree about, and they are asserted
    // where a mistake stops the build instead: `static_assert` on each struct in
    // `src/gpu/field_gpu.hpp`. They cannot be checked here because this suite links no Vulkan --
    // which is deliberate, and the reason is in CMakeLists beside WS_TEST_SOURCES_HEADLESS.
    CHECK(forge::kNodeVoxels == 8);
    CHECK(forge::kNodeVoxels * forge::kNodeVoxels * forge::kNodeVoxels == 512);
}
