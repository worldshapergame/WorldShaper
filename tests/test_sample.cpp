#include <doctest/doctest.h>

#include <cmath>
#include <string>
#include <vector>

#include "core/jobs.hpp"
#include "forge/sample.hpp"
#include "world/tags.hpp"
#include "world/voxel_type.hpp"

using namespace ws;
using namespace ws::forge;

namespace {

// What the sampler would produce if it never skipped anything: ask the field about every cell,
// at the cell's own centre, and paint it by walking every rule in order.
//
// This is the definition the fast sampler is an optimisation *of*. Every trick it plays — asking
// about a box instead of a point, settling a paint rule for a whole region, jumping through empty
// space — is only allowed if the answer is the same as this, voxel for voxel. So this is written
// once, deliberately stupidly, and everything else is checked against it.
Clip brute_force(const Field& field, u32 root, const std::vector<PaintRule>& paint,
                 const SampleSettings& settings) {
    const i32 per_metre = (settings.voxels_per_metre > 0) ? settings.voxels_per_metre
                                                          : kVoxelsPerMetre;
    const f64 voxel = 1.0 / static_cast<f64>(per_metre);
    const f64 shift = settings.sample_at_centre ? 0.5 : 0.0;

    const auto floor_at = [&](f64 metres) {
        return static_cast<i64>(std::floor(metres * static_cast<f64>(per_metre)));
    };
    const i64 lo[3] = {floor_at(settings.low.x), floor_at(settings.low.y), floor_at(settings.low.z)};
    const i64 hi[3] = {floor_at(settings.high.x), floor_at(settings.high.y),
                       floor_at(settings.high.z)};

    Clip clip;
    for (u32 axis = 0; axis < 3; ++axis) {
        clip.size[axis] = static_cast<i32>(hi[axis] - lo[axis]);
    }
    clip.voxels.assign(static_cast<usize>(clip.cell_count()), kAir);
    clip.inside.assign(static_cast<usize>(clip.cell_count()), 0);

    for (i32 z = 0; z < clip.size[2]; ++z) {
        for (i32 y = 0; y < clip.size[1]; ++y) {
            for (i32 x = 0; x < clip.size[0]; ++x) {
                const Vec3 p{(static_cast<f64>(lo[0] + x) + shift) * voxel,
                             (static_cast<f64>(lo[1] + y) + shift) * voxel,
                             (static_cast<f64>(lo[2] + z) + shift) * voxel};
                const usize index = clip.index(x, y, z);
                if (settings.has_bounds && field.eval(settings.bounds, p) > 0.0) continue;
                clip.inside[index] = 1;
                // The same rule the real sampler uses, from the same function: a centre outside the
                // shape is empty UNLESS something thinner than a voxel passes through the cell.
                const f64 here = field.eval(root, p);
                if (here > 0.0 &&
                    !(here < voxel * 0.8660254 &&
                      thin_feature_here(field, root, p, here, voxel))) {
                    continue;
                }

                VoxelTypeId type = kAir;
                for (const PaintRule& rule : paint) {
                    const f64 value = field.eval(rule.test, p);
                    if (value < rule.low || value > rule.high) continue;
                    if (rule.facing_axis < 3) {
                        const Vec3 n = field.normal_at(root, p, voxel);
                        const f64 component = (rule.facing_axis == 0)   ? n.x
                                              : (rule.facing_axis == 1) ? n.y
                                                                        : n.z;
                        if (rule.facing_min >= 0.0) {
                            if (component < rule.facing_min) continue;
                        } else {
                            if (component > rule.facing_min) continue;
                        }
                    }
                    type = rule.type;
                }
                if (type == kAir && !paint.empty()) type = paint.front().type;
                clip.voxels[index] = type;
            }
        }
    }
    return clip;
}

// Reports the first disagreement rather than just a count, because "two voxels differ" is not
// something anyone can act on and "cell (14, 3, 9) is stone here and air there" is.
// `std::string` and not `const char*`, which is not a style preference. doctest here is built
// without DOCTEST_CONFIG_TREAT_CHAR_STAR_AS_STRING, so `toString(const char*)` is never declared
// and a `const char*` handed to INFO falls through to the generic pointer stringifier: the label
// saying WHICH comparison failed printed as `1`. It only shows on failure, which is why it sat
// here unnoticed. Defining that macro is not the fix either -- it quotes every fragment, so the
// message becomes `"solids"": cell (...)"`. A std::string stringifies as itself.
void must_match(const Clip& fast, const Clip& slow, const std::string& what) {
    REQUIRE(fast.size[0] == slow.size[0]);
    REQUIRE(fast.size[1] == slow.size[1]);
    REQUIRE(fast.size[2] == slow.size[2]);
    for (i32 z = 0; z < slow.size[2]; ++z) {
        for (i32 y = 0; y < slow.size[1]; ++y) {
            for (i32 x = 0; x < slow.size[0]; ++x) {
                const usize i = slow.index(x, y, z);
                if (fast.voxels[i] == slow.voxels[i] && fast.inside[i] == slow.inside[i]) continue;
                INFO(what << ": cell (" << x << ", " << y << ", " << z << ") is type "
                          << fast.voxels[i] << "/inside " << int(fast.inside[i])
                          << " but should be type " << slow.voxels[i] << "/inside "
                          << int(slow.inside[i]));
                REQUIRE(false);
            }
        }
    }
}

struct Materials {
    TagRegistry tags;
    VoxelTypeTable types;

