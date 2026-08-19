#pragma once
// Automatic quality: hold a frame rate by spending detail where it is worth most.
//
// Measured first, decided after (documentation/19-auto-quality.md). At 1280x720 on the
// development machine one pass dominates each mode and everything else is rounding error:
//
//     real time    visibility  1.233 ms of a 1.313 ms frame   (94%)
//     path traced  pathtrace   2.495 ms outdoors, 19.988 ms in an enclosed room
//
// So the knobs worth having are the ones inside those two shaders, and the enclosed path-
// traced case is where the headroom has to come from. Streaming, resolve, blit and the HUD
// together are under a tenth of a millisecond and are not worth a knob at all — a setting
// that cannot move the frame time is a setting that only costs the player a decision.

#include "core/types.hpp"

namespace ws {

// Everything the renderer is allowed to trade. One struct, so a quality level is a value that
// can be logged, compared and written to a file rather than a scattering of member variables.
struct QualityKnobs {
    // The fraction of the window the world is rendered at, before being scaled up to it. The
    // largest lever there is, and the most visible, so it moves last and least.
    f32 resolution_scale = 1.0f;

    // How coarsely the marcher is allowed to pick detail with distance. 1.0 is "a voxel when
    // a voxel is a pixel"; above that, detail drops sooner with distance.
    f32 detail_bias = 1.0f;

    // THESE TWO HAVE HAD NO READER SINCE R3d, AND MOVING THEM DOES NOTHING. See D704.
    //
    // Both are written into `TracePush::quality` every frame by `make_trace_push`, and `uvec4
    // quality` is declared in the GLSL mirrors of that block in shaders/clouds.comp and
    // shaders/resolve.comp -- and nothing anywhere reads it. The pass that would want a shadow
    // sample target is the FACE pass, which does not see this block at all: it takes its light
    // count from `node_push.light_count`, a different push constant.
    //
    // This is D577 from the other side. There, `kPreviewExposure` was a constant with no WRITER
    // after R3d deleted the tracer and R1e the buffer under it, and two clips written to test
    // exposure could not be used for months. Here it is writers with no READER. Both are the same
    // shape: a deletion leaves the two ends of a wire in different files, and neither end looks
    // broken on its own.
    //
    // Left in place rather than removed, because the words are part of a std140 block laid out by
    // POSITION and removing one without removing it from both mirrors is worse than leaving it.
    // What is owed is deciding whether the face pass should read them -- a shadow-sample target is
    // a real knob for it -- or whether they go with the tracer that wanted them.
    u32 refine_stride = 4;
    u32 shadow_target = 96;

    // There is no bounce-depth knob here, and there cannot be one. A path in this renderer
    // does not end by running out of depth: the far end of it reads the light already cached
    // on the face it lands on, which that face gathered the same way a frame earlier. So
    // light gains a bounce per frame and keeps it, and depth is a property of how long the
    // camera has been still rather than a number anybody sets.
    //
    // A `bounce_limit` was carried here for a while and copied into the tracer's push
    // constants every frame. The shader never read it, and could not have: there is no loop
    // for it to bound. Its absence is the design, not an omission.
};

// The ladder, from the least the game will ever look like to the most.
//
// Ordered by what each costs to look at, not by what each saves — the cheapest *harm* is spent
// first. Sample counts and stride go early: they cost noise while the camera moves and nothing
// at all once it stops, so on a machine that can hold the target while still they are free.
// Resolution goes last, because scaling the whole picture is the one change nobody fails to
// notice.
inline constexpr u32 kQualityLevels = 8;
QualityKnobs quality_at(u32 level);

// Holds a frame rate by walking that ladder.
//
// Deliberately slow to move and slower to move back. A controller that reacts to one frame
// chases every hitch and spends its life oscillating, which reads as the picture breathing;
// one that waits is invisible. Stepping down is quicker than stepping up because being below
// the target is felt immediately and being above it is not felt at all.
class AutoQuality {
public:
    // `target_fps` is normally the monitor's refresh rate: rendering faster than the display
    // can show is work nobody sees, and that spare time is better spent on samples.
    void create(f32 target_fps, u32 starting_level);

    // One frame's GPU time. Returns true when the level changed, so the caller can apply the
    // new knobs and say so.
    bool observe(f64 frame_ms);

    void set_target_fps(f32 fps);
    void set_enabled(bool on) { enabled_ = on; }
    bool enabled() const { return enabled_; }

    u32 level() const { return level_; }
    void set_level(u32 level);
    const QualityKnobs& knobs() const { return knobs_; }
    f32 target_fps() const { return target_fps_; }
    f64 smoothed_ms() const { return smoothed_ms_; }

private:
    bool enabled_ = true;
    f32 target_fps_ = 60.0f;
    f64 budget_ms_ = 1000.0 / 60.0;
    u32 level_ = kQualityLevels - 1;
    QualityKnobs knobs_{};

    f64 smoothed_ms_ = 0.0;
    u32 over_ = 0;    // consecutive frames past the budget
    u32 under_ = 0;   // consecutive frames comfortably inside it
};

// What the first run measures, and what it decides.
//
// A quality level chosen on the developer's machine is meaningless anywhere else, so the game
// measures the machine it is actually on before it draws anything the player will judge. One
// short benchmark at full detail, and the answer is the rung that fits.
u32 level_for_frame_time(f64 measured_ms, f32 target_fps);

}  // namespace ws
