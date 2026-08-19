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

// Constraint points are not capped, and getting there took three goes worth recording.
//
// Eight, in the parameter block, with a note shrugging that "more than this can exist; they simply
// stop being marked, which is a display limit rather than a tool limit" — the shrug trap 7 is
// about, because the ninth point was dropped, the box grew to reach it, and nothing on screen said
// where it was. Then sixty-four, which is a bigger number and the same shape of answer. Now none:
// the marks live in a storage buffer and any number can be drawn.
//
// What had to be solved to remove the cap is COST, because each mark is tested against the ray in
// the composite and that is per pixel. Two bounds do it, and neither is a limit:
//
//   the whole set has one box, so a pixel that cannot be looking at any mark pays one slab test;
//   the set is sorted spatially and split into groups of kMarkGroup, each with its own box, so a
//   pixel inside the outer box still only opens the handful of groups it actually crosses.
//
// A thousand points scattered over a building therefore cost a pixel a few dozen tests rather than
// a thousand. See `mark_groups` in main.cpp for the packing and preview.glsl for the walk.
inline constexpr u32 kMarkGroup = 32;

// Where the marks live in the clip cell buffer, counted from its far end.
//
// They ride in that buffer rather than in one of their own because it is already bound to both
// shaders that draw previews and is already the place preview data goes — adding a binding to two
// pipelines to carry twelve bytes a point is more moving parts than the problem has. Clips pack
// upward from zero and marks pack downward from the top, so the two can only meet if a clip and a
// hundred thousand constraint points are live at once, and that meeting is checked for rather than
// assumed.
inline constexpr u32 kMarkReserveCells = 512u * 1024u;

// How many preview boxes can be on screen at once. The clipboard's copies are the reason
// there is more than one; beyond this they still stamp, they just stop being drawn.
inline constexpr u32 kMaxPreviewBoxes = 16;

struct RenderParams {
    f32 origin[4];         // xyz: voxel units relative to the camera chunk corner
    f32 forward[4];
    f32 right[4];
    f32 up[4];
    i32 camera_chunk[4];
    // The world's bounding box in chunks, relative to the camera chunk. Every ray is clipped to
    // it. It was the RESIDENT box until R1e, and the wrapped chunk grid's dimensions used to sit
    // above it; both went with the chunk renderer.
    i32 bounds_min[4];
    i32 bounds_max[4];
    u32 resolution[4];     // xy: pixels, z: debug mode, w: feedback capacity
    f32 lens[4];           // x: tan(fov/2), y: max distance, z: detail bias

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

    // The material in hand, plainly, with nothing done to it.
    //
    // Separate from the two tints above because those carry a *decision* — they are already
    // inverted or not according to whether the box is a carve or a place and whether the pixel is
    // in front of geometry. The cursor marker and the constraint marks are not decisions; they are
    // "here is where you are pointing" and "here is a point you dropped", and both want the
    // material's own colour whatever the tool is about to do with it.
    //
    // w = 1 when there is a material to speak of.
    f32 tool_colour[4];

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

    // The box every live mark fits inside, in the same camera-relative voxels.
    //
    // One slab test against this rejects the whole marks walk for any pixel that cannot be looking
    // at one of them, which is nearly all of them. Computed on the host because it is one box for
    // the frame and the alternative is every pixel deriving it again.
    //
    // marks_min[3] is how many GROUPS there are and marks_max[3] is where their headers start in
    // the clip cell buffer. The marks themselves follow the headers. Nought groups means none.
    i32 marks_min[4];
    i32 marks_max[4];

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

    // Where the camera was LAST frame, so a pixel can work out how far what it is looking at has
    // moved across the screen since. Same space and same units as origin/forward/right/up above.
    //
    // The whole of motion blur is this: a point's world position is known from the ray and the
    // distance to what it hit, projecting it through the previous camera says where it used to be
    // on screen, and the difference is how fast it is travelling in pixels. Nothing has to be
    // stored per pixel and no extra pass has to run.
    f32 prev_origin[4];
    f32 prev_forward[4];
    f32 prev_right[4];
    f32 prev_up[4];

