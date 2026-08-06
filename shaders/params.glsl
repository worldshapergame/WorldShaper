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
    uvec4 grid_dims;
    ivec4 bounds_min;
    ivec4 bounds_max;
    uvec4 resolution;     // xy pixels, z debug mode, w feedback capacity
    vec4 lens;            // x tan(fov/2), y max distance, z detail bias
    // Unused here, but the block is shared and std140 lays out by position: leave these out
    // and every field below shifts, which draws a broken screen and blames nothing.
    ivec4 thumb_dims;
    ivec4 thumb_tiers[8];
    vec4 tint_visible;    // xyz colour, w = 1 to use it, 0 to invert the backdrop instead
    vec4 tint_occluded;
    ivec4 box_min[16];    // xyz voxels relative to the camera chunk; w: 0 unused 1 carve 2 place 3 refused 4 idle
    ivec4 box_max[16];
    ivec4 marks[8];       // constraint points, w = 1 when used
    uvec4 clip_slot[16];    // per ghost box: x first cell, yzw size. Size 0 means unused
    uvec4 clip_coarse[16];  // its occupancy mask: x first entry, yzw size in 8-blocks
