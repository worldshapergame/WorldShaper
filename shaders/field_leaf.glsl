// The leaves: every op with no children, evaluated directly. `Field::eval`'s leaf cases.
//
// See field_types.glsl for what this file is and the rule that governs it — when this and
// `src/forge/field.cpp` disagree, field.cpp is right.
//
// STUB. The contract is fixed and the arithmetic is not written yet; every op answers "far away",
// which is a value no gate will mistake for agreement.

#ifndef WS_FIELD_LEAF_GLSL
#define WS_FIELD_LEAF_GLSL

#include "field_types.glsl"

float field_leaf(uint at, vec3 p) {
    return 1.0e30;
}

#endif   // WS_FIELD_LEAF_GLSL
