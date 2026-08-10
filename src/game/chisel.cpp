#include "game/chisel.hpp"

#include <algorithm>
#include <cmath>

#include "world/raycast.hpp"
#include "world/world.hpp"

namespace ws {

void hollow_box(const Op& box, i64 thickness, u64& next_id, std::vector<Op>& out) {
    Op b = box;
    b.normalise();

    const i64 span[3] = {b.x1 - b.x0 + 1, b.y1 - b.y0 + 1, b.z1 - b.z0 + 1};
    if (thickness <= 0 || span[0] <= 2 * thickness || span[1] <= 2 * thickness ||
        span[2] <= 2 * thickness) {
        // No inside to leave alone, so the shell is the whole thing.
        Op whole = b;
        whole.tick = next_id++;
        out.push_back(whole);
        return;
    }

    const i64 t = thickness;
    auto slab = [&](i64 x0, i64 y0, i64 z0, i64 x1, i64 y1, i64 z1) {
        Op piece = b;
        piece.tick = next_id++;
        piece.x0 = x0; piece.y0 = y0; piece.z0 = z0;
        piece.x1 = x1; piece.y1 = y1; piece.z1 = z1;
        out.push_back(piece);
    };

    // Six slabs, cut so no two share a voxel. Overlapping them would be simpler to write and
    // would count the shared voxels twice in the matter ledger, which is audited against a
    // full recount and would fail.
    slab(b.x0, b.y0, b.z0, b.x1, b.y0 + t - 1, b.z1);                        // bottom
    slab(b.x0, b.y1 - t + 1, b.z0, b.x1, b.y1, b.z1);                        // top

    const i64 my0 = b.y0 + t;
    const i64 my1 = b.y1 - t;
    slab(b.x0, my0, b.z0, b.x1, my1, b.z0 + t - 1);                          // front
    slab(b.x0, my0, b.z1 - t + 1, b.x1, my1, b.z1);                          // back

    const i64 mz0 = b.z0 + t;
    const i64 mz1 = b.z1 - t;
    slab(b.x0, my0, mz0, b.x0 + t - 1, my1, mz1);                            // left
    slab(b.x1 - t + 1, my0, mz0, b.x1, my1, mz1);                            // right
}
namespace {

i64 floor_i64(f64 v) { return static_cast<i64>(std::floor(v)); }

u64 box_volume(const i64 lo[3], const i64 hi[3]) {
    const u64 dx = static_cast<u64>(hi[0] - lo[0]) + 1;
    const u64 dy = static_cast<u64>(hi[1] - lo[1]) + 1;
    const u64 dz = static_cast<u64>(hi[2] - lo[2]) + 1;
    return dx * dy * dz;
}

}  // namespace

bool Chisel::resolve_point(const World& world, const f64 origin[3], const f64 direction[3],
                           i64 out[3]) const {
    if (distance_ >= 1.0) {
        for (int a = 0; a < 3; ++a) out[a] = floor_i64(origin[a] + direction[a] * distance_);
        return true;
    }

    const RayHit hit = raycast(world, origin[0], origin[1], origin[2], direction[0],
                               direction[1], direction[2], reach_);
    if (!hit.hit) return false;   // snapping at the sky has nothing to snap to

    // O decides where the tool acts, and it decides it for BOTH modes -- which is why there is no
    // longer a mode parameter to get wrong.
    //
    // It used to be consulted for placing only, with carving always taking the voxel aimed at, on
    // the argument that "carving the air in front of it is not a thing anyone wants". That is true
    // of the air and false of the toggle: a setting that says where the tool acts and then applies
    // to half of it is a setting nobody can predict from, and the two modes then disagreed about
    // which voxel the crosshair means -- including in the marker that now draws it.
    if (against_face_) {
        out[0] = hit.place_x();
        out[1] = hit.place_y();
        out[2] = hit.place_z();
    } else {
        out[0] = hit.x;
        out[1] = hit.y;
        out[2] = hit.z;
    }
    return true;
}

bool Chisel::update(const World& world, const ChiselInput& input, const f64 origin[3],
                    const f64 direction[3], u64 tick, u32 player, Op& out) {
    // The wheel only reaches the chisel while the modifier is held; otherwise it belongs to
    // whatever else wants it. The step scales with the distance so that a metre away it
    // moves in voxels and a hundred metres away it does not take a minute to cross the gap.
    if (input.adjust_distance && input.wheel != 0.0f) {
        const f64 step = std::max(1.0, std::floor(distance_ * 0.0625));
        distance_ = std::clamp(distance_ + static_cast<f64>(input.wheel) * step, 0.0, reach_);
    }

    if (input.toggle_overwrite) overwrite_ = !overwrite_;
    if (input.toggle_anchor) against_face_ = !against_face_;
    if (input.clear_points) constraints_.clear();
    if (input.cancel && dragging_) {
        dragging_ = false;
        mode_ = ChiselMode::None;
    }

    const bool left_pressed = input.left && !prev_left_;
    const bool right_pressed = input.right && !prev_right_;
    const bool released = dragging_ && ((mode_ == ChiselMode::Carve && !input.left) ||
                                        (mode_ == ChiselMode::Place && !input.right));
    prev_left_ = input.left;
    prev_right_ = input.right;

    if (!dragging_ && (left_pressed || right_pressed)) {
        const ChiselMode mode = left_pressed ? ChiselMode::Carve : ChiselMode::Place;
        i64 point[3];
        if (resolve_point(world, origin, direction, point)) {
            mode_ = mode;
            dragging_ = true;
            anchor_[0] = point[0];
            anchor_[1] = point[1];
            anchor_[2] = point[2];
        }
    }

    if (input.add_point) {
        // A constraint lands wherever the tool would act, which is what O decides -- so a point
        // dropped mid-place lands against the face, exactly where the matter is going.
        //
        // Held, X repeats, so a line of points can be swept out with the mouse instead of tapped
        // out one at a time. That makes the same voxel arrive many frames running whenever the
        // crosshair is still, so a repeat that lands where the last one did is dropped: the list is
        // a set of places, and a place recorded fourteen times a second is fourteen times the cost
        // for nothing. Only the immediately previous one is compared, which is all a swept line can
        // produce and costs nothing to check -- crossing an earlier point deliberately still marks
        // it again, and that is harmless.
        i64 point[3];
        if (resolve_point(world, origin, direction, point) &&
            (constraints_.empty() || constraints_.back()[0] != point[0] ||
             constraints_.back()[1] != point[1] || constraints_.back()[2] != point[2])) {
            constraints_.push_back({point[0], point[1], point[2]});
        }
    }

    // Work out what the box would be right now, which is both the preview and -- if the
    // button came up this frame -- the edit.
    i64 cursor[3];
    const bool have_cursor = resolve_point(world, origin, direction, cursor);

    preview_ = ChiselPreview{};
    preview_.mode = dragging_ ? mode_ : ChiselMode::None;
    preview_.dragging = dragging_;

    // Recorded before the early return below, because the cursor marker is drawn whenever the
    // crosshair is on something — including mid-drag, where the box is anchored elsewhere, and
    // including when there is no preview box at all.
    preview_.has_cursor = have_cursor;
    if (have_cursor) {
        for (int a = 0; a < 3; ++a) preview_.cursor[a] = cursor[a];
    }

    if (!have_cursor && !dragging_) {
        return false;   // nothing aimed at and nothing in progress: no preview
    }

    i64 lo[3], hi[3];
    for (int a = 0; a < 3; ++a) {
        const i64 first = dragging_ ? anchor_[a] : cursor[a];
        const i64 second = have_cursor ? cursor[a] : first;
        lo[a] = std::min(first, second);
        hi[a] = std::max(first, second);
    }
    // The constraint points have to end up on the box, so the box grows to reach them.
    for (const std::array<i64, 3>& point : constraints_) {
        for (int a = 0; a < 3; ++a) {
            lo[a] = std::min(lo[a], point[a]);
            hi[a] = std::max(hi[a], point[a]);
        }
    }

    preview_.active = true;
    preview_.removing =
        dragging_ ? world.get(anchor_[0], anchor_[1], anchor_[2])
                  : (have_cursor ? world.get(cursor[0], cursor[1], cursor[2]) : kAir);
    for (int a = 0; a < 3; ++a) {
        preview_.min[a] = lo[a];
        preview_.max[a] = hi[a];
    }
    preview_.volume = box_volume(lo, hi);

    if (!released) return false;

    dragging_ = false;
    const ChiselMode finished = mode_;
    mode_ = ChiselMode::None;

    const bool carving = finished == ChiselMode::Carve;
    // Carving always clears the box; the overwrite toggle is about what placing does to
    // what is already there.
    const WriteMask mask =
        (carving || overwrite_) ? WriteMask::All : WriteMask::IntoAir;
    out = Op::fill_box(tick, player, lo[0], lo[1], lo[2], hi[0], hi[1], hi[2],
                       carving ? kAir : material_,
                       carving ? MatterReason::PlayerBreak : MatterReason::PlayerPlace, mask);
    constraints_.clear();
    return true;
}

}  // namespace ws
