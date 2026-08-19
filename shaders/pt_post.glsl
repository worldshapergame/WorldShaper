// Everything between a radiance value and a pixel on the screen.
//
// Its own module because it is a different subject from light transport: exposure, tone mapping,
// glare, the lens. Transport answers "how much light arrives"; this answers "what should that
// look like", and the two are argued about on completely different grounds.
//
// The order below is the order a camera does it in, and it is not arbitrary:
//
//   reject outliers -> accumulate -> expose -> glare -> tone map -> gamma -> shutter
//
// Rejection is before the average because an average is what it is protecting. Exposure is a
// property of the whole frame and so is read from the frame before it. Glare happens in the
// lens, which is in front of the film, so it belongs on the exposed image and is then tone
// mapped with it -- bloom applied after the curve is bloom that cannot blow anything out, which
// is the one thing bloom is for. The shutter is last because it is the only step that is about
// TIME rather than about a single instant of light.
//
// # What this file is now, and what it was
//
// It was the path tracer's post stage, with the accumulation image, the light meter and the
// debug tools all in it, and R3d deleted the pass that included it. What survived is what §9 of
// documentation/21-renderer-rewrite.md kept it for:
//
//   - **the light meter**, which is not here any more: D577 moved that arithmetic into
//     `shaders/resolve.comp`, because it needs the pre-exposure radiance and that exists in
//     exactly one place for exactly one instruction's worth of time;
//   - **the curve**, which `resolve.comp` also carries a copy of, for the same reason;
//   - **glare and the shutter**, which are R6a and R6b and which are what is left here.
//
// So this file is now a HEADER OF ARITHMETIC AND NOTHING ELSE. It declares no binding, reads no
// image and names no push constant, and the three passes that include it -- `bloom_down.comp`,
// `bloom_up.comp` and `post.comp` -- supply their own. That is the shape the old one could not
// have: it read `accum` directly, so every function in it belonged to one pass.
//
// # And the gather is gone
//
// The bloom that was here took up to **437 taps a pixel at 1440p**, straight out of the
// accumulation image, and it was dense on purpose: one pass and no chain means a sparse kernel
// does not blur a small bright thing, it *copies* it, and a specular glint comes out as a
// handful of discrete ghosts in a pattern that never moves and so never washes out. That
// constraint shaped everything about it, and it is a constraint that a chain simply does not
// have. R6a replaces it with successive halvings: the taps are the same few per pixel at every
// level, and the reach comes from the levels rather than from the radius.
//
// What is kept from it, exactly, so the look does not move:
//
//   - `kBloomThreshold` is in EXPOSED units, so it follows the light meter rather than sitting
//     at a fixed brightness. A scene the meter lifts pushes more pixels over it, which is what
//     makes glare a property of the picture rather than of the numbers behind it.
//   - `kBloomKnee`, so a surface drifting across the threshold does not switch glare on like a
//     light.
//   - `kBloomIntensity` at ten per cent, and the normalisation that makes one number serve both
//     cases: a flat expanse over the threshold gets back exactly that fraction of itself -- haze
//     rather than a second sun -- while a small bright thing glares hard, because a chain
//     summed over its levels and divided by their count is peaked in exactly that way. Small and
//     bright glares hard, large and bright glares gently. That is what a lens does.

// ---------------------------------------------------------------------------------------
// Luminance.

// The weights every luminance in this renderer uses. Named here rather than pulled in from
// face_terms.glsl, which brings a hundred functions and a second copy of kPi with it.
float post_luminance(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }

// ---------------------------------------------------------------------------------------
// The curve.

