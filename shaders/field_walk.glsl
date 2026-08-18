// The walk: one loop over an explicit stack, over every op that has children.
// `Field::mirror_eval`, transliterated.
//
// See field_types.glsl for what this file is and the rule that governs it — when this and
// `src/forge/field.cpp` disagree, field.cpp is right.
//
// STUB. The contract is fixed and the machine is not written yet; it walks nothing and answers
// "far away", which is a value no gate will mistake for agreement.

#ifndef WS_FIELD_WALK_GLSL
#define WS_FIELD_WALK_GLSL

#include "field_types.glsl"

float field_eval(uint root, vec3 p) {
    return 1.0e30;
}

#endif   // WS_FIELD_WALK_GLSL
