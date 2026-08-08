#include <doctest/doctest.h>

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
                if (field.eval(root, p) > 0.0) continue;

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
void must_match(const Clip& fast, const Clip& slow, const char* what) {
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
