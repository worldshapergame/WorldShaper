#include "game/clipboard.hpp"

#include <algorithm>
#include <cmath>
#include <new>

#include "core/log.hpp"
#include "core/time.hpp"
#include "world/raycast.hpp"
#include "world/world.hpp"

namespace ws {
namespace {

constexpr f64 kDegreesToRadians = 3.14159265358979323846 / 180.0;

// Which way "forward" is, rounded to an axis. Scrolling moves the clip along the axis you
// are most nearly facing, so the direction of travel is whatever you are looking at rather
// than a set of keys to learn.
void dominant_axis(const f64 direction[3], u32& axis, i32& sign) {
    axis = 0;
    for (u32 a = 1; a < 3; ++a) {
        if (std::abs(direction[a]) > std::abs(direction[axis])) axis = a;
    }
    sign = (direction[axis] < 0.0) ? -1 : 1;
}

i64 snap_to(i64 value, i64 step) {
    if (step <= 1) return value;
    const f64 scaled = static_cast<f64>(value) / static_cast<f64>(step);
    return static_cast<i64>(std::llround(scaled)) * step;
}

}  // namespace

const char* adjust_mode_name(AdjustMode mode) {
    switch (mode) {
        case AdjustMode::Copies: return "copies";
        case AdjustMode::Resize: return "size (arrows stretch)";
        default:                 return "?";
    }
}

void Clipboard::drop() {
    ++revision_;
    source_ = Clip{};
    shapes_.clear();
    instance_shape_.clear();
    for (u32 a = 0; a < 3; ++a) {
        offset_[a] = 0;
        angles_[a] = 0.0;
        scale_[a] = 1.0;
    }
    copies_ = 1;
    wheel_heat_ = 0.0;
    wheel_idle_ = 0.0;
    wheel_last_sign_ = 0.0;
    preview_ = ClipboardPreview{};
}

void Clipboard::hold(Clip clip, i64 anchor_x, i64 anchor_y, i64 anchor_z) {
    source_ = std::move(clip);
    anchor_[0] = anchor_x;
    anchor_[1] = anchor_y;
    anchor_[2] = anchor_z;
    for (u32 a = 0; a < 3; ++a) {
        offset_[a] = 0;
        angles_[a] = 0.0;
        scale_[a] = 1.0;
    }

    copies_ = 1;
    rebake();
    refresh_preview();
}

void Clipboard::set_offset(i64 dx, i64 dy, i64 dz) {
    offset_[0] = dx;
    offset_[1] = dy;
    offset_[2] = dz;
    apply_snap();
    refresh_preview();
}

void Clipboard::set_copies(i32 copies) {
    // No upper limit. Zero is the one value it cannot hold - no copies at all is what
    // dropping the clip is for.
    copies_ = (copies == 0) ? 1 : copies;
    rebake();
    refresh_preview();
}

void Clipboard::turn(u32 axis, f64 degrees) {
    if (source_.empty() || axis > 2) return;
    angles_[axis] = std::clamp(angles_[axis] + degrees, -kMaxTurnDegrees, kMaxTurnDegrees);
    rebake();
    apply_snap();
    refresh_preview();
}

void Clipboard::resize(f64 factor) {
    if (source_.empty() || factor <= 0.0) return;

    for (u32 a = 0; a < 3; ++a) stretch_axis(a, factor);
    rebake();
    apply_snap();
    refresh_preview();
}

void Clipboard::stretch(u32 axis, f64 factor) {
    if (source_.empty() || axis > 2 || factor <= 0.0) return;
    stretch_axis(axis, factor);
    rebake();
    apply_snap();
    refresh_preview();
}

void Clipboard::stretch_axis(u32 axis, f64 factor) {
    if (source_.empty() || axis > 2) return;
    // The only limit is a single voxel on the axis. There is no ceiling: what stops a resize
    // is the machine failing to allocate it, which is caught and reported.
    const f64 minimum = 1.0 / std::max(1.0, static_cast<f64>(source_.size[axis]));
    scale_[axis] = std::max(scale_[axis] * factor, minimum);
}

u32 Clipboard::instance_count() const {
    if (copies_ >= 0) return static_cast<u32>(std::max(1, copies_));
    // The ghost is the first step; the rest carry on past it.
    return static_cast<u32>(-copies_) + 1u;
}

f64 Clipboard::share(u32 n) const {
    if (copies_ >= 0) {
        // Copy n of count carries (n+1)/count of the transform, so the last one carries all
        // of it and the ghost you are steering is the one you set.
        const u32 count = static_cast<u32>(std::max(1, copies_));
        return static_cast<f64>(std::min(n + 1, count)) / static_cast<f64>(count);
    }
    // Going the other way, the ghost is one whole step and each copy after it is another.
    return static_cast<f64>(n + 1);
}

namespace {

// Turns a vector by the same Euler angles, in the same order, that rotate_clip applies to a
// clip: x, then y, then z. The row of extrapolated copies has to bend the way the clip
// turns, and it can only do that if both use the same convention.
void rotate_vector(const f64 degrees[3], const f64 in[3], f64 out[3]) {
    f64 v[3] = {in[0], in[1], in[2]};
    for (u32 axis = 0; axis < 3; ++axis) {
        const f64 a = degrees[axis] * kDegreesToRadians;
        if (std::abs(a) < 1e-12) continue;
        const u32 u = (axis + 1) % 3;
        const u32 w = (axis + 2) % 3;
        const f64 c = std::cos(a);
        const f64 s = std::sin(a);
        const f64 vu = v[u];
        const f64 vw = v[w];
        v[u] = vu * c - vw * s;
        v[w] = vu * s + vw * c;
    }
    out[0] = v[0];
    out[1] = v[1];
    out[2] = v[2];
}

}  // namespace

void Clipboard::rebake() {
    ++revision_;
    shapes_.clear();
    instance_shape_.clear();
    baked_cells_ = 0;
    baking_truncated_ = false;
    if (source_.empty()) return;

    const u64 started = now_ns();
    const bool scaled = std::abs(scale_[0] - 1.0) > 1e-9 || std::abs(scale_[1] - 1.0) > 1e-9 ||
                        std::abs(scale_[2] - 1.0) > 1e-9;
    const bool transformed =
        scaled || angles_[0] != 0.0 || angles_[1] != 0.0 || angles_[2] != 0.0;

    if (!transformed) {
        // Every copy is the same clip. One bake, one upload, one shape — which is the case
        // almost every paste is, so it is worth not being clever about.
        shapes_.push_back(source_);
        drawn_instances_.clear();
        for (u32 n = 0; n < instance_count() && drawn_instances_.size() < kMaxPreviewInstances;
             ++n) {
            drawn_instances_.push_back(n);
        }
        if (instance_count() > kMaxPreviewInstances) {
            drawn_instances_.back() = instance_count() - 1;
        }
        instance_shape_.assign(drawn_instances_.size(), 0);
        baked_cells_ = source_.cell_count();
        last_bake_ms_ = ns_to_ms(now_ns() - started);
        return;
    }

    const u32 count = instance_count();
    // Only the copies that will be *drawn* are baked and kept. There can be any number of
    // them; a thousand copies do not mean a thousand clips sitting in memory, and stamping
    // bakes each one in turn instead of reading this list.
    drawn_instances_.clear();
    for (u32 n = 0; n < count && drawn_instances_.size() < kMaxPreviewInstances; ++n) {
        drawn_instances_.push_back(n);
    }
    // Whatever else is drawn, the copy being steered has to be, or the tool has no cursor.
    if (count > kMaxPreviewInstances) drawn_instances_.back() = count - 1;

    instance_shape_.assign(drawn_instances_.size(), 0);
    for (usize slot = 0; slot < drawn_instances_.size(); ++slot) {
        Clip baked;
        if (!bake_instance(drawn_instances_[slot], baked)) {
            // The machine could not find room for it. Everything from here reuses the last
            // shape that did fit, and the tool says so rather than falling over.
            baking_truncated_ = true;
            const u32 fallback = shapes_.empty() ? 0u : static_cast<u32>(shapes_.size() - 1);
            for (usize rest = slot; rest < instance_shape_.size(); ++rest) {
                instance_shape_[rest] = fallback;
            }
            break;
        }
        baked_cells_ += baked.cell_count();
        instance_shape_[slot] = static_cast<u32>(shapes_.size());
        shapes_.push_back(std::move(baked));
    }
    if (shapes_.empty()) {
        shapes_.push_back(source_);
        baked_cells_ = source_.cell_count();
        for (usize slot = 0; slot < instance_shape_.size(); ++slot) instance_shape_[slot] = 0;
    }
    last_bake_ms_ = ns_to_ms(now_ns() - started);
}

bool Clipboard::bake_instance(u32 n, Clip& out) const {
    const f64 fraction = share(n);
    f64 radians[3];
    f64 factor[3];
    for (u32 a = 0; a < 3; ++a) {
        radians[a] = angles_[a] * fraction * kDegreesToRadians;
        // Geometric along the ramp, not linear: half way to twice the size is 1.41×, not
        // 1.5×, which is what makes a row of them read as an even progression.
        factor[a] = std::pow(scale_[a], fraction);
    }

    // Always from the source, never from the previous copy: transforms compound their error,
    // and a long row would end with the last one visibly chewed.
    //
    // Turn first, then resize. The other order runs three shear passes over the *enlarged*
    // clip, so growing three times costs twenty-seven times as much shearing for exactly the
    // same result.
    //
    // There is no size limit, so the thing that stops a resize is the machine failing to
    // find the memory. That is caught here and reported, rather than being pre-empted by a
    // cap that stops you doing something the machine could have managed.
    try {
        Clip baked = rotate_clip(source_, radians);
        out = scale_clip(baked, factor);
        // Hollowed here, in the bake both the ghost and the stamp go through, so the preview
        // shows the shell that will actually be placed. Doing it only at stamp time meant the
        // ghost was solid and the shell appeared out of nowhere on release — a preview that
        // does not show what the tool will do is worse than none.
        //
        // After the turn and the resize, so the shell follows the copy as it is really shaped.
        if (hollow_ > 0) out = hollow_clip(out, static_cast<i64>(hollow_));
    } catch (const std::bad_alloc&) {
        return false;
    }
    return true;
}

void Clipboard::apply_snap() {
    if (!snap_ || shapes_.empty()) return;
    // Snapped means the copies tile: the offset is rounded to whole clip lengths, so a
    // repeated piece meets its neighbour exactly instead of nearly. Measured against the
    // untransformed clip, because that is the piece the player selected.
    for (u32 a = 0; a < 3; ++a) {
        offset_[a] = snap_to(offset_[a], static_cast<i64>(source_.size[a]));
    }
}

void Clipboard::instance_origin(u32 n, i64 out[3]) const {
    const u32 count = instance_count();
    if (copies_ >= 0) {
        // Evenly between the original and the ghost, in a straight line. Kept linear rather
        // than composed so the last copy lands exactly where the ghost is however much the
        // row is turning — aiming the ghost has to aim the row.
        const u32 index = std::min(n + 1, count);
        for (u32 a = 0; a < 3; ++a) {
            const f64 fraction =
                static_cast<f64>(offset_[a]) * static_cast<f64>(index) / static_cast<f64>(count);
            out[a] = anchor_[a] + static_cast<i64>(std::llround(fraction));
        }
        return;
    }

    // Going the other way, the whole transform is **one step**, and each copy is that step
    // taken again from where the last one ended. Adding a copy does not squeeze another one
    // into the same span - it puts a new one on the far end, so the row reaches further every
    // time you add to it. That is the difference between the two directions: positive fills a
    // gap you have set the ends of, negative extends a pattern you have set the stride of.
    //
    // Every step is turned by however much the clip has turned by the time it is taken, so a
    // move with no rotation in it repeats in a straight line, and a move that also turns walks
    // the row round a curve. Nothing is rotated about a pivot; each copy is one more step on.
    //
    // The **last** copy is the one drawn with an outline. It is the far end of the row, the
    // one that moves most as the step changes, and therefore the one you watch.
    const f64 step[3] = {static_cast<f64>(offset_[0]), static_cast<f64>(offset_[1]),
                         static_cast<f64>(offset_[2])};
    f64 at[3] = {0.0, 0.0, 0.0};
    for (u32 i = 0; i <= n; ++i) {
        const f64 turned[3] = {angles_[0] * static_cast<f64>(i), angles_[1] * static_cast<f64>(i),
                               angles_[2] * static_cast<f64>(i)};
        f64 leg[3];
        rotate_vector(turned, step, leg);
        for (u32 a = 0; a < 3; ++a) at[a] += leg[a];
    }
    for (u32 a = 0; a < 3; ++a) {
        out[a] = anchor_[a] + static_cast<i64>(std::llround(at[a]));
    }
}

void Clipboard::refresh_preview() {
    preview_ = ClipboardPreview{};
    if (source_.empty() || shapes_.empty()) return;
    preview_.holding = true;
    preview_.total = instance_count();
    preview_.instances = static_cast<u32>(drawn_instances_.size());
    for (u32 slot = 0; slot < preview_.instances; ++slot) {
        i64 at[3];
        instance_origin(drawn_instances_[slot], at);
        const u32 shape = (slot < instance_shape_.size()) ? instance_shape_[slot] : 0u;
        preview_.shape[slot] = shape;
        const Clip& used = (shape < shapes_.size()) ? shapes_[shape] : source_;
        for (u32 a = 0; a < 3; ++a) {
            preview_.min[slot][a] = at[a];
            preview_.max[slot][a] = at[a] + used.size[a] - 1;
        }
    }
}

bool Clipboard::update(const World& world, const ClipboardInput& input, const f64 origin[3],
                       const f64 direction[3], f64 dt, u64 tick, u32 player,
                       std::vector<Op>& out) {
    preview_ = ClipboardPreview{};

    if (input.toggle_paste_mode) {
        paste_mode_ = static_cast<PasteMode>((static_cast<u32>(paste_mode_) + 1) %
                                             static_cast<u32>(PasteMode::Count));
    }
    if (input.cycle_adjust) {
        adjust_mode_ = static_cast<AdjustMode>((static_cast<u32>(adjust_mode_) + 1) %
                                               static_cast<u32>(AdjustMode::Count));
    }
    if (input.toggle_snap) {
        snap_ = !snap_;
        apply_snap();
    }

    const bool right_pressed = input.right && !prev_right_;
    const bool middle_pressed = input.middle && !prev_middle_;

    // ---- no clip yet: the selection drag, which is the chisel ------------------------
    if (source_.empty()) {
        ChiselInput drag{};
        drag.left = input.left;
        drag.middle = input.middle;
        drag.wheel = input.wheel;
        drag.adjust_distance = input.adjust_distance;
        drag.clear_points = input.clear_points;
        // The right button abandons a selection in progress, the same way it abandons a
        // ghost — one button that means "not this" wherever the tool happens to be.
        drag.cancel = input.cancel || right_pressed;

        Op box;
        const bool finished = selector_.update(world, drag, origin, direction, tick, player, box);
        const ChiselPreview& drag_preview = selector_.preview();
        preview_.selecting = drag_preview.active;
        for (u32 a = 0; a < 3; ++a) {
            preview_.select_min[a] = drag_preview.min[a];
            preview_.select_max[a] = drag_preview.max[a];
        }
        prev_right_ = input.right;
        prev_middle_ = input.middle;
        if (!finished) return false;

        box.normalise();
        // No size limit on a selection; what stops it is the machine having no room, which
        // is caught rather than allowed to take the game down.
        try {
            source_ = capture_clip(world, box.x0, box.y0, box.z0, box.x1, box.y1, box.z1);
        } catch (const std::bad_alloc&) {
            source_ = Clip{};
            preview_.too_large = true;
            return false;
        }
        anchor_[0] = box.x0;
        anchor_[1] = box.y0;
        anchor_[2] = box.z0;
        for (u32 a = 0; a < 3; ++a) {
            offset_[a] = 0;
            angles_[a] = 0.0;
            scale_[a] = 1.0;
        }
        copies_ = 1;
        rebake();
        prev_left_ = input.left;   // the release that ended the drag must not also stamp
        return false;
    }

    // ---- holding a clip ---------------------------------------------------------------
    prev_right_ = input.right;
    prev_middle_ = input.middle;

    if (input.cancel || right_pressed || input.clear_points) {
        drop();
        return false;
    }

    // Every press this frame is accumulated and baked once: baking is proportional to
    // volume, and a held key repeats several times on a slow frame.
    const f64 step = turn_step_degrees();
    const i32 vertical =
        static_cast<i32>(input.turn_up) - static_cast<i32>(input.turn_down);
    const i32 taps = static_cast<i32>(input.increase) - static_cast<i32>(input.decrease);
    const bool resizing = adjust_mode_ == AdjustMode::Resize;

    f64 turned[3] = {0.0, 0.0, 0.0};
    turned[1] += step * (static_cast<f64>(input.turn_right) - static_cast<f64>(input.turn_left));
    // Up and down turn the clip, except in resize, where they stretch it along whichever
    // axis you are facing.
    if (!resizing) turned[0] += step * static_cast<f64>(vertical);

    bool shapes_changed = false;
    if (resizing) {
        if (taps != 0) {
            const f64 factor = std::pow(kResizeStep, static_cast<f64>(taps));
            for (u32 a = 0; a < 3; ++a) stretch_axis(a, factor);
            shapes_changed = true;
        }
        if (vertical != 0) {
            u32 axis = 0;
            i32 sign = 1;
            dominant_axis(direction, axis, sign);
            stretch_axis(axis, std::pow(kResizeStep, static_cast<f64>(vertical)));
            shapes_changed = true;
        }
    } else if (taps != 0) {
        // The count runs from one side of the ghost to the other, and zero is not a value
        // it can hold — no copies at all is what dropping the clip is for. So the counting
        // is done on a line with zero taken out of it: ... -3, -2, -1, 1, 2, 3 ...
        const i32 rank = (copies_ > 0) ? copies_ : copies_ + 1;
        const i32 moved = rank + taps;
        const i32 wanted = (moved >= 1) ? moved : moved - 1;
        if (wanted != copies_) {
            copies_ = wanted;
            shapes_changed = true;
        }
    }

    if (turned[0] != 0.0 || turned[1] != 0.0 || turned[2] != 0.0) {
        // Deliberately *not* wrapped at 360. The copies carry fractions of this angle, so
        // wrapping it snaps every one of them back to the start at once — a row winding
        // smoothly round and then jumping. Past a full turn the row keeps winding, which is
        // how a spiral of more than one revolution gets made.
        for (u32 a = 0; a < 3; ++a) {
            angles_[a] = std::clamp(angles_[a] + turned[a], -kMaxTurnDegrees, kMaxTurnDegrees);
        }
        shapes_changed = true;
    }
    if (shapes_changed) {
        rebake();
        apply_snap();
    }

    // Middle click drops the ghost on whatever you are looking at, which beats scrolling
    // it across a room one voxel at a time.
    if (middle_pressed) {
        const RayHit hit = raycast(world, origin[0], origin[1], origin[2], direction[0],
                                   direction[1], direction[2], selector_.reach());
        if (hit.hit) {
            const Clip& last = shapes_.empty() ? source_ : shapes_.back();
            // Centred on the aimed voxel horizontally and resting on the face that was hit,
            // so it lands where it looks like it will rather than hanging off a corner.
            const i64 target[3] = {hit.place_x(), hit.place_y(), hit.place_z()};
            for (u32 a = 0; a < 3; ++a) {
                const i64 normal = (a == 0) ? hit.nx : ((a == 1) ? hit.ny : hit.nz);
                const i64 centred =
                    (normal != 0) ? ((normal > 0) ? 0 : -(last.size[a] - 1)) : -(last.size[a] / 2);
                offset_[a] = target[a] + centred - anchor_[a];
            }
            apply_snap();
        }
    }

    if (input.wheel != 0.0f) {
        u32 axis = 0;
        i32 sign = 1;
        dominant_axis(direction, axis, sign);

        // Keep winding the same way and it speeds up; stop, reverse, or turn to face a
        // different axis and it is back to one voxel a click. Without this, moving a ghost
        // thirty metres is a thousand clicks; with a flat large step, lining it up to the
        // voxel is impossible.
        //
        // The step is taken from the heat *before* this click is counted, so the first
        // click of a run always moves exactly one voxel however many notches the wheel
        // reports at once. Acceleration you cannot escape is worse than none.
        const f64 wheel_sign = (input.wheel > 0.0f) ? 1.0 : -1.0;
        const f64 direction_key = wheel_sign * static_cast<f64>(axis + 1);
        if (direction_key != wheel_last_sign_) wheel_heat_ = 0.0;
        wheel_last_sign_ = direction_key;
        wheel_idle_ = 0.0;
        const f64 heat_before = wheel_heat_;
        wheel_heat_ = std::min(wheel_heat_ + std::abs(static_cast<f64>(input.wheel)), 60.0);
        const i64 accelerated = static_cast<i64>(1.0 + heat_before * heat_before * 0.02);

        const i64 length = std::max<i64>(1, source_.size[axis]);
        const i64 step_voxels = input.big_step ? length : (snap_ ? length : accelerated);
        offset_[axis] += static_cast<i64>(input.wheel) * sign * step_voxels;
        apply_snap();
    } else {
        // A wheel arrives as separate notches with idle frames between them, so the run has
        // to survive those gaps — that is what a "continuous" scroll actually looks like.
        // The speed holds for a quarter second after the last notch, then falls away.
        constexpr f64 kHoldSeconds = 0.25;
        wheel_idle_ += dt;
        if (wheel_idle_ > kHoldSeconds) {
            wheel_heat_ = std::max(0.0, wheel_heat_ - (dt * 90.0));
            if (wheel_heat_ == 0.0) wheel_last_sign_ = 0.0;
        }
    }

    refresh_preview();

    const bool pressed = input.left && !prev_left_;
    prev_left_ = input.left;
    if (!pressed) return false;

    // Commit. Every copy goes into one list, so the whole stamp is a single undo step.
    //
    // Each copy is baked here and let go again rather than read from the preview's list.
    // That is what makes the number of copies unlimited: putting a thousand of them down
    // costs one clip of memory at a time, not a thousand, and the ones that were never drawn
    // are stamped just as exactly as the ones that were.
    out.clear();
    const u32 count = instance_count();
    for (u32 n = 0; n < count; ++n) {
        i64 at[3];
        instance_origin(n, at);
        Clip baked;
        if (!bake_instance(n, baked)) {
            // Out of memory part way through. Everything already gathered still applies as
            // one action; the rest is dropped and said so, which beats a half-stamped row
            // and a crash.
            baking_truncated_ = true;
            break;
        }
        // Already hollowed by bake_instance, so what is stamped is exactly what the ghost drew.
        clip_to_ops(baked, at[0], at[1], at[2], paste_mode_, tick, player, out);
    }
    last_stamp_ops_ = out.size();
    return !out.empty();
}

}  // namespace ws