// ACES, by Stephen Hill's fit of the reference transform.
//
// It replaces Reinhard-with-a-shoulder for one reason that matters more than taste: Reinhard is
// applied per channel, so a saturated red at four times white maps its red to 0.8 and leaves
// green and blue near zero, and the highlight stays fully saturated red no matter how bright it
// gets. Nothing real does that. Bright things desaturate towards white, and the two matrices
// here are what make them: the curve is applied in a working space where the channels are
// mixed, so a hot red rolls off through orange the way film and eyes both do.
//
// The same fit as `resolve.comp`'s, deliberately: this file has to be able to say where a
// threshold written in exposed units LANDS after the composite has already applied the curve,
// and it can only do that by applying the identical curve.
vec3 tonemap_aces(vec3 colour) {
    const mat3 to_working = mat3(0.59719, 0.07600, 0.02840,
                                 0.35458, 0.90834, 0.13383,
                                 0.04823, 0.01566, 0.83777);
    const mat3 from_working = mat3( 1.60475, -0.10208, -0.00327,
                                   -0.53108,  1.10813, -0.07276,
                                   -0.07367, -0.00605,  1.07602);
    vec3 v = to_working * colour;
    vec3 a = v * (v + 0.0245786) - 0.000090537;
    vec3 b = v * (0.983729 * v + 0.4329510) + 0.238081;
    return clamp(from_working * (a / b), 0.0, 1.0);
}

// Reinhard with a shoulder: what this renderer used before there was an exposure to put in
// front of it, kept so a material can be looked at without a curve having an opinion about it.
vec3 tonemap_plain(vec3 colour) {
    return clamp(colour / (colour + vec3(1.0)), 0.0, 1.0);
}

// ---------------------------------------------------------------------------------------
// The gamma the composite writes with, and its inverse.
//
// `resolve.comp` ends `pow(tonemap_aces(radiance * exposure), 1.0 / 2.2)` into an R8G8B8A8_UNORM
// image. Plain 2.2 and not the piecewise sRGB curve, because matching what the composite
// actually did matters far more here than which of the two is nominally correct: post reads that
// image back, and a mismatched decode is a brightness edit hiding inside a blur.
vec3 display_to_linear(vec3 encoded) { return pow(max(encoded, vec3(0.0)), vec3(2.2)); }
vec3 linear_to_display(vec3 linear) { return pow(max(linear, vec3(0.0)), vec3(1.0 / 2.2)); }

// ---------------------------------------------------------------------------------------
// Glare.

const float kBloomIntensity = 0.10;
const float kBloomThreshold = 1.0;    // in exposed units, so it follows the light meter
const float kBloomKnee = 0.6;

// Where those two land AFTER the curve, which is the space the chain actually works in.
//
// The honest version of this pass takes linear radiance from the composite and hands the curve
// back to post, so glare is added before the tone map -- and that needs `resolve.comp` to stop
// tone mapping, which is a change to a file another pass owns. Until it happens, the chain reads
// the display image and the threshold has to be carried across the curve rather than used raw:
// `kBloomThreshold` in exposed units is `tonemap_aces(kBloomThreshold)` once the composite has
// been through it, and a threshold applied to display values without that mapping would sit at a
// completely different brightness.
//
// Carried rather than written down as a second constant, so the pair above stay the only numbers
// anybody has to argue about. It is about twenty instructions once per invocation -- and both at
// once, because the knee is defined in terms of the threshold and asking for them separately
// evaluates the curve three times for two numbers.
void bloom_display_limits(out float threshold, out float knee) {
    threshold = post_luminance(tonemap_aces(vec3(kBloomThreshold)));
    float low = post_luminance(tonemap_aces(vec3(max(kBloomThreshold - kBloomKnee, 0.0))));
    knee = max(threshold - low, 1e-3);
}

// What of a pixel bleeds: the part above the threshold, with a soft shoulder so that a surface
// drifting across it does not switch glare on like a light.
vec3 bloom_source(vec3 colour, float threshold, float knee) {
    float lum = post_luminance(colour);
    float over = lum - threshold + knee;
    if (over <= 0.0) return vec3(0.0);
    // Quadratic in the knee, linear above it, and the two meet with the same slope.
    float weight = (over < 2.0 * knee) ? (over * over / (4.0 * knee)) : (lum - threshold);
    return colour * (weight / max(lum, 1e-4));
}

