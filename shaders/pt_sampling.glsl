// Where the random numbers come from, and how directions are drawn from them.
//
// Its own module because the quality of the sampling is orthogonal to everything it is used
// for: swapping this hash for a low-discrepancy sequence changes the noise in every estimate
// in the renderer and changes the meaning of none of them.

const float kPi = 3.14159265359;

// ---------------------------------------------------------------------------------------
// Sampling
//
// One stream per pixel per sample, hashed rather than stored. A stored state would need a
// buffer the size of the screen and would serialise nothing useful.
uint pcg(inout uint state) {
    state = state * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

float rand(inout uint state) { return float(pcg(state)) * (1.0 / 4294967296.0); }

// Cosine-weighted hemisphere around n. Cosine-weighted because the diffuse BRDF is divided by
// exactly that cosine, so the two cancel and the estimator is just the albedo ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€šÃ‚Â the lowest
// variance a diffuse bounce can have without knowing where the light is.
// ---- stratification across the pixels of a face ---------------------------------------
//
// A face's answer is the pooled work of every pixel standing on it — often hundreds of them,
// each sending one ray per frame. Drawn independently those rays land where chance puts them:
// three in one part of the hemisphere, none in the next. The face is paying for a hundred
// samples and getting the coverage of far fewer.
//
// Handing each pixel a different cell of the hemisphere fixes that, and costs nothing. The cell
// comes from the pixel's own position, so neighbouring pixels take neighbouring cells and the
// pixels covering a face between them sweep the whole set. This is the ordinary variance
// reduction of stratified sampling, except that the strata are shared out across *pixels*
// rather than across one pixel's samples over time — which is the arrangement that suits a
// cache keyed to faces, because the pooling is what does the averaging.
//
// The assignment walks with the frame, so a single pixel still sees every cell in turn. That
// matters for the small faces: one covered by four pixels gets four cells this frame and four
// others next, rather than the same four for ever.
const uint kStrata = 4u;

vec2 stratified_pair(ivec2 pixel, uint frame, uint salt, inout uint state) {
    uint shift = hash_u32(frame * 0x9E3779B9u + salt);
    // Halved coordinates, because not every pixel traces every frame. The pixels that do are
    // chosen by (3x + 5y + frame) modulo the refine stride, which at stride two is a parity
    // condition on x + y — and a cell index taken straight from x and y is a function of that
    // same parity, so half the cells were unreachable in any given frame. Shifting down by one
    // breaks the correlation: among the pixels that do trace, both halved coordinates still run
    // through every value.
    uint i = ((uint(pixel.x) >> 1) + shift) % kStrata;
    uint j = ((uint(pixel.y) >> 1) + (shift >> 8)) % kStrata;
    // Jittered inside the cell, so it stays an unbiased draw from the whole square rather than
    // a fixed lattice with its own pattern.
    return vec2((float(i) + rand(state)) / float(kStrata),
                (float(j) + rand(state)) / float(kStrata));
}

vec3 cosine_hemisphere_at(vec3 n, vec2 u) {
    float r = sqrt(u.x);
    float phi = 2.0 * kPi * u.y;

    vec3 up = (abs(n.z) < 0.999) ? vec3(0, 0, 1) : vec3(1, 0, 0);
    vec3 tangent = normalize(cross(up, n));
    vec3 bitangent = cross(n, tangent);
    return normalize(tangent * (r * cos(phi)) + bitangent * (r * sin(phi)) +
                     n * sqrt(max(0.0, 1.0 - u.x)));
}

vec3 cosine_hemisphere(vec3 n, inout uint state) {
    return cosine_hemisphere_at(n, vec2(rand(state), rand(state)));
}

// GGX half-vector sample, for the specular lobe.
vec3 ggx_half(vec3 n, float roughness, inout uint state) {
    float a = max(roughness * roughness, 1e-3);
    float u1 = rand(state);
    float u2 = rand(state);
    float cos_theta = sqrt((1.0 - u1) / (1.0 + (a * a - 1.0) * u1));
    float sin_theta = sqrt(max(0.0, 1.0 - cos_theta * cos_theta));
    float phi = 2.0 * kPi * u2;

    vec3 up = (abs(n.z) < 0.999) ? vec3(0, 0, 1) : vec3(1, 0, 0);
    vec3 tangent = normalize(cross(up, n));
    vec3 bitangent = cross(n, tangent);
    return normalize(tangent * (sin_theta * cos(phi)) + bitangent * (sin_theta * sin(phi)) +
                     n * cos_theta);
}

float ggx_smith(float n_dot_v, float n_dot_l, float roughness) {
    float a = max(roughness * roughness, 1e-3);
    float k = a * 0.5;
    float gv = n_dot_v / (n_dot_v * (1.0 - k) + k);
    float gl = n_dot_l / (n_dot_l * (1.0 - k) + k);
    return gv * gl;
}

