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

    // Where the camera stood last frame, so a pixel can find where its surface was on the
    // screen then and carry its average forward instead of starting again.
    //
    // Expressed in *this* frame's space. The space is anchored to the chunk the camera is in,
    // so crossing a chunk boundary shifts every coordinate by 256 voxels; folding that in on
    // the CPU, where the chunk numbers are 64-bit anyway, leaves the shader with one
    // subtraction rather than a pair of coordinate systems to reconcile.
    //
    // prev_origin.w is 1 when there is a previous frame worth reprojecting from and 0 on the
    // first frame, after a resize, and any other time the history is not the same picture.
    f32 prev_origin[4];
    f32 prev_forward[4];
    f32 prev_right[4];
    f32 prev_up[4];
};
static_assert(sizeof(RenderParams) == 1552, "RenderParams must match the GLSL block");

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

}  // namespace ws
