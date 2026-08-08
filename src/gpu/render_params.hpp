#pragma once
// Per-frame parameters shared by the render passes.
//
// These used to be push constants, which was fine until they grew past 128 bytes — the
// minimum a Vulkan implementation is required to offer, and exactly what AMD hardware
// gives, which includes the Steam Deck. The dev machine allows 256 and would have hidden
// that until the first run on the target device.
//
// A per-frame uniform buffer has no such limit, costs one extra descriptor, and is read
// once per invocation into registers. The layout below is std140: every member is
// 16-byte aligned, so it matches the GLSL block member for member without padding rules
// to get wrong.

#include "core/types.hpp"

namespace ws {

// How many constraint points the preview can draw. More than this can exist; they simply
// stop being marked, which is a display limit rather than a tool limit.
inline constexpr u32 kMaxPreviewMarks = 8;

// How many preview boxes can be on screen at once. The clipboard's copies are the reason
// there is more than one; beyond this they still stamp, they just stop being drawn.
inline constexpr u32 kMaxPreviewBoxes = 16;

// Levels of the summary octree the marcher can read. Declared here rather than included from
// the world layer, which this header deliberately does not depend on; world_buffers.cpp sees
// both and asserts they agree.
inline constexpr u32 kRenderSummaryTiers = 8;

struct RenderParams {
    f32 origin[4];         // xyz: voxel units relative to the camera chunk corner
    f32 forward[4];
    f32 right[4];
    f32 up[4];
    i32 camera_chunk[4];
    u32 grid_dims[4];      // xyz: wrapped chunk grid size
    i32 bounds_min[4];     // resident chunk bounding box, relative to the camera chunk
    i32 bounds_max[4];
    u32 resolution[4];     // xy: pixels, z: debug mode, w: feedback capacity
    f32 lens[4];           // x: tan(fov/2), y: max distance, z: detail bias
    i32 thumb_dims[4];     // xyz: wrapped summary grid size, in blocks; w: how many levels
    // Per level: x first grid cell, y first slot, z chunks per block. Every level shares one
    // grid buffer and one slot buffer, so the marcher needs one binding rather than sixteen.
    i32 thumb_tiers[kRenderSummaryTiers][4];

    // The chisel's preview box, in voxels relative to the camera chunk corner — the same
    // space as `origin`, so the resolve pass can intersect it with the ray it already has
    // without knowing anything about 64-bit world coordinates.
    // What the preview is drawn in, split by whether something is in front of it.
    //
    // Carving takes the inverse of the backdrop where you can see it, and where it is
    // buried it takes the colour of the material it is about to remove — so the shape
    // inside the rock reads as the rock that is going to leave. Placing is the opposite
    // way round: the material's own colour where it is visible, its inverse where it is
    // hidden, so the two never look alike.
    //
    // w = 1 means "use this colour"; w = 0 means "invert whatever is behind this pixel",
    // which only the shader can do because it is per pixel.
    f32 tint_visible[4];
    f32 tint_occluded[4];

    // The preview boxes, in voxels relative to the camera chunk corner — the same space as
    // `origin`, so the resolve pass can intersect them with the ray it already has without
    // knowing anything about 64-bit world coordinates.
    //
    // An array rather than one box because the clipboard shows a row of ghosts at once. The
    // chisel simply uses the first slot.
    //
    // w of box_min: 0 unused, 1 carving, 2 placing, 3 refused, 4 idle.
    i32 box_min[kMaxPreviewBoxes][4];
    i32 box_max[kMaxPreviewBoxes][4];

    // Constraint markers, w = 1 when the slot is used.
    i32 marks[kMaxPreviewMarks][4];

    // Per ghost box: where its clip starts in the cell buffer, and how big that clip is.
    // Ghost boxes (state 5) are marched against this rather than outlined, which is what
    // makes a paste preview show the voxels instead of the space they will go in.
    //
    // One slot per box rather than one clip for all of them, because copies carry a share
    // of the transform: a row turning through 90° is sixteen differently-shaped clips, not
    // sixteen instances of one. Slots point at the same offset when the shapes are the
    // same, which is the usual case and costs one upload.
    //
    // x: first cell, yzw: size. A size of zero means the slot has nothing in it.
    u32 clip_slot[kMaxPreviewBoxes][4];

    // The same clip's occupancy mask, one byte per 8x8x8 block, packed after its cells.
    // x: first entry, yzw: size in blocks. The march steps over empty blocks instead of
    // walking them a voxel at a time.
    u32 clip_coarse[kMaxPreviewBoxes][4];