    VoxelTypeId make(u8 red) {
        VisualRecord visual{};
        visual.red = red;
        visual.green = 128;
        visual.blue = 128;
        visual.opacity = 255;
        BehaviourRecord behaviour{};
        return types.intern(visual, behaviour);
    }
};

}  // namespace

TEST_CASE("the sampler agrees with asking every voxel, whatever it skips") {
    Materials m;
    const VoxelTypeId stone = m.make(120);
    const VoxelTypeId moss = m.make(60);
    const VoxelTypeId trim = m.make(200);

    SampleSettings settings;
    settings.low = {-1.5, -1.5, -1.5};
    settings.high = {1.5, 1.5, 1.5};
    settings.voxels_per_metre = 32;

    JobSystem jobs;

    SUBCASE("plain solids and the space between them") {
        Field f;
        const u32 ball = f.sphere({0, 0, 0}, 0.9);
        const u32 slab = f.box({0, -1.0, 0}, {1.4, 0.2, 1.4}, 0.0);
        const u32 post = f.cylinder({0.7, 0, 0.7}, 0.15, 1.2, 1);
        const u32 all = f.unite({ball, slab, post});

        std::vector<PaintRule> paint;
        paint.push_back(PaintRule{f.constant(0.0), -1e30, 1e30, stone});
        paint.push_back(PaintRule{slab, -1e30, 0.0, trim});

        must_match(sample(f, all, paint, settings, &jobs).clip,
                   brute_force(f, all, paint, settings), "solids");
    }

    SUBCASE("a shape with pieces cut out of it") {
        Field f;
        const u32 block = f.box({0, 0, 0}, {1.0, 0.6, 1.0}, 0.05);
        const u32 hole = f.cylinder({0, 0, 0}, 0.35, 1.0, 1);
        const u32 notch = f.box({0.8, 0, 0}, {0.4, 0.3, 0.4}, 0.0);
        const u32 carved = f.subtract({block, hole, notch});

        std::vector<PaintRule> paint;
        paint.push_back(PaintRule{f.constant(0.0), -1e30, 1e30, stone});

        must_match(sample(f, carved, paint, settings, &jobs).clip,
                   brute_force(f, carved, paint, settings), "carved");
    }

    // The case the fast sampler is most likely to get wrong, and the one that cost 265 voxels
    // when the displacement slack was half what it should have been. A displaced surface can be
    // nearer than the field admits, so every skip has to allow for it.
    SUBCASE("a displaced surface, where the field understates how near it is") {
        Field f;
        const u32 block = f.box({0, 0, 0}, {1.0, 0.5, 1.0}, 0.0);
        const u32 grain = f.fbm(0.18, 3, 0.5, 2.0, 7u);
        const u32 rough = f.displace(block, grain, 0.09);

        std::vector<PaintRule> paint;
        paint.push_back(PaintRule{f.constant(0.0), -1e30, 1e30, stone});
        paint.push_back(PaintRule{grain, 0.2, 1e30, moss});

        must_match(sample(f, rough, paint, settings, &jobs).clip,
                   brute_force(f, rough, paint, settings), "displaced");
    }

    SUBCASE("paint keyed on patterns, which no box can settle in advance") {
        Field f;
        const u32 block = f.box({0, 0, 0}, {1.2, 0.4, 1.2}, 0.0);
        const u32 courses = f.stripes(1, 0.2, 0.15);
        const u32 speckle = f.noise(0.12, 3u);

        std::vector<PaintRule> paint;
        paint.push_back(PaintRule{f.constant(0.0), -1e30, 1e30, stone});
        paint.push_back(PaintRule{courses, 0.5, 1e30, trim});
        paint.push_back(PaintRule{speckle, 0.35, 1e30, moss});

        must_match(sample(f, block, paint, settings, &jobs).clip,
                   brute_force(f, block, paint, settings), "patterned");
    }

    SUBCASE("paint that follows the surface normal") {
        Field f;
        const u32 ball = f.sphere({0, 0, 0}, 1.0);

        std::vector<PaintRule> paint;
        paint.push_back(PaintRule{f.constant(0.0), -1e30, 1e30, stone});
        PaintRule up{f.constant(0.0), -1e30, 1e30, moss};
        up.facing_axis = 1;
        up.facing_min = 0.6;
        paint.push_back(up);

        must_match(sample(f, ball, paint, settings, &jobs).clip,
                   brute_force(f, ball, paint, settings), "facing");
    }

    SUBCASE("a clip narrowed by a bounds shape, so not every cell belongs to it") {
        Field f;
        const u32 slab = f.box({0, 0, 0}, {1.4, 0.5, 1.4}, 0.0);
        const u32 keep = f.cylinder({0, 0, 0}, 1.0, 1.4, 1);

        std::vector<PaintRule> paint;
        paint.push_back(PaintRule{f.constant(0.0), -1e30, 1e30, stone});

        SampleSettings bounded = settings;
        bounded.bounds = keep;
        bounded.has_bounds = true;

        must_match(sample(f, slab, paint, bounded, &jobs).clip,
                   brute_force(f, slab, paint, bounded), "bounded");
    }

    // Repetition folds a coordinate into its nearest cell, which gives the distance to the copy
    // in *that* cell rather than to the nearest copy. With the shape hard against one side of its
    // cell — a row of slats, a colonnade — those differ, and the fold reports the larger. A
    // sampler that trusted it skipped whole slats.
    SUBCASE("a row of repeated slats, each off-centre in its cell") {
        Field f;
        const u32 slat = f.box({0.05, 0.0, 0.0}, {0.05, 0.9, 0.25}, 0.0);
        const u32 row = f.repeat(slat, {0.26, 0.0, 0.0}, {5.0, 0.0, 0.0});
        const u32 plinth = f.box({0, -1.0, 0}, {1.4, 0.15, 1.4}, 0.0);
        const u32 all = f.unite({row, plinth});

        std::vector<PaintRule> paint;
        paint.push_back(PaintRule{f.constant(0.0), -1e30, 1e30, stone});

        must_match(sample(f, all, paint, settings, &jobs).clip,
                   brute_force(f, all, paint, settings), "repeated");
    }

    // A surface of revolution is the shape the whole building is made of, and the sampler treats
    // it as a true distance — it settles whole boxes inside a column base from one reading. If
    // that trust is misplaced anywhere the base comes out with holes in it, so it is checked
    // against asking every voxel.
    SUBCASE("a profile turned about an axis, which the sampler settles boxes inside") {
        Field f;
        const u32 section = f.unite({f.box({0.55, 0.1, 0}, {0.25, 0.1, 2.0}, 0.0),
                                     f.sphere({0.5, 0.35, 0}, 0.2),
                                     f.box({0.3, 0.7, 0}, {0.15, 0.25, 2.0}, 0.0)});
        const u32 turned = f.revolve(section, {0, -0.9, 0}, 1);
        const u32 floor_ = f.box({0, -1.2, 0}, {1.4, 0.35, 1.4}, 0.0);
        const u32 all = f.unite({turned, floor_});
        f.build_bounds();

        std::vector<PaintRule> paint;
        paint.push_back(PaintRule{f.constant(0.0), -1e30, 1e30, stone});
        paint.push_back(PaintRule{turned, -1e30, 0.0, trim});

        must_match(sample(f, all, paint, settings, &jobs).clip,
                   brute_force(f, all, paint, settings), "revolved");
    }

    // A volute is a thin tube that doubles back on itself several times within a small box, which
    // is the hardest thing in the building for a sampler to believe about: most of its bounding
    // box is air, and the air is threaded.
    SUBCASE("a spiral scroll, thin and folded back on itself") {
        Field f;
        const u32 scroll = f.spiral({0, 0, 0}, 1.0, 0.55, 0.06, 2.5, 2);
        const u32 eye = f.cylinder({0, 0, 0}, 0.09, 0.12, 2);
        const u32 all = f.unite({scroll, eye});
        f.build_bounds();

        std::vector<PaintRule> paint;
        paint.push_back(PaintRule{f.constant(0.0), -1e30, 1e30, stone});

        must_match(sample(f, all, paint, settings, &jobs).clip,
                   brute_force(f, all, paint, settings), "spiral");
    }

    // The case scoped weathering produces: a shape displaced by a mask multiplied by an amount.
    // The allowance for that is now worked out from the pattern's real range rather than assumed,
    // which is what lets any of it settle — and an allowance that is too small is a hole.
    SUBCASE("a displacement scoped by a mask, the shape every weathering makes") {
        Field f;
        const u32 block = f.box({0, 0, 0}, {1.0, 0.5, 1.0}, 0.0);
        const u32 half = f.box({-0.6, 0, 0}, {0.6, 1.0, 1.4}, 0.0);
        const u32 mask = f.smoothstep(f.negate(half), -0.06, 0.0);
        const u32 grain = f.fbm(0.16, 3, 0.5, 2.0, 13u);
        const u32 scoped =
            f.displace(block, f.multiply({grain, mask, f.constant(0.8)}), 0.12);
        f.build_bounds();

        std::vector<PaintRule> paint;
        paint.push_back(PaintRule{f.constant(0.0), -1e30, 1e30, stone});
        paint.push_back(PaintRule{f.add({grain, f.multiply({mask, f.constant(-1e9)})}), 0.2, 1e30,
                                  moss});

        must_match(sample(f, scoped, paint, settings, &jobs).clip,
                   brute_force(f, scoped, paint, settings), "scoped displacement");
    }

    SUBCASE("many small things, so most boxes hold nothing at all") {
        Field f;
        std::vector<u32> parts;
        for (i32 i = -1; i <= 1; ++i) {
            for (i32 k = -1; k <= 1; ++k) {
                parts.push_back(f.sphere({static_cast<f64>(i) * 0.9, 0.0,
                                          static_cast<f64>(k) * 0.9}, 0.16));
            }
        }
        const u32 scatter = f.unite(parts);

        std::vector<PaintRule> paint;
        paint.push_back(PaintRule{f.constant(0.0), -1e30, 1e30, stone});

        must_match(sample(f, scatter, paint, settings, &jobs).clip,
                   brute_force(f, scatter, paint, settings), "scattered");
    }
}