    // x: how much of a frame the shutter is open for, as a fraction — 0 turns blur off entirely,
    // 0.5 is a 180-degree shutter, which is what a film camera does and what the eye is used to.
    // y: the longest streak allowed, in pixels, so a fast turn cannot cost an unbounded number of
    // taps. z and w spare.
    f32 motion[4];

    // The weather. x is coverage, 0 clear to 1 overcast; y is the time in seconds, which is what
    // moves the decks and therefore their shadows. See shaders/pt_clouds.glsl.
    f32 sky_cloud[4];
    // The low deck's wind, in metres a second. Higher decks are derived from it: faster with
    // altitude and veered, which is both true and what makes a sky read as deep.
    f32 sky_wind[4];

    // The tone stage's dials. x is the ceiling the light meter may not expose past, as a
    // multiplier, with 0 meaning the shader's own default -- it is what decides whether a dark room
    // is allowed to be dark, and `--exposure-max N` sweeps it. See kExposureMaxDefault in
    // shaders/resolve.comp. yzw spare.
    //
    // Appended rather than folded into an existing vector, and `motion.w` was the obvious candidate
    // because the accumulator it described went with R3d. D553 is why not: changing what a word
    // means makes every rule written about it suspect, and a spare field costs sixteen bytes once.
    f32 tone[4];

    // ---- R7 primary ray -------------------------------------------------------------------
    //
    // x: 1 when shaders/beam.comp has written this frame's start distances, 0 for `--no-beam` --
    //    which makes shaders/visibility.comp start every ray at nought, exactly as it always did.
    // y: 1 when the previous frame's depth may lower that start (R7b), 0 for
    //    `--no-temporal-start` and on any frame whose depth image does not hold a usable previous
    //    frame -- the first one after startup or a resize.
    // zw spare.
    //
    // Appended, not folded into a spare word of `tone`, and D553 is the standing reason: changing
    // what a word means makes every rule written about it suspect, and a fresh vector costs sixteen
    // bytes once.
    f32 beam[4];
    // R12c derive in marcher
    //
    // x: 1 when a descent that reaches a node the pool has not built evaluates the field there
    // instead of drawing R2d's stand-in (D237). Off by default and behind `--derive-in-marcher`.
    // y: the CAP -- how many cells one dispatch may derive. Past it a ray falls back to the
    // stand-in, which is the behaviour this replaces and so the only safe thing to fall back to.
    // D687 priced an uncapped frame on the enclosed camera at 1,239 ms.
    // z: which slot of the derivation counters this frame owns, so the host reads a slot nobody is
    // writing.
    // w: the WORK budget, in units of 1024 FIELD NODES one dispatch may walk deriving, 0 for none.
    // The cell cap alone does not bound a frame and this is not a refinement of it: D688 measured
    // one cell at 1,073,935 nodes and 372 ms against a typical 5,195 and 1.8, so twenty thousand
    // cells is anywhere between forty milliseconds and two hours. Measured here: 20,000 cells with
    // no visit budget lost the device on a 150-second run.
    //
    // A runtime value rather than a preprocessor define because shaders/node.glsl is included by
    // three passes and a define would rebuild all of them into a different renderer.
    u32 derive[4];
    // R4c/R4d ---------------------------------------------------------------------------------
    //
    // Two control arms that could not be spelled where every other one here is spelled. The dials
    // live in word 0 of the light probe buffer, and the bits that name them are declared in
    // `shaders/node.glsl` — so a stage that may not touch that file has nowhere to put a bit. A
    // vec4 of this block costs sixteen bytes once, is read straight into a register by both passes
    // that want it, and needs no second buffer and no second binding.
    //
    //   x  R4c's reflected IMAGE. 0 puts the bins back over the whole hemisphere with the kernel
    //      widened to the bin, which is D591 and D594 exactly and is what every figure before this
    //      was measured in. `--no-reflected-image`.
    //   y  R4d's DISPERSION. 0 gives every medium one index of refraction for all three channels,
    //      which is D652 exactly. `--no-dispersion`.
    //   z  R4f's CONTRIBUTION BUDGET, as a share of the pixel: a reflected segment worth less than
    //      this is not cast. The flag and the dial are one number on purpose -- "off" is "nothing
    //      is worth casting", so `--no-ior-reflection` and `--reflect-budget 0` are the same arm
    //      and there is no second bit that could disagree with the first. See
    //      kReflectBudgetDefault in shaders/reflect.glsl.
    //   w spare.
    //
    // Appended rather than folded into `tone`, whose y and z are already `--denoise-edge` and
    // `--lobe-floor`: D553 is the standing measurement of what changing a word's meaning costs, and
    // a spare field costs sixteen bytes once.
    f32 r4[4];
    // R5c/R5d ---------------------------------------------------------------------------------
    //
    // The two SECOND halves, and they are here rather than in the probe for `r4`'s reason with one
    // more on top of it: `shaders/resolve.comp` has no binding for the light probe buffer at all
    // (D665 records the same wall from the other side), so a dial that the COMPOSITE has to read
    // cannot be a probe bit however convenient that would be.
    //
    //   x  R5c's level-0 arm. 0 draws a level-0 hit's own colour outright instead of blending it
    //      towards the brick average sitting in the same word, which is D664 exactly and is what
    //      every figure before this was measured in. `--no-voxel-blend`.
    //   y  R5d's third layer. 0 leaves the far node's own coverage byte and its "sky beyond" bit at
    //      nought in `out_behind`, so the composite draws the far surface of an edge opaque
    //      whatever its coverage says, which is D663 exactly. `--no-edge-layers`.
    //   zw spare.
    //
    // Both are read as `!= 0.0` and neither is a magnitude, for the same reason `r4`'s two are: a
    // control arm that can be half on is a control arm nobody can quote a figure against.
    f32 r5[4];
};
// Written out rather than accumulated as a sum of historical deltas, which is what this was and
// which nobody could check: 41 vectors of 16 bytes, in the order shaders/params.glsl declares
// them. std140 lays out by position, so a shader whose field list disagrees with this reads
// everything after the first gap at the wrong offset -- silently, and it has cost two sessions.
//
// The count: origin, forward, right, up, camera_chunk, bounds_min, bounds_max, resolution, lens,
// tint_visible, tint_occluded, tool_colour (12), box_min and box_max at 16 each (32), marks_min,
// marks_max (2), clip_slot and clip_coarse at 16 each (32), edit_min, edit_max (2), prev_origin,
// prev_forward, prev_right, prev_up, motion, sky_cloud, sky_wind, tone (8) -- 88, and then FOUR
// appended in one afternoon: R7's `beam` makes 89, R12c's `derive` 90, R4c/R4d's `r4` 91 and
// R5c/R5d's `r5` 92.
//
// Every one of the first three was written against a block of 88, because each was built in its own
// worktree from the same base, and not one of them could have known about the other two. That is
// what this assert is for and it is the fourth time today it has been the thing that caught it: a
// count left at 89 is the host writing one structure while every shader reads another, at every
// offset past the gap, silently. The field ORDER has to be checked by eye as well -- the assert
// counts and does not compare -- and it was: beam, derive, r4, r5, in that order in both files.
static_assert(sizeof(RenderParams) == 92 * 16, "RenderParams must match the GLSL block");

