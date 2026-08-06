#pragma once
// The chisel — the tool that carves and places (answer O5, roadmap Stage 5).
//
// Hold left to carve or right to place: the press fixes one corner, the release fixes the
// other, and the box between them is the edit. Both corners are placed at a *distance from
// the camera* rather than on a surface, which is what lets you cut a shaft through open air
// or square off a cliff you are standing away from. Scroll with the modifier held to change
// that distance; wind it all the way down to zero and it stops being a distance at all and
// snaps to whatever voxel you are looking at.
//
// Middle click drops a constraint point. The box has to reach it, so the shape grows to
// touch it at its edge — the way to make a cut line up with something without eyeballing it.
//
// Boxes only for now. The tool is deliberately one shape done properly rather than five
// half-done; spheres, cylinders and lines come with the shape library later.

#include <array>
#include <vector>

#include "core/types.hpp"
#include "world/op.hpp"
#include "world/voxel_type.hpp"

namespace ws {

class World;

enum class ChiselMode : u8 { None, Carve, Place };

struct ChiselInput {
    bool left = false;
    bool right = false;
    bool middle = false;
    f32 wheel = 0.0f;
    bool adjust_distance = false;   // the modifier that hands the wheel to the chisel
    bool clear_points = false;      // pressed this frame
    bool cancel = false;            // abandon the drag without editing
    bool toggle_overwrite = false;  // P, pressed this frame
    bool toggle_anchor = false;     // O, pressed this frame
};

struct ChiselPreview {
    bool active = false;
    bool dragging = false;
    bool too_large = false;
    ChiselMode mode = ChiselMode::None;
    i64 min[3]{};
    i64 max[3]{};
    u64 volume = 0;

    // What a carve is about to remove, sampled where the drag was anchored. The preview is
    // drawn in this colour where it is buried, so a shape inside the ground reads as the
    // material that is going to leave. kAir when the box starts in open space.
    VoxelTypeId removing = kAir;
};

// A single edit is capped, because it happens in one frame and there is no way to take it
// back once it has started. The number comes from measurement rather than taste: an edit
// costs roughly six nanoseconds a voxel through geometry that is not uniform, so eight
// million is about fifty milliseconds — a visible hitch on release, and nothing worse.
// A 200-voxel cube, six and a quarter metres on a side.
//
// Raising it means slicing the work across frames first. That has to happen anyway before
// Stage 16, since an edit this size is also more than a network tick can carry.
inline constexpr u64 kMaxEditVoxels = 8ull * 1024ull * 1024ull;

class Chisel {
public:
    void set_material(VoxelTypeId type) { material_ = type; }
    VoxelTypeId material() const { return material_; }

    // 0 means "snap to the voxel under the crosshair" rather than a distance.
    f64 distance() const { return distance_; }
    bool snapping() const { return distance_ < 1.0; }
    f64 reach() const { return reach_; }

    // Advances the tool by one frame. Returns true, and fills `out`, when a drag has just
    // finished and there is an edit to apply.
    bool update(const World& world, const ChiselInput& input, const f64 origin[3],
                const f64 direction[3], u64 tick, u32 player, Op& out);

    // P. On, placing replaces whatever is in the box. Off, it only fills the empty parts,
    // so you can drop a beam through a wall without eating the wall.
    bool overwrites() const { return overwrite_; }

    // O. On (the default), placing puts the first point on the empty voxel against the face
    // you are looking at — a block lands *on* the surface. Off, it puts it on the surface
    // voxel itself, so the placement replaces what you aimed at. Carving always takes the
    // voxel you aimed at; carving the air in front of it is not a thing anyone wants.
    bool places_against_face() const { return against_face_; }

    const ChiselPreview& preview() const { return preview_; }
    const std::vector<std::array<i64, 3>>& constraints() const { return constraints_; }
    void clear_constraints() { constraints_.clear(); }

private:
    bool resolve_point(const World& world, ChiselMode mode, const f64 origin[3],
                       const f64 direction[3], i64 out[3]) const;

    VoxelTypeId material_ = 1;
    f64 distance_ = 0.0;        // starts in snap mode, which is what a new player expects
    f64 reach_ = 4096.0;        // 128 metres
    ChiselMode mode_ = ChiselMode::None;
    bool overwrite_ = true;
    bool against_face_ = true;
    bool dragging_ = false;
    i64 anchor_[3]{};
    std::vector<std::array<i64, 3>> constraints_;
    ChiselPreview preview_;
    bool prev_left_ = false;
    bool prev_right_ = false;
    bool prev_middle_ = false;
};

}  // namespace ws
