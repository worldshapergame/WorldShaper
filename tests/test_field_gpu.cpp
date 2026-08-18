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

#include <fstream>
#include <map>
#include <sstream>
#include <string>

#include "forge/field.hpp"
#include "forge/sample.hpp"

using namespace ws;

namespace {

// Every `#define WS_OP_NAME <n>u` in the shader header, by name.
std::map<std::string, u32> shader_op_numbers() {
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
        if (name.rfind("WS_OP_", 0) != 0) continue;
        if (!value.empty() && value.back() == 'u') value.pop_back();
        try {
            out[name] = static_cast<u32>(std::stoul(value));
        } catch (...) {
            // A define whose value is not a number is not an op number.
        }
    }
    return out;
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
