#include "game/quality.hpp"

#include <algorithm>
#include <cmath>

namespace ws {

namespace {

// The rungs, spelled out rather than interpolated, because the order things degrade in is a
// judgement about what a player notices and not a curve.
//
// Reading up the table: the top rung is everything at full. Coming down, sample counts and the
// refine stride go first — they cost noise while moving and nothing when still. Bounce depth
// next; Russian roulette already ends most paths long before the limit, so lowering it only
// touches the few that would have gone deepest. Detail bias after that, which pulls distance
// detail in slightly. Resolution only at the bottom two rungs, and only then because a machine
// that has run out of everything else has run out.
constexpr QualityKnobs kLadder[kQualityLevels] = {
    // scale, detail bias, refine stride, bounces, shadow target
    {0.50f, 1.60f, 16u,  4u, 24u},   // 0  last resort
    {0.65f, 1.40f, 12u,  6u, 32u},   // 1
    {0.80f, 1.25f, 10u,  8u, 40u},   // 2
    {1.00f, 1.15f,  8u, 12u, 48u},   // 3  first rung that renders at full resolution
    {1.00f, 1.05f,  6u, 20u, 64u},   // 4
    {1.00f, 1.00f,  5u, 32u, 80u},   // 5
    {1.00f, 1.00f,  4u, 48u, 96u},   // 6
    {1.00f, 1.00f,  2u, 64u, 128u},  // 7  everything, and more samples than the default
};

// How long a run of frames must agree before the level moves. About a third of a second down,
// a second and a half up, at sixty frames a second.
constexpr u32 kFramesToDrop = 20;
constexpr u32 kFramesToRaise = 90;

// Raise only when there is room for the *next* rung as well, not merely for this one. Without
// the margin a controller steps up into a frame time it cannot hold, drops back, and spends
// its life alternating — which is worse to look at than simply staying one rung low.
constexpr f64 kRaiseMargin = 0.80;

}  // namespace

QualityKnobs quality_at(u32 level) {
    return kLadder[std::min(level, kQualityLevels - 1u)];
}

void AutoQuality::create(f32 target_fps, u32 starting_level) {
    set_target_fps(target_fps);
    set_level(starting_level);
    smoothed_ms_ = budget_ms_;
    over_ = 0;
    under_ = 0;
}

void AutoQuality::set_target_fps(f32 fps) {
    target_fps_ = std::clamp(fps, 20.0f, 480.0f);
    budget_ms_ = 1000.0 / static_cast<f64>(target_fps_);
}

void AutoQuality::set_level(u32 level) {
    level_ = std::min(level, kQualityLevels - 1u);
    knobs_ = quality_at(level_);
    over_ = 0;
    under_ = 0;
}

bool AutoQuality::observe(f64 frame_ms) {
    if (!enabled_) return false;
    if (!(frame_ms > 0.0) || !std::isfinite(frame_ms)) return false;

    // A running average, so one stalled frame — a chunk arriving, a window being dragged —
    // does not drop the quality for everybody.
    smoothed_ms_ = (smoothed_ms_ <= 0.0) ? frame_ms : (smoothed_ms_ * 0.9 + frame_ms * 0.1);

    if (smoothed_ms_ > budget_ms_) {
        ++over_;
        under_ = 0;
    } else if (smoothed_ms_ < budget_ms_ * kRaiseMargin) {
        ++under_;
        over_ = 0;
    } else {
        // Inside the budget but not comfortably: exactly where it should sit. Neither counter
        // advances, so it stays put.
        over_ = 0;
        under_ = 0;
    }

    if (over_ >= kFramesToDrop && level_ > 0) {
        set_level(level_ - 1);
        return true;
    }
    if (under_ >= kFramesToRaise && level_ + 1 < kQualityLevels) {
        set_level(level_ + 1);
        return true;
    }
    return false;
}

u32 level_for_frame_time(f64 measured_ms, f32 target_fps) {
    // The benchmark runs at the top rung, so this asks how much of the budget full detail
    // costs on this machine and picks accordingly.
    if (!(measured_ms > 0.0) || !std::isfinite(measured_ms)) return kQualityLevels - 1;
    const f64 budget_ms = 1000.0 / static_cast<f64>(std::clamp(target_fps, 20.0f, 480.0f));
    const f64 ratio = measured_ms / budget_ms;

    // Start one rung below what the measurement strictly allows. The benchmark is one camera
    // in one place, and the first thing a player does is walk somewhere more expensive; being
    // asked to drop a level in the first ten seconds reads as the game misjudging itself,
    // where quietly climbing after a while reads as nothing at all.
    if (ratio <= 0.40) return kQualityLevels - 1;   // twice the headroom needed
    if (ratio <= 0.60) return 6;
    if (ratio <= 0.80) return 5;
    if (ratio <= 1.00) return 4;
    if (ratio <= 1.50) return 3;
    if (ratio <= 2.50) return 2;
    if (ratio <= 4.00) return 1;
    return 0;
}

}  // namespace ws
