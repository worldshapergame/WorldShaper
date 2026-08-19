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
// X drops a constraint point. The box has to reach it, so the shape grows to touch it at its
// edge — the way to make a cut line up with something without eyeballing it. On a key rather
// than middle click because dropping a point is something you do *while* dragging a box out
// with a mouse button already held, and a hand cannot press two mouse buttons apart cleanly.
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

// Splits a filled box into the six slabs of a shell `thickness` voxels thick, leaving the
// inside untouched.
//
// Untouched, not emptied. A hollow placement should build walls inside a hill without
// scooping out the hill, and a hollow carve should cut a shell of rock away and leave what was
// behind it — carving the interior as well would make "hollow" mean "solid" for anything that
// removes matter, which is exactly backwards.
//
// The slabs do not overlap: two ops writing the same voxel would count it twice in the matter
// ledger, and the ledger is checked against a full recount.
//
// A box too thin to have an inside is emitted whole, because a one-voxel wall with a one-voxel
// shell is a one-voxel wall.
void hollow_box(const Op& box, i64 thickness, u64& next_id, std::vector<Op>& out);

// ---- R11h: what an edit is about to touch, and whether anything has ever sampled it -----------
//
// The one box every op in a group falls inside, in world voxels. `false` when the group is empty,
// which is not the same as a box of nought volume and must not read as one.
//
// A group, not an op, because that is what the tool produces: `hollow_box` turns one carve into six
// slabs and the clipboard produces a run of them. The volume that has to be sampled before the cut
// is the union of all of them, and asking six times would sample the middle of a hollow shell that
// nothing is going to write to.
bool edit_bounds(const std::vector<Op>& ops, i64 low[3], i64 high[3]);

// The proximity radius, in metres, and it is the SAME twenty as `NodePoolBudget::proximity_voxels`.
//
// R2c and D199: twenty metres is held at full detail regardless of visibility, because collision,
// physics and editing all touch what is behind you and under your feet, and none of them can be
// served by what a pixel happened to ask for. The radius has held RESIDENCY since D270 — what the
// node pool keeps built out of what the world already holds — and R11h is the other half of the
// same sentence: after R11d the world does not hold real voxels everywhere, so a radius that
// guarantees residency guarantees residency of a coarse approximation.
inline constexpr f64 kProximityMetres = 20.0;

// Is this edit outside the radius, so that nothing guarantees the world holds it at full detail?
//
// Measured from the camera to the box, nought inside it, in VOXELS — `Camera::position_*` is in
// voxel doubles (see game/camera.hpp) and scaling it again is the mistake that put a scripted
// chisel a kilometre away from the facade it was aimed at.
//
// This is a question about the CAMERA and not about the world, deliberately. "Has this volume been
// sampled finely?" is answerable and is asked separately by the ladder's own bookkeeping; this one
// says whether the twenty-metre guarantee is even claiming to cover the edit, which is what decides
// whether the edit has to go and get its own voxels first.
bool edit_beyond_proximity(const i64 low[3], const i64 high[3], const f64 camera_voxel[3],
                           f64 radius_voxels);

struct ChiselInput {
    bool left = false;
    bool right = false;
    bool add_point = false;         // X, pressed this frame
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
    ChiselMode mode = ChiselMode::None;
    i64 min[3]{};
    i64 max[3]{};
    u64 volume = 0;

    // Where the crosshair is pointing right now, on its own.
    //
    // Separate from the box because the two stop being the same thing the moment a drag starts:
    // the box is anchored where the button went down and the cursor has moved on, and the marker
    // that says "here" has to follow the second. Reported even when there is no preview to draw —
    // aiming at the sky is still aiming somewhere.
    bool has_cursor = false;
    i64 cursor[3]{};

    // What a carve is about to remove, sampled where the drag was anchored. The preview is
    // drawn in this colour where it is buried, so a shape inside the ground reads as the
    // material that is going to leave. kAir when the box starts in open space.
    VoxelTypeId removing = kAir;
};

// There is no cap on how large an edit may be, and that is deliberate. There was one, and
// it also capped what the clipboard could select — selecting *is* the chisel — so a limit
// meant for a carve was quietly refusing to copy a large building.
//
// What it costs is reported rather than prevented. An edit happens in one frame, at roughly
// six nanoseconds a voxel through geometry that is not uniform, so a very large one is a
// pause of that length on the frame the button comes up. The developer panel shows how long
// the last one took; undo and the matter ledger cope with any size.
//
// Slicing an edit across frames is what would make a huge one smooth, and that has to happen
// before Stage 16 regardless — an edit this size is more than a network tick can carry. It
// is a better answer than a number that says no.

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

    // O. Off (the default), the tool acts on the voxel under the crosshair — the one the cursor
    // marker rings. On, it acts on the empty voxel against the face you are looking at, so a
    // placement lands *on* a surface rather than replacing it.
    //
    // It applies to carving as well as to placing. A setting that says where the tool acts and
    // then only half applies is one nobody can predict from.
    bool places_against_face() const { return against_face_; }

    const ChiselPreview& preview() const { return preview_; }

    // Where the crosshair is pointing, resolved the way this tool resolves it — honouring the
    // distance override and the against-face toggle.
    //
    // Public because the cursor marker is drawn whatever tool is in hand, including while a
    // clipboard ghost is being steered and this tool is not being updated at all. A second
    // implementation of the same question would drift from this one the first time either the
    // distance rule or the face rule changed, and the mark would then be a voxel away from where
    // the edit lands — which is worse than no mark.
    bool cursor(const World& world, const f64 origin[3], const f64 direction[3],
                i64 out[3]) const {
        return resolve_point(world, origin, direction, out);
    }

    const std::vector<std::array<i64, 3>>& constraints() const { return constraints_; }
    void clear_constraints() { constraints_.clear(); }

private:
    bool resolve_point(const World& world, const f64 origin[3], const f64 direction[3],
                       i64 out[3]) const;

    VoxelTypeId material_ = 1;
    f64 distance_ = 0.0;        // starts in snap mode, which is what a new player expects
    f64 reach_ = 4096.0;        // 128 metres
    ChiselMode mode_ = ChiselMode::None;
    bool overwrite_ = true;
    // O, and it starts OFF: the tool acts on the voxel under the crosshair rather than on the
    // empty one against its face.
    //
    // It defaulted the other way, which is the convention a block game teaches -- you point at a
    // surface and the block lands on top of it. This is not that game. Here the crosshair is on a
    // 3 cm voxel, the cursor marker draws a ring around exactly the one it means, and having the
    // tool act on a DIFFERENT voxel from the one being marked is a contradiction the player has to
    // hold in their head on every stroke. Acting where you are pointing is the answer that needs
    // no explaining; O is there for when a placement genuinely has to go in front of a surface.
    bool against_face_ = false;
    bool dragging_ = false;
    i64 anchor_[3]{};
    std::vector<std::array<i64, 3>> constraints_;
    ChiselPreview preview_;
    bool prev_left_ = false;
    bool prev_right_ = false;
    bool prev_middle_ = false;
};

}  // namespace ws