// Every subcase above is at the world's own thirty-two voxels a metre, and until R11 nothing asked
// the sampler for less except the clip ladder's one coarse rung. R11 asks for eight resolutions:
// a node at level 8 is one voxel to the metre, where a voxel is larger than most of the building's
// detail and the thin-feature rule is carrying the whole answer.
//
// This is where `--sample-cost` found the sampler disagreeing with ITSELF -- the same node came out
// differently sampled alone and sampled inside the whole building, at one and two voxels a metre --
// so the definition has to be asked which of the two is right.
TEST_CASE("the sampler agrees with asking every voxel when a voxel is bigger than the detail") {
    Materials m;
    const VoxelTypeId stone = m.make(120);
    const VoxelTypeId trim = m.make(200);

    Field f;
    // Slats a fifth of a metre thick, a plinth, and a scroll: at one voxel to the metre every one
    // of these is thinner than a voxel, which is the case the coarse rungs of R11 are made of.
    const u32 slat = f.box({0.05, 0.0, 0.0}, {0.05, 0.9, 0.25}, 0.0);
    const u32 row = f.repeat(slat, {0.26, 0.0, 0.0}, {5.0, 0.0, 0.0});
    const u32 plinth = f.box({0, -1.0, 0}, {1.4, 0.15, 1.4}, 0.0);
    const u32 scroll = f.spiral({0, 1.2, 0}, 0.8, 0.45, 0.05, 2.0, 2);
    const u32 all = f.unite({row, plinth, scroll});
    f.build_bounds();

    std::vector<PaintRule> paint;
    paint.push_back(PaintRule{f.constant(0.0), -1e30, 1e30, stone});
    paint.push_back(PaintRule{plinth, -1e30, 0.0, trim});

    JobSystem jobs;
    for (i32 per_metre : {1, 2, 4, 8, 16}) {
        SampleSettings settings;
        settings.low = {-2.0, -2.0, -2.0};
        settings.high = {2.0, 2.0, 2.0};
        settings.voxels_per_metre = per_metre;
        CAPTURE(per_metre);
        must_match(sample(f, all, paint, settings, &jobs).clip,
                   brute_force(f, all, paint, settings), "coarse");
    }
}

