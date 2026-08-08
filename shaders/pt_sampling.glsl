// Where the random numbers come from, and how directions are drawn from them.
//
// Its own module because the quality of the sampling is orthogonal to everything it is used
// for: swapping this hash for a low-discrepancy sequence changes the noise in every estimate
// in the renderer and changes the meaning of none of them.

const float kPi = 3.14159265359;

// ---------------------------------------------------------------------------------------
// Decisions
//
// One stream per pixel per sample, hashed rather than stored. A stored state would need a
// buffer the size of the screen and would serialise nothing useful.
//
// This is the *decision* stream and deliberately not the integration stream: which lobe a
// bounce takes, whether a face refines this frame, whether the path asks a lamp. Those are coin
// flips, and a coin flip is the one draw a low-discrepancy sequence must not be given. Make the
// flip agree between neighbouring pixels and the choice itself becomes a picture - contiguous
// patches all taking the specular branch, all asking the same lamp - and a structured choice is
// far worse to look at than a noisy one. So the flips stay independent and hashed, and the
// sequence below is spent where it pays: on the integrals.
uint pcg(inout uint state) {
    state = state * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

float rand(inout uint state) { return float(pcg(state)) * (1.0 / 4294967296.0); }

// ---------------------------------------------------------------------------------------
// Integration: an Owen-scrambled Sobol sequence
//
// Dimension zero of the Sobol sequence is the radical inverse in base two; dimension one is its
// (0,2)-sequence partner. Together they are the standard low-discrepancy pair, and their
// guarantee is that any block of 2^m consecutive indices starting at a multiple of 2^m puts
// exactly one point in every elementary rectangle of that size.
uint sobol_vdc(uint i) { return bitfieldReverse(i); }

// The partner dimension. Direction numbers v_k = v_{k-1} ^ (v_{k-1} >> 1) starting from the top
// bit; index bit k selects v_k. Written as a fixed thirty-two turns rather than the usual
// early-out loop because every lane in a warp then runs the same count, and the branchy version
// buys nothing on a machine that has to wait for the slowest lane anyway.
uint sobol_partner(uint i) {
    uint r = 0u;
    uint v = 0x80000000u;
    for (uint k = 0u; k < 32u; ++k) {
        if ((i & 1u) != 0u) r ^= v;
        i >>= 1u;
        v ^= v >> 1u;
    }
    return r;
}

// Owen scrambling, the hash-based form: a random permutation of the sequence in which the
// permutation applied to each bit depends on every bit above it.
//
// A plain XOR scramble would decorrelate two pixels' sequences but leave Sobol's structure
// intact, and that structure is visible - it reads as faint diagonal texture rather than as
// noise. Owen scrambling randomises the sequence *within* every stratum it defines while
// leaving the strata themselves where they are, so what survives is the even coverage and what
// is destroyed is the pattern.
//
// The nesting is what does the work, and it is worth being precise about because the rest of
// this file leans on it: the multiplies and the add carry information from low bits to high and
// never the other way, so after the two bit-reversals a result's leading bits depend only on the
// input's leading bits. Two indices that agree at the top land in the same stratum and at
// independent places inside it. That is the property; it is not incidental.
uint owen_scramble(uint x, uint seed) {
    x = bitfieldReverse(x);
    x ^= x * 0x3d20adeau;
    x += seed;
    x *= (seed >> 16) | 1u;
    x ^= x * 0x05526c56u;
    x ^= x * 0x53a22864u;
    return bitfieldReverse(x);
}

// ---- stratification across the pixels of a face ---------------------------------------
//
// A face's answer is the pooled work of every pixel standing on it - often hundreds of them,
// each sending one ray per frame. Drawn independently those rays land where chance puts them:
// three in one part of the hemisphere, none in the next. The face is paying for a hundred
// samples and getting the coverage of far fewer.
//
// Handing each pixel a different cell of the hemisphere fixes that, and costs nothing. The cell
// comes from the pixel's own position, so neighbouring pixels take neighbouring cells and the
// pixels covering a face between them sweep the whole set. This is the ordinary variance
// reduction of stratified sampling, except that the strata are shared out across *pixels*
// rather than across one pixel's samples over time - which is the arrangement that suits a
// cache keyed to faces, because the pooling is what does the averaging.
//
// Two things about the arrangement were measured rather than reasoned, and both are load
// bearing. The measurement bins a face's patch of pixels into a k-by-k grid of the unit square
// and takes the chi-square deviation from perfectly even coverage - zero is perfect - averaged
// over every offset the patch could have and over ninety-six frames.
//
//   patch of cells    bins   4x4 grid   Sobol in Morton order   this
//   2x2  (4 rays)      2x2     1.2500          0.2812          0.0000
//   2x3  (6 rays)      2x2     1.0000          0.1667          0.1667
//   4x4  (16 rays)     4x4     0.0000          1.1094          0.0000
//   5x5  (25 rays)     4x4     0.3975          0.8725          0.3975
//   8x8  (64 rays)     8x8     0.7469          0.6406          0.0000
//
// The first thing is that the cell index must be TRANSLATION INVARIANT. A voxel face does not
// begin on a power-of-two pixel boundary; it begins wherever the geometry put it. Feeding the
// pixels' Morton index to the Sobol sequence - which was tried, and is the obvious way to use
// one - gives a perfect net on aligned runs and nothing in particular on the offset ones a face
// actually occupies: the 1.11 and 0.87 in the middle column, where a plain modulo grid scores
// 0.00 and 0.40. Rendered, that version won five per cent on a distant view where a face covers
// about one cell and lost nineteen on a near one where it covers several. A modulo grid has no
// alignment to miss.
//
// The second is the ORDER the cells are visited in. A plain modulo grid hands adjacent pixels
// adjacent strata, so the four rays of a small face land in one corner of the hemisphere - the
// 1.25 in the first column. Reversing the bits of the cell index fixes it for nothing: adjacent
// cells then map to strata half the range apart, while any eight consecutive still cover all
// eight, because bit reversal is a permutation. That is the whole difference between the first
// and last columns at 2x2, and it costs three shifts.
const uint kStrata = 8u;

// Three bits, reversed. Neighbouring cells land several strata apart instead of side by side -
// four of the eight on average - without disturbing which cells a run of pixels covers.
uint reverse3(uint v) { return ((v & 1u) << 2) | (v & 2u) | ((v >> 2) & 1u); }

vec2 stratified_pair(ivec2 pixel, uint frame, uint salt) {
    // Halved coordinates, because not every pixel traces every frame. The pixels that do are
    // chosen by (3x + 5y + frame) modulo the refine stride, which at stride two is a parity
    // condition on x + y - and a cell index taken straight from x and y is a function of that
    // same parity, so half the cells were unreachable in any given frame. Shifting down by one
    // breaks the correlation: among the pixels that do trace, both halved coordinates still run
    // through every value.
    uvec2 half_px = uvec2(pixel) >> 1;

    // The assignment walks with the frame, so a single pixel still sees every cell in turn. That
    // matters for the small faces: one covered by four pixels gets four cells this frame and four
    // others next, rather than the same four for ever.
    //
    // Counted, not hashed, and the two axes count at different rates: x advances every frame and
    // y every eighth, so a pixel visits all sixty-four cells of the grid exactly once every
    // sixty-four frames. An independent draw per frame would stumble over about two thirds of
    // them in the same time and revisit the rest. The hash only randomises which order each run
    // of sixty-four goes in, so two faces do not march in step for ever.
    uint jumble = hash_u32((frame >> 6) * 0x9E3779B9u + salt);
    uint i = reverse3((half_px.x + frame + jumble) & (kStrata - 1u));
    uint j = reverse3((half_px.y + (frame >> 3) + (jumble >> 8)) & (kStrata - 1u));

    // Inside the cell, an Owen-scrambled Sobol pair walked by the frame rather than a fresh
    // random number. Two pixels that share a cell - they are sixteen apart, so a large face has
    // many such pairs - get independent positions in it from their own scramble, and one pixel's
    // positions over frames form a low-discrepancy sequence instead of a scatter. That is the
    // part of the estimate that a hash could never improve with more samples, only average.
    uint seed = hash_u32(hash_u32(half_px.x * 0x9E3779B9u + half_px.y) + salt * 0x85EBCA6Bu);
    vec2 sub = vec2(float(owen_scramble(sobol_vdc(frame), seed)),
                    float(owen_scramble(sobol_partner(frame), seed ^ 0x632BE59Bu))) *
               (1.0 / 4294967296.0);

    return (vec2(float(i), float(j)) + sub) * (1.0 / float(kStrata));
}

// Kept so the call sites that pass a stream do not have to change. The jitter that stream used
// to supply is now the Sobol pair above, which is a better jitter for the same price: it is
// stratified over frames rather than redrawn from scratch each one.
vec2 stratified_pair(ivec2 pixel, uint frame, uint salt, inout uint state) {
    return stratified_pair(pixel, frame, salt);
}

// Anti-aliasing is the one draw that must NOT be shared across pixels: a pixel's footprint is
// its own, and there is no pooling to serve. So this is the other arrangement - the pixel keeps
// a scramble of its own and walks the sequence with the sample count, which is the textbook
// progressive Sobol and covers a pixel's area far more evenly than a hashed pair does.
vec2 pixel_jitter(ivec2 pixel, uint sample_index) {
    uint seed = hash_u32(uint(pixel.x) * 0x9E3779B9u + uint(pixel.y));
    return vec2(float(owen_scramble(sobol_vdc(sample_index), seed)),
                float(owen_scramble(sobol_partner(sample_index), seed ^ 0x632BE59Bu))) *
           (1.0 / 4294967296.0);
}

// Cosine-weighted hemisphere around n. Cosine-weighted because the diffuse BRDF is divided by
// exactly that cosine, so the two cancel and the estimator is just the albedo - the lowest
// variance a diffuse bounce can have without knowing where the light is.
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

// GGX half-vector sample, for the specular lobe. Split from its draw the way the hemisphere is,
// so a call site that knows which pixel it is can hand it a stratified pair instead of a hash.
vec3 ggx_half_at(vec3 n, float roughness, vec2 u) {
    float a = max(roughness * roughness, 1e-3);
    float cos_theta = sqrt((1.0 - u.x) / (1.0 + (a * a - 1.0) * u.x));
    float sin_theta = sqrt(max(0.0, 1.0 - cos_theta * cos_theta));
    float phi = 2.0 * kPi * u.y;

    vec3 up = (abs(n.z) < 0.999) ? vec3(0, 0, 1) : vec3(1, 0, 0);
    vec3 tangent = normalize(cross(up, n));
    vec3 bitangent = cross(n, tangent);
    return normalize(tangent * (sin_theta * cos(phi)) + bitangent * (sin_theta * sin(phi)) +
                     n * cos_theta);
}

vec3 ggx_half(vec3 n, float roughness, inout uint state) {
    return ggx_half_at(n, roughness, vec2(rand(state), rand(state)));
}

float ggx_smith(float n_dot_v, float n_dot_l, float roughness) {
    float a = max(roughness * roughness, 1e-3);
    float k = a * 0.5;
    float gv = n_dot_v / (n_dot_v * (1.0 - k) + k);
    float gl = n_dot_l / (n_dot_l * (1.0 - k) + k);
    return gv * gl;
}