// ---------------------------------------------------------------------------------------
// The chain's kernels.
//
// Offsets in TEXELS of the image being read, and weights that sum to one, so a flat region
// survives every step of the chain at exactly its own value. That is the property the
// normalisation above depends on: sum the levels, divide by how many there were, and a flat
// expanse over the threshold comes back as itself.

// The halving step, for every level below the first: a four-by-four tent, separable [1,3,3,1],
// centred on the two-by-two block the destination pixel stands for. Wide enough that the level
// below it is smooth rather than sampled, which is what stops the chain from stepping in blocks
// when the camera moves.
//
// Divided by EIGHT and not by sixteen, and it was written wrong first. The two axes multiply, so
// a per-axis kernel summing to a half makes every level below the first a quarter of the one
// above it: the chain reads D0 + D1/4 + D2/16 + ... instead of D0 + D1 + D2 + ... That is not a
// dimmer glare, it is a glare with no reach, because the levels that carry the reach are exactly
// the ones being divided away -- and it looks like a plausible picture, so the only thing that
// catches it is checking that the weights sum to one before believing one.
const int kBloomDownSpan = 4;
const float kBloomDownWeight[4] = float[4](0.125, 0.375, 0.375, 0.125);

// The upsample: three coarse texels each way, weighted by where the fine pixel actually falls
// between them -- a quadratic B-spline, which is the smoothest kernel that fits in three taps and
// still sums to one at every sub-texel position.
//
// A tent rather than a box, and the phase rather than a fixed pattern, because both of the cheap
// alternatives are visible: a box upsample reproduces the coarse level's own grid as squares --
// at six levels the coarsest square is a sixty-fourth of the screen -- and a fixed-phase tent
// makes the glare crawl in steps of two pixels when the camera pans.
vec3 bloom_up_weights(float f) {
    return vec3(0.5 * (1.0 - f) * (1.0 - f),
                0.75 - (f - 0.5) * (f - 0.5),
                0.5 * f * f);
}

// ---------------------------------------------------------------------------------------
// The shutter.
//
// It is a CAMERA blur and not a per-object one, which for this game is the whole of it: nothing
// in a voxel world moves except the player's own view, and the view is exactly what a shutter
// would smear. A wall you turn past streaks, a wall you walk towards barely does, and a wall you
// stand still in front of does not at all -- because the velocity IS the speed, so "speed based"
// needs no separate term.
//
// How fast what this pixel is looking at is moving across the screen, in pixels a frame. The ray
// and the distance to what it hit give the point's position in the world; projecting that
// through where the camera was last frame says where it appeared then; the difference is the
// velocity. Nothing is stored per pixel, no buffer is kept between frames, and no pass is added
// for it -- the previous camera is four vec4s in a block that was already there.
//
// `prev_*` are in the same space as `origin`: voxels relative to the CURRENT camera chunk, which
// the host has already corrected for a chunk crossing (see main.cpp, where the correction is,
// and the fault it fixes -- a step across a boundary smearing the whole screen for one frame).
vec2 post_screen_velocity(vec3 world_point, ivec2 pixel, vec2 resolution, float aspect,
                          float tan_half_fov, vec3 prev_origin, vec3 prev_forward,
                          vec3 prev_right, vec3 prev_up) {
    vec3 relative = world_point - prev_origin;
    float ahead = dot(relative, prev_forward);
    // Behind the old camera, or so close to its plane that the projection blows up. A point that
    // was not on screen last frame has no velocity worth trusting, and inventing one streaks the
    // edge of the screen every time the player turns.
    if (ahead < 1e-3) return vec2(0.0);

    float across = dot(relative, prev_right);
    float upward = dot(relative, prev_up);
    vec2 ndc = vec2(across / (ahead * tan_half_fov * aspect), -upward / (ahead * tan_half_fov));
    vec2 was = (ndc * 0.5 + 0.5) * resolution;
    return (vec2(pixel) + 0.5) - was;
}