// And the same question asked of the BOX rather than of the resolution: the sampler settles whole
// regions from one reading, and how much it can settle depends on how big a box it was handed. Two
// boxes over the same voxels must still agree, or the world's geometry depends on how large a bite
// it was sampled in -- which is what R11 is about to make vary from node to node.
TEST_CASE("a small box and a big one agree about the voxels they share") {
    Materials m;
    const VoxelTypeId stone = m.make(120);

    Field f;
    const u32 slat = f.box({0.05, 0.0, 0.0}, {0.05, 0.9, 0.25}, 0.0);
    const u32 row = f.repeat(slat, {0.26, 0.0, 0.0}, {5.0, 0.0, 0.0});
    const u32 ball = f.sphere({0.4, 0.5, 0.3}, 0.35);
    const u32 all = f.unite({row, ball});
    f.build_bounds();

    std::vector<PaintRule> paint;
    paint.push_back(PaintRule{f.constant(0.0), -1e30, 1e30, stone});

    JobSystem jobs;
    // The third box is the first GROWN BY ONE VOXEL, which is the case R11b's skirt makes: a node
    // sampled with a one-voxel margin so its edge voxels can see their true neighbours. It is not
    // the same question as a big box against a small one -- the grown box is a different size on
    // every axis, so the descent halves it differently all the way down, and every settle decision
    // is taken over a box the aligned run never asked about.
    for (i32 per_metre : {1, 2, 4, 32}) {
        SampleSettings big;
        big.low = {-4.0, -4.0, -4.0};
        big.high = {4.0, 4.0, 4.0};
        big.voxels_per_metre = per_metre;
        const SampleResult whole = sample(f, all, paint, big, &jobs);

        SampleSettings small = big;
        small.low = {-1.0, -1.0, -1.0};
        small.high = {1.0, 1.0, 1.0};
        const SampleResult part = sample(f, all, paint, small, &jobs);

        const f64 voxel = 1.0 / static_cast<f64>(per_metre);
        SampleSettings grown = small;
        grown.low = {small.low.x - voxel, small.low.y - voxel, small.low.z - voxel};
        grown.high = {small.high.x + voxel, small.high.y + voxel, small.high.z + voxel};
        const SampleResult skirted = sample(f, all, paint, grown, &jobs);

        CAPTURE(per_metre);
        REQUIRE(!part.clip.empty());
        const auto same_as = [&](const SampleResult& other, const std::string& what) {
            for (i32 z = 0; z < part.clip.size[2]; ++z) {
                for (i32 y = 0; y < part.clip.size[1]; ++y) {
                    for (i32 x = 0; x < part.clip.size[0]; ++x) {
                        const i32 at[3] = {
                            static_cast<i32>(part.origin_voxel[0] - other.origin_voxel[0]) + x,
                            static_cast<i32>(part.origin_voxel[1] - other.origin_voxel[1]) + y,
                            static_cast<i32>(part.origin_voxel[2] - other.origin_voxel[2]) + z};
                        if (part.clip.at(x, y, z) == other.clip.at(at[0], at[1], at[2])) continue;
                        INFO(what << ": cell (" << x << ", " << y << ", " << z << ") is "
                                  << part.clip.at(x, y, z) << " alone and "
                                  << other.clip.at(at[0], at[1], at[2]) << " there");
                        REQUIRE(false);
                    }
                }
            }
        };
        same_as(whole, "against a box four times the size");
        same_as(skirted, "against the same box grown by one voxel");
    }
}

