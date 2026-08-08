// The normal a surface is SHADED by, which is not the normal it is MADE of.
//
// A voxel face is axis-aligned. There are six of them, so a sphere built of voxels reflects in
// exactly six directions and a curved wall shades in six steps. That is not noise and no
// denoiser will touch it: it is the geometry, faithfully rendered. It is also why a polished
// sphere in this renderer stipples, and why every column, dome and baluster in the facility
// will read as a stack of blocks however good the light is.
//
// The fix is to shade with a normal derived from the shape the voxels are approximating rather
// than from the face that was hit. The information is there -- the occupancy of the voxels
// around the hit describes the surface far better than one face does -- and the standard way to
// get at it is a gradient of the occupancy over a small neighbourhood, which is a smooth field
// where the faces are a staircase.
//
// Rules for whoever implements it:
//   - it must fall back to the face normal exactly when the neighbourhood says nothing useful,
//     because a wrong normal is worse than a blocky one: it leaks light through walls.
//   - it must not round off a genuine arris. A cornice, a step and a quoin are supposed to have
//     a hard edge, and a filter wide enough to smooth a sphere will destroy them. Whatever the
//     rule is, it has to tell a curve from a corner.
//   - it must be cheap. This runs for every shaded pixel.

vec3 shading_normal(ivec3 voxel, ivec3 face, vec3 point, int level) {
    // Placeholder: the face itself, which is what the renderer has always used.
    return vec3(face);
}