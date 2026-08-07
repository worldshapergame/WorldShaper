// The guide buffer's format, in one place.
//
// The tracer writes it, the spatial filter reads it to tell one surface from another, and the
// reprojection reads last frame's copy to decide whether the pixel it landed on is the same
// piece of world. Three readers of one layout, so it is written once here rather than three
// times: a pack and an unpack that disagree produce a picture that is subtly wrong everywhere
// and points at nothing.
//
//   x  normal, octahedral, two 16-bit halves
//   y  distance along the primary ray, as raw float bits. 1e30 means sky
//   z  albedo, 8 bits a channel
//   w  spare

uint pack_normal(vec3 n) {
    // Octahedral: cheap, and a normal is one of six axis directions here anyway.
    vec3 a = n / (abs(n.x) + abs(n.y) + abs(n.z));
    vec2 p = (a.z >= 0.0) ? a.xy
                          : (1.0 - abs(a.yx)) * vec2(a.x >= 0.0 ? 1.0 : -1.0,
                                                     a.y >= 0.0 ? 1.0 : -1.0);
    uvec2 q = uvec2(clamp((p * 0.5 + 0.5) * 65535.0, vec2(0.0), vec2(65535.0)));
    return q.x | (q.y << 16u);
}

vec3 unpack_normal(uint packed) {
    vec2 q = vec2(float(packed & 0xFFFFu), float(packed >> 16u)) / 65535.0 * 2.0 - 1.0;
    vec3 n = vec3(q, 1.0 - abs(q.x) - abs(q.y));
    if (n.z < 0.0) {
        n.xy = (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    }
    return normalize(n);
}

uint pack_albedo(vec3 c) {
    uvec3 q = uvec3(clamp(c, vec3(0.0), vec3(1.0)) * 255.0);
    return q.r | (q.g << 8u) | (q.b << 16u);
}

vec3 unpack_albedo(uint packed) {
    return vec3(float(packed & 0xFFu), float((packed >> 8u) & 0xFFu),
                float((packed >> 16u) & 0xFFu)) / 255.0;
}

// Sky is stored at a distance nothing solid can reach, so one comparison separates the two
// cases everywhere they matter.
const float kGuideSky = 1e29;