// One entry per chunk the marcher wanted and could not find. Written by the shader,
// read back by the streamer two frames later.
struct FeedbackEntry {
    i32 x = 0;
    i32 y = 0;
    i32 z = 0;
    i32 level = 0;   // the detail level the ray was at, for prioritising later
};
static_assert(sizeof(FeedbackEntry) == 16, "FeedbackEntry must match the GLSL struct");

// Set in `level` when the entry says a ray READ this node rather than failed to find it.
//
// One buffer carries both because they arrive from the same place at the same rate and a second
// would need its own capacity, its own readback and its own barrier. A level is 0..24, so the bit
// is free. Must match kFeedbackUsed in shaders/node.glsl.
inline constexpr i32 kFeedbackUsed = 0x10000;

// ...and this one says the entry carries a node SLOT in x rather than a coordinate. Residency is
// per node now (D260), and a ray knows the slot it stopped on, so the CPU stores rather than
// descends. Must match kFeedbackRead in shaders/node.glsl.
inline constexpr i32 kFeedbackRead = 0x20000;

// ...and this one is a FACE the eye can see: a node coordinate in xyz, and its level with the
// direction packed above it. What the face pass shades. Must match kFeedbackFace in
// shaders/node.glsl.
inline constexpr i32 kFeedbackFace = 0x40000;