// R11a's primitive, checked against the tree it is a mapping onto. Two structures answering one
// question is this project's most repeated fault (trap 13), and "where is a node and how finely is
// it asked" is about to be answered by the sampler as well as by the pool.
TEST_CASE("a node is eight voxels a side at every level, on the pool's own grid") {
    for (u32 level = kLeafLevel; level <= kCoarsestSampledLevel; ++level) {
        const i32 per_metre = node_voxels_per_metre(level);
        REQUIRE(per_metre > 0);

        // Straddling the origin deliberately: the facility does, and a node grid that rounds
        // towards zero is one node out on half the building.
        for (i64 at : {i64{-3}, i64{-1}, i64{0}, i64{5}}) {
            const NodeKey key{at, at + 1, at - 2, level};
            const NodeBox box = node_box(key);

            // The box is exactly the voxels the pool says the node covers.
            const f64 per_world = static_cast<f64>(kVoxelsPerMetre);
            CHECK(box.low.x * per_world == doctest::Approx(static_cast<f64>(key.x << level)));
            CHECK(box.high.z * per_world ==
                  doctest::Approx(static_cast<f64>((key.z + 1) << level)));
            // And a voxel inside it belongs to that node, by the pool's own arithmetic.
            const i64 inside_voxel = (key.x << level) + ((i64{1} << level) / 2);
            CHECK(node_key_of(inside_voxel, inside_voxel, inside_voxel, level).x == key.x);

            // Eight voxels a side, whatever the level, which is the whole point of 256 / 2^level.
            const f64 span = box.high.x - box.low.x;
            CHECK(span * static_cast<f64>(per_metre) == doctest::Approx(kNodeVoxels));
        }
    }
    // The leaf is the world's own resolution and the coarsest node is a metre a voxel. If either
    // of these moves, the ladder of levels no longer lands on the voxel grid at all.
    CHECK(node_voxels_per_metre(kLeafLevel) == kVoxelsPerMetre);
    CHECK(node_voxels_per_metre(kCoarsestSampledLevel) == 1);
    CHECK(node_voxels_per_metre(kCoarsestSampledLevel + 1) == 0);
}

