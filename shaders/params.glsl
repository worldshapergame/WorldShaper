// The per-frame parameter block's fields, in one place.
//
// std140 lays out by position, so a shader declaring fewer fields than the buffer holds reads
// everything after the gap at the wrong offset, silently. That has cost two debugging
// sessions already - once a broken screen, once a wrong picture with no error anywhere.
//
// Each shader still picks its own binding number; only the field list is shared. Mirrors
// RenderParams in gpu/render_params.hpp.
    vec4 origin;
    vec4 forward;
    vec4 right;
    vec4 up;
    ivec4 camera_chunk;
    ivec4 bounds_min;
    ivec4 bounds_max;
    uvec4 resolution;     // xy pixels, z debug mode, w feedback capacity
    vec4 lens;            // x tan(fov/2), y max distance, z detail bias
    vec4 tint_visible;    // xyz colour, w = 1 to use it, 0 to invert the backdrop instead
    vec4 tint_occluded;
    // The material in hand with nothing done to it. The two tints above carry a decision -- already
    // inverted or not by carve/place and by depth -- and the cursor marker and the marks are not
    // decisions, so they take this instead. w = 1 when there is a material.
    vec4 tool_colour;
    // xyz voxels relative to the camera chunk.
    // w: 0 unused 1 carve 2 place 3 refused 4 idle 5 clip ghost 6 cursor sphere
    ivec4 box_min[16];
    // w: bits 0-7 outline flag, 8-15 shell thickness, 16-23 how strongly to fill the faces
    ivec4 box_max[16];
    // The box every live constraint point fits in. One slab test against this rejects the whole
    // walk for a pixel that cannot be looking at any of them. The points themselves are unbounded
    // and live in the clip cell buffer: marks_min.w is how many GROUPS there are and marks_max.w is
    // where their headers start. See kMarkGroup in src/gpu/render_params.hpp.
    ivec4 marks_min;
    ivec4 marks_max;
    uvec4 clip_slot[16];    // per ghost box: x first cell, yzw size. Size 0 means unused
    uvec4 clip_coarse[16];  // its occupancy mask: x first entry, yzw size in 8-blocks
    // What was just edited and how far its shadow reaches, in voxels relative to the camera
    // chunk. edit_min.w is 1 while it is live.
    ivec4 edit_min;
    ivec4 edit_max;

    // Where the camera was last frame, and how long the shutter is open. See RenderParams.
    vec4 prev_origin;
    vec4 prev_forward;
    vec4 prev_right;
    vec4 prev_up;
    vec4 motion;          // x shutter as a fraction of a frame, y longest streak in pixels,
                          // z which cloud history holds this frame, w how much accumulated weight
                          // an edited world is allowed to keep -- zero means no limit

    // Weather. See shaders/pt_clouds.glsl.
    vec4 sky_cloud;       // x coverage 0 clear to 1 overcast, y time in seconds, zw spare
    vec4 sky_wind;        // xy the low deck's wind in metres a second, zw how far it slid this frame

    // The tone stage's dials. x: the ceiling the light meter may not expose past, as a multiplier,
    // 0 meaning the shader's own default. It is what decides whether a dark room is allowed to be
    // dark -- see kExposureMaxDefault in shaders/resolve.comp. y: how far above its neighbours a
    // face's light has to be before the denoise leaves it out, 0 meaning the shader's own default --
    // see kDenoiseOutlier in shaders/shade_faces.comp. z: how much a face's lobe has to be worth
    // before it asks the pool for a block of outgoing bins, NEGATIVE meaning the shader's own
    // default -- see kLobeWorthFloor in shaders/face_terms.glsl, and note that nought is a real
    // setting there and means every face asks. w spare.
    vec4 tone;

    // ---- R7 primary ray ------------------------------------------------------------------------
    // x: 1 when shaders/beam.comp has written this frame's start distances, 0 for `--no-beam` --
    //    which makes shaders/visibility.comp start every ray at nought, exactly as it always did.
    // y: 1 when the previous frame's depth may lower that start (R7b), 0 for
    //    `--no-temporal-start` and on any frame whose depth image does not hold a usable previous
    //    frame -- the first one after startup or a resize.
    // zw spare.
    vec4 beam;