    // What was just built or carved, and how far its shadow can fall, in voxels relative to the
    // camera chunk — the same space as `origin`. Faces inside it are made to look again at
    // once instead of trickling; everything outside is left alone, which is the difference
    // between a new shadow appearing and the whole scene shifting under you.
    //
    // edit_min[3] is 1 while the region is live and 0 when it is not.
    i32 edit_min[4];
    i32 edit_max[4];
};
static_assert(sizeof(RenderParams) == 1520, "RenderParams must match the GLSL block");

// One entry per chunk the marcher wanted and could not find. Written by the shader,
// read back by the streamer two frames later.
struct FeedbackEntry {
    i32 x = 0;
    i32 y = 0;
    i32 z = 0;
    i32 level = 0;   // the detail level the ray was at, for prioritising later
};
static_assert(sizeof(FeedbackEntry) == 16, "FeedbackEntry must match the GLSL struct");

// Entries per frame. Beyond this the frame's report is truncated, which costs nothing:
// the renderer asks again next frame until it gets served.
// One report per sampled pixel worst case, and sampling is one pixel in 64 — so this has
// to cover a 4K frame's worth (3840*2160/64 = 129,600) to avoid dropping reports, which
// slows convergence rather than breaking it.
inline constexpr u32 kFeedbackCapacity = 131072;

// What the frame turned out to look like, added up on the GPU while it is drawn.
//
// This is for automatic exposure. A brightness the tracer can be exposed for cannot be known
// before the frame is traced — it is the frame — so the only honest source is the frame
// before it, which is a sixtieth of a second stale and nobody can tell. The shader adds its
// pixels' log luminance up here as it writes them, and reads the previous frame's total back
// to choose the multiplier it tone maps with.
//
// Log luminance rather than luminance because exposure is a stop, not a scale: a single
// window in a dark room drags a linear mean far more than it drags what the eye adapts to.
//
// Fixed point because atomicAdd on a float is an extension not every driver has, and the
// accumulation has to work on the Steam Deck's AMD part as well as this desk's card.
struct FrameStatistics {
    // The sum of one value per *workgroup*: that group's mean of
    // clamp(log2(luminance) + kLogLuminanceBias, 0, 2 * kLogLuminanceBias) * kLogLuminanceUnit.
    //
    // Per workgroup and not per pixel, because per pixel overflows. A pixel contributes at
    // most 32 * 256 = 8192, and a 4K frame has 8.3M of them: 6.8e10 against the 4.29e9 a u32
    // holds. One 8x8 group's *mean* is bounded by the same 8192, and a 4K frame has 129,600
    // groups, so the total tops out near 1.06e9 — a quarter of the range, with room for a
    // display twice the size again.
    u32 log_luminance = 0;
    // How many groups went into that sum, so the mean is log_luminance / groups. Zero means
    // nothing was accumulated and whatever exposure was in use should be kept.
    u32 groups = 0;
    // The multiplier the shader settled on, times kExposureUnit. Written by one invocation,
    // read back the next frame as the value to move away from — which is what makes the
    // adaptation gradual instead of a cut. See the note on clearing below.
    u32 exposure = 0;
    // Where the lens is focused, in 1/16 of a voxel, as measured by the centre pixel of the
    // previous frame — the same previous-frame discipline `exposure` is read under, and for the
    // same reason: a frame cannot know its own depth before it is traced. Zero means nothing has
    // been measured yet, which a reader must take as focus at infinity rather than at nothing.
    u32 focus = 0;
};
static_assert(sizeof(FrameStatistics) == 16, "FrameStatistics must match the GLSL uvec4");

// Two slots, and this is the whole point of the arrangement.
//
//   [0] the frame being drawn. Zeroed just before the trace dispatch, added to with atomics.
//   [1] the frame before it: finished, complete, and written by nobody while it is read.
//
// The zeroing is preceded by a copy of slot 0 into slot 1, so slot 1 is always exactly what
// slot 0 finished as. One slot cannot do both jobs: a shader reading the words it is also
// adding to sees however much of the frame happened to have run, which depends on scheduling
// — so the same scene would expose differently twice in a row, and differently again on
// another card. That is the sort of fault that gets called flickering and blamed on the
// tracer.
//
// Cleared every frame rather than decayed: a sum left to accumulate across frames is divided
// by a count that no longer means anything, and while the camera is still the path tracer
// keeps refining the same pixels, so an undecayed sum would drift for a reason that has
// nothing to do with brightness. What carries between frames is the *exposure* in slot 1, not
// the luminance total — the shader blends towards the new measurement rather than jumping to
// it, which is where the eye's slowness lives. A time constant near half a second is what
// this was built for; the number is the shader's to choose.
inline constexpr u32 kFrameStatsSlots = 2;

// Where the tracer's descriptor set binds it. The set runs 0..18 already; this is the next
// one. It is bound to the path tracing pipeline only, because that is the pass that produces
// high dynamic range in the first place.
inline constexpr u32 kFrameStatsBinding = 19;

// The fixed-point conventions, spelled out here so the shader and anything that reads the
// buffer back cannot disagree about them.
inline constexpr f32 kLogLuminanceBias = 16.0f;    // log2 range is [-16, +16] before biasing
inline constexpr f32 kLogLuminanceUnit = 256.0f;   // and 1/256 of a stop is finer than sight
inline constexpr f32 kExposureUnit = 65536.0f;

}  // namespace ws