// The R11a gate, in miniature and headless. `--sample-cost` asks it of the facility at every
// level; this asks it of the shapes most likely to break it, in a second, so a regression is
// caught by test.bat rather than by somebody running the instrument.
//
// What could go wrong is not obvious, which is why it is checked rather than assumed: the sampler
// settles a paint rule for a whole box from one reading at its centre, and a box eight voxels wide
// gives it almost nothing to settle against. A rule that comes out differently at that size is a
// world whose colour depends on how big a bite it was sampled in.
TEST_CASE("a node sampled alone is the same node sampled inside a bigger box") {
    Materials m;
    const VoxelTypeId stone = m.make(120);
    const VoxelTypeId moss = m.make(60);
    const VoxelTypeId trim = m.make(200);

    Field f;
    // A displaced surface, a pattern-keyed coat and a shape with holes in it: the three things the
    // sampler takes shortcuts on, all inside two metres so every level from the leaf up fits.
    const u32 block = f.box({0, 0, 0}, {1.0, 0.5, 1.0}, 0.0);
    const u32 grain = f.fbm(0.18, 3, 0.5, 2.0, 7u);
    const u32 rough = f.displace(block, grain, 0.06);
    const u32 hole = f.cylinder({0.3, 0, 0.3}, 0.2, 1.2, 1);
    const u32 carved = f.subtract({rough, hole});
    const u32 courses = f.stripes(1, 0.2, 0.15);
    f.build_bounds();

    std::vector<PaintRule> paint;
    paint.push_back(PaintRule{f.constant(0.0), -1e30, 1e30, stone});
    paint.push_back(PaintRule{courses, 0.5, 1e30, trim});
    paint.push_back(PaintRule{grain, 0.2, 1e30, moss});

    SampleSettings authored;
    authored.low = {-1.0, -1.0, -1.0};
    authored.high = {1.0, 1.0, 1.0};

    JobSystem jobs;
    usize checked = 0;
    for (u32 level = kLeafLevel; level <= kCoarsestSampledLevel; ++level) {
        // The bigger box: everything, at this level's resolution, snapped out to this level's own
        // node boundaries. Without the snap a two-metre box is half a node wide at level 6 and the
        // nodes over it hang off the side of their own reference, which is a fault in the test
        // rather than in the sampler -- and it is exactly what the instrument does for the same
        // reason.
        SampleSettings whole_settings = authored;
        whole_settings.voxels_per_metre = node_voxels_per_metre(level);
        const f64 node_m = static_cast<f64>(i64{1} << level) / static_cast<f64>(kVoxelsPerMetre);
        whole_settings.low = {std::floor(authored.low.x / node_m) * node_m,
                              std::floor(authored.low.y / node_m) * node_m,
                              std::floor(authored.low.z / node_m) * node_m};
        whole_settings.high = {std::ceil(authored.high.x / node_m) * node_m,
                               std::ceil(authored.high.y / node_m) * node_m,
                               std::ceil(authored.high.z / node_m) * node_m};
        const SampleResult whole = sample(f, carved, paint, whole_settings, &jobs);
        if (whole.clip.empty()) continue;

        const i64 nodes[3] = {whole.clip.size[0] / kNodeVoxels, whole.clip.size[1] / kNodeVoxels,
                              whole.clip.size[2] / kNodeVoxels};
        std::vector<VoxelTypeId> want_types(static_cast<usize>(kNodeVoxels) * kNodeVoxels *
                                            kNodeVoxels);
        std::vector<u8> want_inside(want_types.size());
        for (i64 z = 0; z < nodes[2]; ++z) {
            for (i64 y = 0; y < nodes[1]; ++y) {
                for (i64 x = 0; x < nodes[0]; ++x) {
                    // One node in three at the finest level. Every node there is five hundred and
                    // twelve separate samples, and this runs on every test.bat; the coarser levels
                    // are walked entire.
                    if (level == kLeafLevel && ((x + y + z) % 3) != 0) continue;
                    const NodeKey key{(whole.origin_voxel[0] >> kLeafLevel) + x,
                                      (whole.origin_voxel[1] >> kLeafLevel) + y,
                                      (whole.origin_voxel[2] >> kLeafLevel) + z, level};
                    REQUIRE(node_block(whole, key, want_types.data(), want_inside.data()));

                    const SampleResult alone =
                        sample(f, carved, paint, node_sample_settings(authored, key), &jobs);
                    REQUIRE(alone.clip.size[0] == kNodeVoxels);

                    Clip expected;
                    expected.size[0] = kNodeVoxels;
                    expected.size[1] = kNodeVoxels;
                    expected.size[2] = kNodeVoxels;
                    expected.voxels = want_types;
                    expected.inside = want_inside;
                    must_match(alone.clip, expected, "one node against the box around it");
                    ++checked;
                }
            }
        }
    }
    // Trap 10 in the test rather than in the instrument: a check that ran over nothing passes.
    CHECK(checked > 100);
}

TEST_CASE("the sampler gives the same answer with a job system and without") {
    Materials m;
    const VoxelTypeId stone = m.make(120);

    Field f;
    const u32 ball = f.sphere({0, 0, 0}, 0.8);
    const u32 bar = f.box({0, 0, 0}, {1.2, 0.1, 0.1}, 0.0);
    const u32 all = f.unite({ball, bar});

    std::vector<PaintRule> paint;
    paint.push_back(PaintRule{f.constant(0.0), -1e30, 1e30, stone});

    SampleSettings settings;
    settings.low = {-1.3, -1.3, -1.3};
    settings.high = {1.3, 1.3, 1.3};
    settings.voxels_per_metre = 24;

    JobSystem jobs;
    const Clip threaded = sample(f, all, paint, settings, &jobs).clip;
    const Clip serial = sample(f, all, paint, settings, nullptr).clip;
    must_match(threaded, serial, "threading");
}
