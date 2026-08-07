#pragma once
// Measuring a clip, so that "is this right?" has an answer other than squinting at it.
//
// A screenshot tells you a room looks plausible. It does not tell you the doorway is 2.05 m when
// you meant 2.00, that the two halves differ by three voxels, that the stair rise is 17.6 cm
// where building regulations and the eye both want 18, or that the volume of the pillar you
// copied sixteen times is not sixteen times the volume of one. Those are the errors that survive
// looking, and they are the ones that make a built space feel subtly wrong without anyone being
// able to say why.
//
// So everything here answers a question in numbers, in both voxels and metres, because a clip is
// authored in metres and stored in voxels and almost every mistake lives in the conversion.
//
// # What is measured
//
//   extent        the bounding box of the matter, which is rarely the box that was sampled
//   volume        solid voxels, and the same in cubic metres and litres
//   area          exposed faces — the surface a path tracer will actually light
//   centroid      where the mass is, which catches a shape that is not the symmetry it claims
//   spans         the width of the matter along a line, which is how a doorway is checked
//   profile       a slice printed as text, for when a number is not enough and a picture is too
//                 much
//   histogram     how much of each material, which catches a paint rule that never fires
//
// # Why exposed faces rather than a triangle count
//
// Because that is the quantity the renderer cares about and the quantity that changes when a
// surface is rough. A wall given a raspy displacement has the same volume and the same bounding
// box as a smooth one; its exposed area is half as much again. It is the only number here that
// notices texture.

#include <string>
#include <vector>

#include "core/types.hpp"
#include "game/clip.hpp"
#include "world/voxel_type.hpp"

namespace ws {
namespace forge {

struct Extent {
    bool any = false;
    i32 low[3]{0, 0, 0};
    i32 high[3]{0, 0, 0};   // inclusive

    i32 span(u32 axis) const { return any ? (high[axis] - low[axis] + 1) : 0; }
};

struct TypeShare {
    VoxelTypeId type = 0;
    u64 count = 0;
    f64 fraction = 0.0;
};

struct Measurement {
    i32 size[3]{0, 0, 0};       // the sampled box, in voxels
    i32 voxels_per_metre = kVoxelsPerMetre;

    Extent extent;              // of the matter, not the box
    u64 solid = 0;
    u64 covered = 0;            // cells that are part of the clip, full or empty
    u64 exposed_faces = 0;      // solid faces with air or nothing on the other side

    f64 centroid[3]{0, 0, 0};   // in voxels, relative to the clip's own origin

    std::vector<TypeShare> types;

    // Derived, in metres, because that is what the author was thinking in.
    f64 metres(i32 voxels) const {
        return static_cast<f64>(voxels) / static_cast<f64>(voxels_per_metre);
    }
    f64 cubic_metres() const {
        const f64 per = static_cast<f64>(voxels_per_metre);
        return static_cast<f64>(solid) / (per * per * per);
    }
    f64 square_metres() const {
        const f64 per = static_cast<f64>(voxels_per_metre);
        return static_cast<f64>(exposed_faces) / (per * per);
    }
};

Measurement measure(const Clip& clip, i32 voxels_per_metre = kVoxelsPerMetre);

// How wide the matter is along one axis, at a fixed position on the other two.
//
// This is the doorway check, the corridor check and the wall-thickness check. It reports the
// first and last solid cell and whether anything in between was empty, because "two metres wide"
// and "two metres wide with a gap in the middle" are different answers to the same question.
struct Span {
    bool any = false;
    i32 first = 0;
    i32 last = 0;
    i32 solid = 0;      // how many of the cells between first and last hold matter
    bool contiguous = false;

    i32 length() const { return any ? (last - first + 1) : 0; }
};

Span span_along(const Clip& clip, u32 axis, i32 a, i32 b);

// The gap: the run of empty cells along a line, which is what a doorway or a window actually is.
// Reported the same way, because an opening is measured exactly as a solid is and confusing the
// two is how a door ends up a voxel short.
Span gap_along(const Clip& clip, u32 axis, i32 a, i32 b);

// Is the clip the same when reflected about the middle of an axis?
//
// Reports how many cells disagree rather than a yes or no, because a clip that is symmetric
// except for four voxels has a typo in it, and a clip that disagrees in half its cells was never
// meant to be symmetric.
u64 mirror_mismatch(const Clip& clip, u32 axis);

// A slice through the clip as text, one character per voxel.
//
// For when a number will not do and a screenshot is too much: this is what catches a wall that
// is one voxel thick where it should be two, or a stair whose treads are not level. `axis` is
// the one held fixed, `at` is where. Scaled down by `step` so a 256-voxel slice fits on a
// screen, taking the most common cell in each block so a thin wall does not vanish.
std::string slice_text(const Clip& clip, u32 axis, i32 at, i32 step = 1);

// The whole measurement as a report, in the order a person reads it.
//
// The names may be null; when given, entry i is the name of voxel type i, so the report says
// "stone" rather than "type 3". The script layer knows those names and nothing else does.
std::string report(const Measurement& m, const std::vector<std::string>* names = nullptr);

}  // namespace forge
}  // namespace ws
