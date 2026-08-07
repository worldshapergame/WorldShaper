// Automatic quality. The controller decides what the game looks like on a machine nobody
// here owns, so its behaviour has to be pinned rather than eyeballed on this one.

#include <doctest/doctest.h>

#include "game/quality.hpp"

using namespace ws;

namespace {

// Feed the controller a steady frame time until it settles or gives up.
u32 settle(AutoQuality& q, f64 frame_ms, u32 frames = 4000) {
    for (u32 i = 0; i < frames; ++i) q.observe(frame_ms);
    return q.level();
}

}  // namespace

TEST_CASE("the ladder only ever gets cheaper going down") {
    // Every rung must be at least as cheap as the one above it on every axis, or stepping
    // down could make the frame *slower* — a controller that reacts by making things worse
    // will keep reacting.
    for (u32 level = 1; level < kQualityLevels; ++level) {
        const QualityKnobs lower = quality_at(level - 1);
        const QualityKnobs upper = quality_at(level);
        CHECK(lower.resolution_scale <= upper.resolution_scale);
        CHECK(lower.detail_bias >= upper.detail_bias);        // higher bias = less detail
        CHECK(lower.refine_stride >= upper.refine_stride);    // longer stride = fewer rays
        CHECK(lower.bounce_limit <= upper.bounce_limit);
        CHECK(lower.shadow_target <= upper.shadow_target);
    }
}

TEST_CASE("the top rung is full detail and the bottom is not") {
    const QualityKnobs top = quality_at(kQualityLevels - 1);
    CHECK(top.resolution_scale == doctest::Approx(1.0f));
    CHECK(top.detail_bias == doctest::Approx(1.0f));

    const QualityKnobs bottom = quality_at(0);
    CHECK(bottom.resolution_scale < 1.0f);
    CHECK(bottom.refine_stride > top.refine_stride);
}

TEST_CASE("resolution is the last thing spent") {
    // Scaling the whole picture is the one change nobody fails to notice, so the rungs that
    // still render at full resolution have to reach well down the ladder.
    CHECK(quality_at(kQualityLevels - 1).resolution_scale == doctest::Approx(1.0f));
    CHECK(quality_at(3).resolution_scale == doctest::Approx(1.0f));
    CHECK(quality_at(2).resolution_scale < 1.0f);
}

TEST_CASE("a machine that cannot hold the target drops, and stops dropping") {
    AutoQuality q;
    q.create(60.0f, kQualityLevels - 1);
    // 40 ms a frame against a 16.7 ms budget: hopeless at any level.
    CHECK(settle(q, 40.0) == 0);
    // And it stays there rather than wrapping or going negative.
    CHECK(settle(q, 40.0) == 0);
}

TEST_CASE("a machine with room to spare climbs to the top and stays") {
    AutoQuality q;
    q.create(60.0f, 0);
    CHECK(settle(q, 1.0) == kQualityLevels - 1);
    CHECK(settle(q, 1.0) == kQualityLevels - 1);
}

TEST_CASE("it does not oscillate when a frame time sits on the budget") {
    // The failure this guards is the picture visibly breathing: a controller that raises into
    // a frame time it cannot hold, drops, and repeats for ever.
    AutoQuality q;
    q.create(60.0f, 4);
    settle(q, 16.0);   // just inside a 16.67 ms budget

    const u32 settled = q.level();
    u32 changes = 0;
    for (u32 i = 0; i < 2000; ++i) {
        if (q.observe(16.0)) ++changes;
    }
    CHECK(changes == 0);
    CHECK(q.level() == settled);
}

TEST_CASE("one stalled frame does not drop the level") {
    AutoQuality q;
    q.create(60.0f, 5);
    settle(q, 10.0);
    const u32 before = q.level();
    // A chunk arriving, a window dragged: one 200 ms frame among healthy ones.
    q.observe(200.0);
    for (u32 i = 0; i < 10; ++i) q.observe(10.0);
    CHECK(q.level() == before);
}

TEST_CASE("dropping is quicker than climbing") {
    // Being below the target is felt at once; being above it is not felt at all.
    AutoQuality q;
    q.create(60.0f, 4);
    u32 frames_to_drop = 0;
    while (q.level() == 4 && frames_to_drop < 10000) { q.observe(30.0); ++frames_to_drop; }

    AutoQuality r;
    r.create(60.0f, 4);
    u32 frames_to_raise = 0;
    while (r.level() == 4 && frames_to_raise < 10000) { r.observe(2.0); ++frames_to_raise; }

    CHECK(frames_to_drop < frames_to_raise);
}

TEST_CASE("turning it off freezes the level wherever it was") {
    AutoQuality q;
    q.create(60.0f, 5);
    q.set_enabled(false);
    CHECK_FALSE(q.observe(500.0));
    CHECK(q.level() == 5);
    CHECK(settle(q, 500.0) == 5);
}

TEST_CASE("the target follows the monitor, and a faster monitor asks for more") {
    AutoQuality slow;
    slow.create(60.0f, 4);
    AutoQuality fast;
    fast.create(144.0f, 4);
    // 10 ms a frame is comfortable at 60 and impossible at 144.
    CHECK(settle(slow, 10.0) > settle(fast, 10.0));
}

TEST_CASE("a silly target is clamped rather than believed") {
    AutoQuality q;
    q.create(0.0f, 4);
    CHECK(q.target_fps() >= 20.0f);
    q.set_target_fps(100000.0f);
    CHECK(q.target_fps() <= 480.0f);
}

TEST_CASE("the first run picks a level from what the machine measured") {
    // Comfortably fast at full detail: start at the top.
    CHECK(level_for_frame_time(2.0, 60.0f) == kQualityLevels - 1);
    // Right on the budget: not the top, because the benchmark is one camera in one place and
    // the first thing a player does is walk somewhere more expensive.
    CHECK(level_for_frame_time(16.0, 60.0f) < kQualityLevels - 1);
    // Hopeless: the bottom.
    CHECK(level_for_frame_time(200.0, 60.0f) == 0);
    // Monotonic — a slower machine never gets a higher level.
    for (f64 ms = 1.0; ms < 100.0; ms += 1.0) {
        CHECK(level_for_frame_time(ms, 60.0f) >= level_for_frame_time(ms + 1.0, 60.0f));
    }
    // Nonsense in, top rung out, rather than a level nobody chose.
    CHECK(level_for_frame_time(0.0, 60.0f) == kQualityLevels - 1);
}
