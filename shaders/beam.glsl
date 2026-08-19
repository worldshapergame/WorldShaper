// R7 -- the arithmetic the beam pre-pass and the primary ray have to agree about.
//
// Two shaders read the same grid: shaders/beam.comp writes one start distance per TILE CORNER and
// shaders/visibility.comp reads the four corners of its own tile. Everything that decides where a
// corner is, how wide its cone is, and how far back the answer has to be pulled therefore lives
// here rather than twice -- a second copy of any of it would eventually disagree with the first,
// which is the fault D133 and D147 both describe.
//
// Include AFTER node.glsl: every function here reads `push`, which node.glsl declares.

#ifndef WS_BEAM_GLSL
#define WS_BEAM_GLSL

// The tile, in pixels each way. 8, which is `04-rendering.md` §1's "1/8-resolution pre-pass" and
// is also visibility.comp's workgroup, so a tile and a workgroup are the same 64 pixels whenever
// R9c's halo margin is nought -- which is every settled frame.
const int kBeamTile = 8;

// How much wider the coarse ray's cone is than one pixel's, as a multiple.
//
// This is the whole of the conservatism argument and it is not a tuning number.
//
// node_march picks its level as `floor(log2(t * pixel_angle * lens.z))`, so the cell it walks has
// an edge of `2^floor(log2(x))`, which is strictly greater than x/2. Feed it `16 * pixel_angle /
// lens.z` and the cell edge is therefore strictly greater than `8 * t * pixel_angle` -- the
// geometric width of one TILE at that distance.
//
// That is the property that makes four corner rays enough. A grid cell whose edge is at least as
// long as the tile is wide, and which overlaps the tile's cross-section, must contain one of the
// tile's four corners: in one axis, a cell [a, a+s] with s >= w overlapping [0, w] contains 0 when
// a <= 0 and contains w otherwise, and the two axes are independent. So anything the tile's
// frustum can reach is on a corner ray, and the smallest of the four corner answers bounds every
// pixel between them.
const float kBeamCone = 16.0;

// How far back the answer is pulled before anybody is allowed to start from it.
//
// The argument above is exact for a cross-section square that lies in the cell grid's own axes and
// approximate for one that does not, and node.glsl's own level clamp puts a ceiling on the cell
// (see beam_trust below). So the answer is pulled back by a small multiple of the larger of the
// two lengths in play -- the tile's width and the cell the walk stopped in -- rather than trusted
// to the voxel. At 1440p that is about 3% of the distance, so 97% of the skip survives it.
const float kBeamBackoff = 2.0;

// ...and a floor in voxels, because the relief above goes to nought as the distance does and a
// surface a hand's breadth from the eye is exactly where being one voxel wrong is visible.
const float kBeamFloor = 2.0;

// The coarsest cell node.glsl's outer walk will ever step, in voxels: `1 << kNodeMaxDetail`.
// Named here because the trust distance below is entirely a consequence of it.
const float kBeamCellMax = 128.0;

// One pixel's angular size, the way node.glsl means it.
float beam_pixel_angle() {
    return 2.0 * push.lens.x / float(push.resolution.y);
}

// The cone the coarse ray is marched with. `lens.z` is divided out because node_march multiplies
// it back in, so this stays exactly 16 pixel widths whatever the detail bias is set to.
float beam_cone_angle() {
    return beam_pixel_angle() * kBeamCone / max(push.lens.z, 1e-4);
}

// How far out the corner rays are still conservative, in voxels from the eye.
//
// node.glsl clamps its level at kNodeMaxDetail, so past the distance where the cone would ask for
// a cell bigger than 128 voxels the cell stops growing while the tile goes on widening -- and the
// "a cell at least as wide as the tile contains a corner" argument stops holding. That distance is
// where `kBeamCone * t * pixel_angle` reaches 2 * kBeamCellMax, which is `16 / pixel_angle`
// voxels: **360 m at 1440p and 540 m at 4K**, at 32 voxels to the metre.
//
// Those two figures read 625 m and 936 m until they were checked. The formula was right and the
// arithmetic under it had been done from the screen's WIDTH while `beam_pixel_angle()` above
// correctly divides by `resolution.y` -- so both were out by exactly the aspect ratio. Worth
// leaving the correction visible rather than silent, because the two ways of reading "1440p" are
// what makes this class of slip so easy: 11,520 voxels at 1440 lines is 360 m, and 2560 columns
// would have made it 640.
//
// Beyond it the beam is not refused -- it is simply not allowed to move the start any further, so
// a ray past that range marches exactly as it does today.
float beam_trust() {
    return (2.0 * kBeamCellMax / kBeamCone) / max(beam_pixel_angle(), 1e-9);
}

// The distance a ray may begin at, given how far a coarse ray got before it could have hit
// anything. `bound` is in voxels from the eye; so is the answer.
float beam_relieve(float bound) {
    const float angle = beam_pixel_angle();
    // The tile's own width at that distance -- how far a corner ray can be from the fine ray whose
    // start it is bounding.
    const float tile = float(kBeamTile) * bound * angle;
    // ...and the cell the coarse walk stopped in, which may itself stand a cell proud of the matter
    // inside it.
    const float cell = min(max(bound * beam_cone_angle() * push.lens.z, 1.0), kBeamCellMax);
    return max(bound - kBeamBackoff * max(tile, cell) - kBeamFloor, 0.0);
}

// How many corners the grid has for the resolution in `push`. One more than the tile count each
// way, so the last tile has a right and a bottom edge to read.
ivec2 beam_corners() {
    return ivec2((push.resolution.xy + uint(kBeamTile) - 1u) / uint(kBeamTile)) + ivec2(1);
}

// The direction through a corner of the tile grid.
//
// The +0.5 a pixel ray carries is deliberately absent: a pixel centre sits at p + 0.5 and a tile
// spanning pixels [8c, 8c+8) has its geometric corners at 8c and 8c+8, so the four corner rays
// strictly enclose every pixel-centre ray in the tile. Half a pixel of slack in the right
// direction, for free.
vec3 beam_direction(vec2 corner_pixel, vec3 fwd, vec3 rgt, vec3 upv) {
    const vec2 uv = corner_pixel / vec2(push.resolution.xy) * 2.0 - 1.0;
    const float aspect = float(push.resolution.x) / float(push.resolution.y);
    return normalize(fwd + rgt * uv.x * push.lens.x * aspect - upv * uv.y * push.lens.x);
}

// Whether the host has switched either half of R7 on. `--no-beam` and `--no-temporal-start` clear
// these, and a cleared beam bit means visibility.comp starts every ray at nought, which is exactly
// the renderer as it was.
bool beam_enabled() { return push.beam.x != 0.0; }
bool beam_temporal_enabled() { return push.beam.y != 0.0; }

#endif  // WS_BEAM_GLSL