// The coordinate is exact and must not be dilated to its neighbours. Must match kFeedbackExact in
// shaders/node.glsl, where the reasoning is.
inline constexpr i32 kFeedbackExact = 0x80000;

// ...and this one is a FACE a pixel READ, carrying a face-store SLOT in x. The consumer's whole
// answer is `FaceStore::touch`: it claims nothing and asks for nothing.
//
// Not to be conflated with kFeedbackFace, which is the request lattice asking for a face to be
// CLAIMED and arrives for one pixel in stride² — a sample, and therefore useless as a residency
// clock for any face whose coverage is under a pixel. This one is one entry per face per
// `face_read_period` frames however many pixels are on it. Must match kFeedbackFaceRead in
// shaders/node.glsl. D508.
inline constexpr i32 kFeedbackFaceRead = 0x100000;

// ...and this one rides on kFeedbackFace and says a LIGHT ray landed on it, not a pixel. R9a.
//
// The consumer's answer is the same claim with one difference: it is subject to the off-screen cap
// and is DECLINED rather than refused when that cap is full. It builds nothing and streams nothing,
// so R9h's rule -- no light path may cause streaming -- is not bent by it. Must match
// kFeedbackSecondary in shaders/node.glsl.
inline constexpr i32 kFeedbackSecondary = 0x200000;

// The gathering ray's counters: one word a question, over the whole dispatch, cleared every frame
// and read back at the screenshot audit. Must match kLightProbeWords in shaders/node.glsl, which
// carries the word map -- the shader is the authority because it is the only writer.
inline constexpr u32 kLightProbeWords = 36;
// Where the by-level histogram of "stopped by a cell the pool has not built" starts in it. Must
// match kLightProbeLevels in shaders/node.glsl.
inline constexpr u32 kLightProbeLevels = 9;
// R4c's three, which are counters and so live inside the span the host fills with nought: how many
// faces are holding a block of outgoing bins, how many asked and were turned away, and how many
// took one over this frame. The third is what tells a full pool from a thrashing one.
inline constexpr u32 kProbeLobeHeld = 24;
inline constexpr u32 kProbeLobeDeclined = 25;
inline constexpr u32 kProbeLobeTaken = 26;
inline constexpr u32 kProbeLobeRays = 27;
inline constexpr u32 kProbeLobeBursting = 28;
// R4b: how many of the held blocks are the expensive four-block class.
inline constexpr u32 kProbeLobeBigHeld = 29;
// The last word, which is not a counter at all: the OFF-SCREEN set's stride, in frames, host-written
// exactly as the dials are. Must match kProbeSecondaryStride in shaders/node.glsl.
//
// At the far END of the buffer on purpose. The host writes the dials, writes this, and fills what is
// between them with nought -- three ranges that do not touch, because transfer commands in one
// command buffer have no ordering between them and a host word inside the filled span would be a
// race. See the write site in main.cpp.
inline constexpr u32 kProbeSecondaryStride = 32;
// R5b's ramp, and it is TWO counters for the reason trap 7 gives: how many faces were short enough
// of their own shadow rays to want the coarse face over them, and how many of those found nothing up
// the tree worth taking. With one number, "nothing was under-measured" and "everything was, and
// kSunSeedMin refused all of it" print the same nought -- which is exactly how this change was first
// read as doing nothing. Counters, so they sit inside the span the host fills with nought.
inline constexpr u32 kProbeSunRamped = 30;
inline constexpr u32 kProbeSunNoStandIn = 31;
// R9c's two, past the counters for the same reason: host-written, outside the span the host fills
// with nought, so the three ranges do not touch. Must match kProbeHaloMargin and kProbeHaloStride
// in shaders/node.glsl. The margin is in pixels each side and nought means the stage is off.
inline constexpr u32 kProbeHaloMargin = 33;
inline constexpr u32 kProbeHaloStride = 34;
// R5b's, in the same host-written range and for the same reason: how many samples of the coarse
// face above it a NEWLY CLAIMED face starts its sun term from. 0 is the control arm. Must match
// kProbeSunSeed in shaders/node.glsl; the figure itself is kSunSeedSamples, there.
inline constexpr u32 kProbeSunSeed = 35;

