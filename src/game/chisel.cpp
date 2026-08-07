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

bool Chisel::resolve_point(const World& world, ChiselMode mode, const f64 origin[3],
                           const f64 direction[3], i64 out[3]) const {
    if (distance_ >= 1.0) {
        for (int a = 0; a < 3; ++a) out[a] = floor_i64(origin[a] + direction[a] * distance_);
        return true;
    }

    const RayHit hit = raycast(world, origin[0], origin[1], origin[2], direction[0],
                               direction[1], direction[2], reach_);
    if (!hit.hit) return false;   // snapping at the sky has nothing to snap to

    // Carving wants the voxel itself; placing wants the empty one against the face, or the
    // new block would appear buried inside what you aimed at — unless O has turned that
    // off, in which case a placement lands on the surface voxel and replaces it.
    if (mode == ChiselMode::Place && against_face_) {
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
    const bool middle_pressed = input.middle && !prev_middle_;
    const bool released = dragging_ && ((mode_ == ChiselMode::Carve && !input.left) ||
                                        (mode_ == ChiselMode::Place && !input.right));
    prev_left_ = input.left;
    prev_right_ = input.right;
    prev_middle_ = input.middle;

    if (!dragging_ && (left_pressed || right_pressed)) {
        const ChiselMode mode = left_pressed ? ChiselMode::Carve : ChiselMode::Place;
        i64 point[3];
        if (resolve_point(world, mode, origin, direction, point)) {
            mode_ = mode;
            dragging_ = true;
            anchor_[0] = point[0];
            anchor_[1] = point[1];
            anchor_[2] = point[2];
        }
    }

    if (middle_pressed) {
        // Constraints resolve the same way the current action does, so a constraint dropped
        // during a place lands against a face rather than inside it.
        const ChiselMode mode = dragging_ ? mode_ : ChiselMode::Carve;
        i64 point[3];
        if (resolve_point(world, mode, origin, direction, point)) {
            constraints_.push_back({point[0], point[1], point[2]});
        }
    }

    // Work out what the box would be right now, which is both the preview and — if the
    // button came up this frame — the edit.
    const ChiselMode shown = dragging_ ? mode_ : ChiselMode::Carve;
    i64 cursor[3];
    const bool have_cursor = resolve_point(world, shown, origin, direction, cursor);

    preview_ = ChiselPreview{};
    preview_.mode = dragging_ ? mode_ : ChiselMode::None;
    preview_.dragging = dragging_;

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