// The dials in word 0 of that buffer, host-written and card-read.
//
// They live in a buffer rather than in the push block because that block is exactly 128 bytes full
// -- the static_assert on NodePush in main.cpp says so, and 128 is what Vulkan guarantees, which is
// what the Steam Deck gives. The alternative was to change what an existing field means, and D553
// is the standing measurement of what that costs: a rule written about a word outlives the meaning
// it was written for, with no compiler error and no obvious picture.
inline constexpr u32 kProbeOn = 1u << 0;             // count at all. `--no-light-probe` clears it
inline constexpr u32 kProbeCoarseBounce = 1u << 1;   // R9f's read half. `--no-coarse-bounce` clears it
// R5a. Cleared, `face_denoise` writes a face's OWN answer into the filtered words rather than its
// neighbourhood's, so the composite reads the same four words in both arms and the A/B prices the
// eight neighbour lookups instead of a branch in the reader. `--no-face-denoise` clears it.
inline constexpr u32 kProbeDenoise = 1u << 2;
// R4a. Cleared, no face asks what it is made of and neither the descent nor the load that finds out
// happens at all -- the renderer as it was before R4. `--no-face-materials` clears it, and the
// census in the audit reads this bit so that "nothing is specular here" and "nobody looked" cannot
// print the same line (trap 15).
inline constexpr u32 kProbeMaterial = 1u << 3;
// R9f's fold: a coarse face takes the average of the four faces under it rather than its own rays.
// Cleared by `--no-face-fold`, which is the control arm.
inline constexpr u32 kProbeFold = 1u << 4;
// R4c: a face may hold a block of outgoing bins and a pixel may read one. Cleared by
// `--no-face-lobe`, which is the control arm for the storage half of that stage -- the sun's
// highlight is arithmetic and is drawn either way, so the arms differ by exactly the bins.
inline constexpr u32 kProbeLobe = 1u << 5;
// R4b's ray, on its own. Cleared by `--no-lobe-ray`: the pool, the bins and the energy split are
// identical in both arms and the bins are then filled only by the far ray that was being cast
// anyway, which is the state D592 measured as unable to carry a reflection.
inline constexpr u32 kProbeLobeRay = 1u << 6;
// R4b's coverage rule: a polished face that fills enough of the screen gets four blocks and a
// hundred and forty-four bins instead of one block and thirty-six. Cleared by
// `--no-lobe-coverage`, which is the arm that prices the expensive class on its own.
inline constexpr u32 kProbeLobeCoverage = 1u << 7;
// R4d: a light ray carries on through transmissive matter instead of stopping dead on it. Cleared
// by `--no-see-through`, which is the state every figure before R4d was taken in.
inline constexpr u32 kProbeSeeThrough = 1u << 8;
// R4d's second half: the primary ray bends where it enters glass or water and bends back where it
// leaves it, and the medium absorbs over the true distance rather than once per voxel crossed.
// Cleared by `--no-refraction`, which restores D604's straight ray exactly.
inline constexpr u32 kProbeRefract = 1u << 9;
// R4e: a translucent face with the sun behind it casts the ray `sun_possible` used to throw away,
// through the matter behind it, and is lit by what survives the crossing. Cleared by
// `--no-translucency`, which is the state every figure before it was taken in: marble as granite.
inline constexpr u32 kProbeTranslucent = 1u << 10;
// R5d: a primary ray that stops on a coarse node only part of which is matter marches on past it,
// and the composite draws the two surfaces in proportion to the node's own per-direction coverage.
// Cleared by `--no-edge-aa`, which is every build before this one: a node larger than a voxel drawn
// as a solid block, so every distant silhouette is a hard stair-step.
inline constexpr u32 kProbeEdgeAA = 1u << 11;
// R5c's first half: a hit blends its folded colour towards the colour of the level the ordered
// dither did not pick for that pixel. Cleared by `--no-level-blend`, which draws the cell's own
// colour outright and is the two-tone 4x4 pattern this renderer has shown on middle-distance
// surfaces since the marcher existed. The dither still picks the CELL in both arms, so the coverage,
// the face key and the depth are identical and an image diff between the two arms is colour alone.
inline constexpr u32 kProbeLevelBlend = 1u << 12;
// R5b: a face that has cast fewer than four shadow rays is drawn part way towards the coarse face
// standing over it, in proportion to how many it has. Cleared by `--no-sun-confidence`, which
// believes a face's own ratio the moment it has one sample -- the state every figure before this was
// taken in, and what a player sees as speckle on everything the camera has just revealed.
//
// Read where the stand-in is WRITTEN and never where it is read, which is kProbeDenoise's
// arrangement and for a harder reason than tidiness: `resolve.comp` has no binding for this buffer
// at all, so a control arm spelled in the composite is not available to it. Cleared, the shading
// pass leaves the stand-in word at nought and both readers return the raw ratio bit-exactly.
inline constexpr u32 kProbeSunConfidence = 1u << 13;

// R4c's pool: how many blocks of outgoing bins there are and how many words each is. Must match
// kLobeBlocks, kLobeBins and the layout in shaders/face_terms.glsl, which is the authority because
// it is the only thing that indexes them. Two words a block of header and two a bin -- a packed
// rgb9e5 mean and a float weight sum.
//
// The host's only interest in any of this is how large the buffer has to be. Sized against R4a's
// census: 35,950 faces of 801,175 on the facility carry any metal at all.
inline constexpr u32 kLobeBins = 36;
// ...and R4b's expensive class, which is four consecutive blocks used as one grid. Must match
// kLobeBigSide in shaders/face_terms.glsl.
inline constexpr u32 kLobeBigBins = 144;
inline constexpr u32 kLobeBlocks = 131072;
inline constexpr u32 kLobePoolWords = kLobeBlocks * (4 + kLobeBins * 2);

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

// Where the quarter-resolution cloud volume lives. Written by shaders/clouds.comp and read by the
// path tracer, which share one descriptor set layout because the cloud pass needs the parameter
// block and the sun and nothing else.
inline constexpr u32 kCloudBinding = 20;

// Where this frame's cloud marches land, packed one per block before they are resolved into the
// full-resolution history.
inline constexpr u32 kCloudMarchedBinding = 21;

// How many full-resolution pixels a cloud sample covers, in each axis. Must match kCloudScale in
// shaders/pt_clouds.glsl.
inline constexpr u32 kCloudScale = 4;

// The fixed-point conventions, spelled out here so the shader and anything that reads the
// buffer back cannot disagree about them.
inline constexpr f32 kLogLuminanceBias = 16.0f;    // log2 range is [-16, +16] before biasing
inline constexpr f32 kLogLuminanceUnit = 256.0f;   // and 1/256 of a stop is finer than sight
inline constexpr f32 kExposureUnit = 65536.0f;

// ---- R4b coverage bins -------------------------------------------------------------------------
//
// Appended at the end of the file rather than beside the other dial bits, so nothing above this
// line moves: another worktree is reading this header for `shade_faces.comp` and `resolve.comp`
// while this was written.
//
// D186's rule, and it is the half of R4b that was never built: how many outgoing bins a face gets
// is a function of how many PIXELS it covers as well as of how sharp its material is. Both are read
// as quantities and neither is compared against a cutoff -- see `face_bins_earned` in
// src/world/face_store.hpp for the arithmetic and shaders/face_worklist.comp for the pass that
// applies it.
//
// Off, every lobe is the one fixed block the shading pass hands out, whatever the face covers:
// today exactly. `--no-coverage-bins` clears it. It is a separate lever from `--lobe-coverage`
// (kProbeLobeCoverage), which is D599's ROUGHNESS-only sharp class and is still off by default;
// with both on, a face may reach the sharp class by either route, which is why they are two bits
// and not one.
inline constexpr u32 kProbeCoverageBins = 1u << 14;

// How many bytes one block of the lobe pool is, header and bins together. Derived from the two
// figures above rather than written again, so a change to either cannot leave this behind.
inline constexpr u32 kLobeBlockBytes = (kLobePoolWords / kLobeBlocks) * 4u;

// How many blocks the sharp class is. Must match kLobeBigWays in shaders/face_terms.glsl, which is
// the authority; it is here because the occupancy census in gpu/face_buffers.cpp has to turn a
// count of sharp-class faces into bytes and cannot include a shader.
inline constexpr u32 kLobeBigWays = 4;

}  // namespace ws
