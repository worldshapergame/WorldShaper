// Everything between a radiance value and a pixel on the screen.
//
// Its own module because it is a different subject from light transport: exposure, tone mapping,
// glare, the lens. Transport answers "how much light arrives"; this answers "what should that
// look like", and the two are argued about on completely different grounds.
//
// The order below is the order a camera does it in, and it is not arbitrary:
//
//   reject outliers -> accumulate -> expose -> glare -> tone map -> gamma -> tools
//
// Rejection is before the average because an average is what it is protecting. Exposure is a
// property of the whole frame and so is read from the frame before it. Glare happens in the
// lens, which is in front of the film, so it is added to the exposed image and then tone
// mapped with it -- bloom applied after the curve is bloom that cannot blow anything out, which
// is the one thing bloom is for. The tools are last because they are interface, not light.

// What the frame turned out to look like, added up while it is drawn. See the long note in
// gpu/render_params.hpp: slot 0 is this frame and is being written by every invocation; slot 1
// is the frame before it, finished, and is written by nobody while it is read.
//
// Declared as four named words rather than a uvec4 because atomicAdd wants a single component
// of a buffer variable and naming it says which quantity is being added to.
struct FrameStatsEntry {
    uint log_luminance;
    uint groups;
    uint exposure;
    uint focus;
};
layout(std430, binding = 19) buffer FrameStats { FrameStatsEntry slots[]; } stats;

// The fixed-point conventions, and they must match gpu/render_params.hpp exactly. A shader and
// a header disagreeing about a scale factor does not fail, it just exposes wrongly.
const float kLogLuminanceBias = 16.0;
const float kLogLuminanceUnit = 256.0;
const float kExposureUnit = 65536.0;

// The window of the light meter, in stops either side of one.
//
// Narrower than the [-16, +16] the fixed point can hold, and deliberately. A log average is
// dragged by its darkest pixels as hard as by its brightest, and a room has corners that are
// genuinely thirty stops down -- unlit, unreachable, and not what anyone is looking at. Pinned
// at -10 they still say "this frame is dark" without being allowed to say it thirty times over.
const float kMeterFloor = -10.0;
const float kMeterCeiling = 12.0;

// What the log-average of the frame is exposed to.
//
// 0.18 is the photographic middle grey and the reason to start there rather than anywhere else.
// The ACES curve lands 0.18 near 0.10 linear, which is 0.35 after gamma -- a little under the
// 0.46 an sRGB mid-grey chart patch sits at, and that is the ACES look rather than an error.
const float kMiddleGrey = 0.18;

// How far towards the new measurement one frame moves.
//
// Per frame and not per second, because this shader is given a frame counter and no clock. At
// the 60 Hz the game runs at, 0.033 is a time constant of half a second: walking from a lit
// street into a doorway takes about a second to settle, which is roughly what an eye does and
// well short of what a camera does. If a delta time ever reaches the push block this should
// become 1 - exp(-dt / 0.5) and the constant deleted.
const float kAdaptRate = 0.033;

// The bounds the exposure is allowed inside, as multipliers. A frame with nothing in it must
// not send this to infinity and then to NaN, and a frame that is genuinely all sky must not
// have its exposure pulled so far down that the next frame takes a visible age to come back.
const float kExposureMin = 0.0039;   // -8 stops
const float kExposureMax = 4096.0;   // +12 stops

// ---------------------------------------------------------------------------------------
// The light meter.

// The exposure multiplier for this frame, computed identically by every invocation.
//
// Identically matters more than it looks. Every invocation reads slot 1, which is complete and
// which nothing writes during the dispatch, so every invocation gets the same words and the
// same answer. Read slot 0 instead -- the one being added to -- and each invocation would see
// however much of the frame happened to have run by the time it looked, so the same scene would
// expose differently across the picture and differently again on another card. That is a
// gradient across the image that would get called a shading bug.
float frame_exposure() {
    uint sum = stats.slots[1].log_luminance;
    uint groups = stats.slots[1].groups;
    uint previous = stats.slots[1].exposure;

    // No measurement means the very first frame, before anything has been drawn. Whatever was
    // in use stays in use; if nothing was, 1.0 is what this renderer always did.
    if (groups == 0u) {
        return (previous == 0u) ? 1.0 : float(previous) / kExposureUnit;
    }

    // log_luminance / groups is the mean of the biased fixed-point value, by construction: see
    // the note on how it is accumulated at the bottom of this file.
    float mean = float(sum) / float(groups);
    float average_stops = mean / kLogLuminanceUnit - kLogLuminanceBias;
    float target = clamp(kMiddleGrey / exp2(average_stops), kExposureMin, kExposureMax);

    // A measurement but no previous exposure is the first frame that has anything to go on, and
    // it snaps. Easing from the 1.0 of a frame that had not been drawn yet is easing away from
    // a number that never meant anything, and it shows up as the picture fading in.
    if (previous == 0u) return target;

    // In stops, not in multipliers. Adaptation is logarithmic -- two stops down from bright is
    // the same amount of adjusting as two stops up from dark -- and blending the multipliers
    // linearly makes coming out of a dark room take several times longer than going in.
    float from = log2(clamp(float(previous) / kExposureUnit, kExposureMin, kExposureMax));
    return exp2(mix(from, log2(target), kAdaptRate));
}

// ---------------------------------------------------------------------------------------
// The lens.

// Where the camera is focused, in voxels, as the previous frame measured it. Zero means it has
// not measured yet, and a caller must read that as "focus at infinity" rather than "focus at
// nothing" -- the same distinction the transport code makes between no answer and zero.
const float kFocusUnit = 16.0;      // 1/16 of a voxel is about two millimetres at metre 32
const float kFocusMax = 262143.0;   // times the unit, this stays well inside a 32-bit word

float focus_distance() {
    uint stored = stats.slots[1].focus;
    return (stored == 0u) ? 0.0 : float(stored) / kFocusUnit;
}

// The primary ray, aperture and all.
//
// Lives here rather than in the tracer because a lens is not transport: it decides which ray
// stands for this pixel, not what happens to it afterwards, and that is the same question
// exposure and the tone curve answer. It is also the only part of the camera that has to agree
// with the pixel filter, and the pixel filter is the jitter below.
//
// push.lens.w is the aperture radius in voxels, and zero is a pinhole -- which is what the
// parameter block is value-initialised to, so this is exactly the old behaviour until somebody
// sets it.
//
// It has to be a much larger number than photographic instinct suggests, and the arithmetic is
// worth writing down rather than rediscovering. The blur circle a point at distance d makes,
// in pixels, is
//
//     height * aperture * |d - focus| / (d * d * tan(fov/2))
//
// so at this 90-degree lens and 460 pixels high, an aperture of 0.9 voxels focused at 5.6 m
// leaves the far end of mirror_test blurred by half a pixel -- invisible, and measured. Ten
// voxels puts it at six pixels, which is the render that shows. Wide-angle lenses have enormous
// depth of field and this one is no exception; a shallow look needs an aperture that would be
// absurd on a portrait lens.
//
// The focal plane is a plane and not a sphere. Dividing by the cosine to the forward axis is
// what puts the corners of the image in focus at the same time as the centre; without it a
// flat wall square to the camera goes soft towards the edges, which reads as a lens fault.
void camera_ray(ivec2 pixel, inout uint state, float aspect, out vec3 origin, out vec3 dir) {
    // Jitter inside the pixel, which is the whole of the anti-aliasing: with enough samples the
    // average over the pixel's area is the correct answer, and no edge filter is needed.
    vec2 jitter = vec2(rand(state), rand(state));
    vec2 uv = (vec2(pixel) + jitter) / vec2(push.resolution) * 2.0 - 1.0;

    vec3 straight = normalize(push.forward.xyz + push.right.xyz * uv.x * push.lens.x * aspect -
                              push.up.xyz * uv.y * push.lens.x);
    origin = push.origin.xyz;
    dir = straight;

    float aperture = push.lens.w;
    float focus = focus_distance();
    if (aperture <= 0.0 || focus <= 0.0) return;   // a pinhole, and nothing to pay for one

    // Uniform over the disc: sqrt of the radial sample, or the middle of the aperture would be
    // sampled as heavily as the whole of its rim and the blur would have a bright core.
    float radius = aperture * sqrt(rand(state));
    float angle = 6.28318530718 * rand(state);
    vec3 shift = push.right.xyz * (radius * cos(angle)) + push.up.xyz * (radius * sin(angle));

    float forward_cos = max(dot(straight, normalize(push.forward.xyz)), 1e-4);
    vec3 focal_point = origin + straight * (focus / forward_cos);
    origin += shift;
    dir = normalize(focal_point - origin);
}

// ---------------------------------------------------------------------------------------
// Outlier rejection.

// How far above what this corner of the picture has already settled on a single sample may be
// before it is treated as an outlier rather than as news.
//
// This replaces a constant clamp, and the difference is the point. A clamp at 8 says "no pixel
// anywhere is brighter than 8", which is false the moment the sun is caught in chrome, and it
// throws that light away in the one place it was real.
//
// The first version of this said "no sample is more than sixteen times the local mean", and it
// was the same mistake in relative clothing. A path tracer's samples are *supposed* to be many
// times their own mean: a wall lit only through a window is black on most of its samples and
// bright on the few that find the window, and the mean is low precisely because the bright ones
// are rare. Sixteen times the mean caught the carriers rather than the fireflies, and it cost
// the exposure_range room 1.9 stops -- measured, by rendering the same frame with the test off
// and comparing the log-average. A test that quietly removes the light a room is lit by is
// worse than the clamp it replaced, because it looks like an answer.
//
// So the bound grows with the sample count, and that is the whole idea. What makes a firefly a
// firefly is not that it is bright, it is that it is bright enough to *redefine the pixel on its
// own*: with N samples already in, a sample contributes S/(N+1) to the mean, so one worth more
// than the entire rest of the average put together is the one to catch. That is a bound
// proportional to N, and it has the property the constant ratio did not -- as a pixel
// converges, the test stops touching anything real at all, so the estimator it protects stays
// unbiased where it matters.
const float kFireflyRatio = 12.0;   // the tolerance before any samples have accumulated
const float kFireflyLift = 4.0;     // and how fast it opens up per sample after that

// A floor under all of it, so a pixel whose neighbourhood is genuinely black still has a
// tolerance and does not refuse the first real light that reaches it. Without this the darkest
// corner of a room would reject every sample that could ever brighten it and stay black.
const float kFireflyFloor = 0.5;

// What happens above the threshold, and it is not a clamp either.
//
// The excess is square-rooted rather than cut off: a sample sixteen times over the limit still
// arrives four times brighter than one twice over it, so the ordering of real light survives
// and the energy is compressed instead of discarded. A hard cut is what makes a clamped render
// lose the sun; this only makes it lose the *spike*.
vec3 reject_outlier(ivec2 pixel, vec3 value, vec4 total) {
    // Nothing to compare against on the first sample. Zero and "no answer yet" are different,
    // and inventing a reference here would be inventing one -- it would reject the very first
    // sample of every bright thing in the scene, which is how a picture starts dark and stays
    // that way. The tracer's own clamps still cover this frame.
    if (total.w < 1.0) return value;

    float lum = face_luminance(value);
    if (lum <= kFireflyFloor) return value;

    // The reference is the brightest mean in the neighbourhood, not this pixel's alone, and the
    // max rather than the average. An outlier test that throws away anything brighter than the
    // local average would shave every specular edge in the frame, because an edge is exactly
    // where one pixel is legitimately far brighter than the pixel beside it. Taking the max
    // means a sample only has to agree with *something* nearby to be kept.
    float reference = face_luminance(total.rgb / total.w);
    ivec2 limit = ivec2(push.resolution.xy) - 1;
    for (uint i = 0u; i < 4u; ++i) {
        ivec2 step = (i == 0u) ? ivec2(1, 0) : (i == 1u) ? ivec2(-1, 0)
                   : (i == 2u) ? ivec2(0, 1) : ivec2(0, -1);
        vec4 near = imageLoad(accum, clamp(pixel + step, ivec2(0), limit));
        // A neighbour with no samples yet says nothing, and must not be read as saying black.
        if (near.w >= 1.0) reference = max(reference, face_luminance(near.rgb / near.w));
    }

    // Proportional to how much is already in the average, so that a sample is only refused when
    // it would be worth more than everything the pixel has learned so far.
    float allowed = max(reference, 0.0) * (kFireflyRatio + kFireflyLift * total.w) + kFireflyFloor;
    if (lum <= allowed) return value;

    // Scaled, so the colour of the sample survives being dimmed. Clamping the channels one at a
    // time is what turns an orange spark white on its way out.
    return value * (allowed * sqrt(lum / allowed) / lum);
}

// ---------------------------------------------------------------------------------------
// Glare.

// One pass and no mip chain, so this is a gather rather than a blur: the taps read straight out
// of the accumulation buffer. That constraint is what shapes everything below.
//
// A sparse kernel is what a single pass usually reaches for, and it cannot be used here. Taps
// spread thinly over a wide disc do not blur a small bright thing, they *copy* it: a specular
// glint is caught by whichever few taps happen to land on it and comes out as a handful of
// discrete ghosts around it, in a fixed pattern that does not move and so never washes out. The
// first version of this was 48 taps on a spiral and that is exactly what it drew.
//
// So the kernel is dense where it is strong and sparse only where it is weak. The core is every
// pixel inside its radius -- no gaps, nothing to alias -- and carries about 98 per cent of the
// weight. The tail is three rings of eight beyond it, and it can be sparse precisely because it
// is faint: a point source a hundred times over the threshold puts 0.002 into a tail tap, which
// is below a step of an 8-bit channel.
//
// Reading the accumulation buffer while other invocations are writing it is safe here and only
// here: an entry is a mean over up to 1024 samples, so a neighbour caught mid-frame differs
// from the same neighbour caught after it by less than a thousandth, and the difference is
// spread over a blur that is already an approximation.
const float kBloomCoreRadius = 0.018;   // of frame height: the dense part, 8 px at 460
const float kBloomTailRadius = 0.055;   // of frame height: how far the faint tail reaches
const float kBloomTurnover = 0.006;     // of frame height: where the falloff bends over
const uint kBloomTailRings = 3u;
const uint kBloomSpokes = 8u;

// A kernel this peaked is what lets one number serve both cases. Normalised, a flat expanse of
// sky over the threshold gets back exactly kBloomIntensity of itself -- ten per cent, which
// reads as haze rather than as a second sun -- while a single pixel a hundred times over it
// puts about 1.0 into the pixel beside it, because nearly all of the weight is within a few
// pixels of the centre. Small and bright glares hard; large and bright glares gently. That is
// what a lens does.
const float kBloomIntensity = 0.10;
const float kBloomThreshold = 1.0;    // in exposed units, so it follows the light meter
const float kBloomKnee = 0.6;

// What of a pixel bleeds: the part above white, with a soft shoulder so that a surface drifting
// across the threshold does not switch glare on like a light.
vec3 bloom_source(vec3 exposed) {
    float lum = face_luminance(exposed);
    float over = lum - kBloomThreshold + kBloomKnee;
    if (over <= 0.0) return vec3(0.0);
    // Quadratic in the knee, linear above it, and the two meet with the same slope.
    float weight = (over < 2.0 * kBloomKnee) ? (over * over / (4.0 * kBloomKnee))
                                             : (lum - kBloomThreshold);
    return exposed * (weight / max(lum, 1e-4));
}

vec3 gather_bloom(ivec2 pixel, float exposure) {
    float height = float(push.resolution.y);
    ivec2 limit = ivec2(push.resolution.xy) - 1;
    // Capped as well as scaled. The cap is a cost bound -- the dense part is quadratic in this
    // radius, and 12 is already 437 taps a pixel -- and the floor keeps the kernel from
    // collapsing to a single tap on a small window, where it would do nothing at all.
    int core = int(clamp(height * kBloomCoreRadius, 2.0, 12.0));
    float turn = max(height * kBloomTurnover, 1.0);
    float turn2 = turn * turn;

    vec3 sum = vec3(0.0);
    float weights = 0.0;

    // The dense core: every pixel inside the radius, weighted by a curve that is flat for the
    // first pixel or two and then falls off as one over the distance squared, which is the shape
    // scatter in a lens actually has.
    for (int dy = -core; dy <= core; ++dy) {
        for (int dx = -core; dx <= core; ++dx) {
            float r2 = float(dx * dx + dy * dy);
            if (r2 > float(core * core)) continue;   // a disc, not a square: no corners to see
            ivec2 at = pixel + ivec2(dx, dy);
            // Dropped rather than clamped when it falls off the frame. Clamping reads the edge
            // pixel once for every tap that ran off past it, which weights that one pixel dozens
            // of times over and prints a band of its colour along all four sides -- eighteen
            // pixels deep at this radius, and plainly visible along the top of a ceiling.
            if (any(lessThan(at, ivec2(0))) || any(greaterThan(at, limit))) continue;
            vec4 tap = imageLoad(accum, at);
            if (tap.w < 1.0) continue;   // no samples there yet: no answer, not black
            float weight = 1.0 / (1.0 + r2 / turn2);
            sum += bloom_source(tap.rgb / tap.w * exposure) * weight;
            weights += weight;
        }
    }

    // The faint tail, which is what makes a bright window light the wall a long way from it.
    float span = height * kBloomTailRadius;
    for (uint ring = 1u; ring <= kBloomTailRings; ++ring) {
        float r = mix(float(core) + 1.0, span, float(ring) / float(kBloomTailRings));
        float weight = 1.0 / (1.0 + r * r / turn2);
        // Half a step of rotation per ring, so the taps do not line up into spokes radiating out
        // of every bright thing in the picture.
        float phase = (float(ring) * 0.5) * 6.28318530718 / float(kBloomSpokes);
        for (uint spoke = 0u; spoke < kBloomSpokes; ++spoke) {
            float angle = phase + 6.28318530718 * float(spoke) / float(kBloomSpokes);
            ivec2 at = pixel + ivec2(round(vec2(cos(angle), sin(angle)) * r));
            if (any(lessThan(at, ivec2(0))) || any(greaterThan(at, limit))) continue;
            vec4 tap = imageLoad(accum, at);
            if (tap.w < 1.0) continue;
            // Each tail tap stands for the ring of pixels it was drawn from, so it is weighted
            // by that ring's share of the area as well as by the falloff. Without this the tail
            // would be worth eight pixels where it stands for several thousand.
            float share = 6.28318530718 * r / float(kBloomSpokes);
            sum += bloom_source(tap.rgb / tap.w * exposure) * weight * share;
            weights += weight * share;
        }
    }

    // Divided by the weights actually used, not by the weights the kernel has. Taps that fell on
    // pixels with no samples yet were skipped, and dividing by them anyway would dim the glare
    // at the edge of the frame by however many taps fell off it.
    return sum / max(weights, 1e-4) * kBloomIntensity;
}

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
// The plain curve is still reachable -- debug mode 10 -- because "see what the material does,
// not what a grading curve does to it" is a real argument and a filmic curve genuinely hides
// material faults. What it is not is a good default for judging whether a room is lit.
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
// What the primary ray picked up, and lost, before it reached the surface it shades.
//
// A ray that passes through glass is shaded at whatever is *behind* the glass, and everything
// below this point in the shader is written as though it were shading what the eye sees
// directly. Rather than thread a tint through every one of the dozen places that writes a
// pixel, the glass leaves its mark here: what it added on the way (its own reflection) and what
// fraction of the light behind it survives (transmission, times absorption over the distance
// travelled inside). write_pixel applies both, once, for all of them.
vec3 g_prefix = vec3(0.0);
vec3 g_throughput = vec3(1.0);

// The ray this pixel was traced along, unjittered. Wanted twice below -- once for the air in
// front of the camera and once for the tools drawn over the top -- and both want the pixel's
// centre rather than wherever the jitter put it this frame.
vec3 eye_ray(ivec2 pixel, float aspect) {
    vec2 uv = (vec2(pixel) + 0.5) / vec2(push.resolution) * 2.0 - 1.0;
    return normalize(push.forward.xyz + push.right.xyz * uv.x * push.lens.x * aspect -
                     push.up.xyz * uv.y * push.lens.x);
}


// --- motion blur -------------------------------------------------------------------------------
//
// How fast what this pixel is looking at is moving across the screen, in pixels a frame.
//
// The ray and the distance to what it hit give the point's position in the world; projecting that
// through where the camera was last frame says where it appeared then; the difference is the
// velocity. Nothing is stored per pixel, no buffer is kept between frames, and no pass is added —
// the previous camera is five vec4s in a block that was already there.
//
// It is a CAMERA blur and not a per-object one, which for this game is the whole of it: nothing in
// a voxel world moves except the player's own view, and the view is exactly what a shutter would
// smear. A wall you turn past streaks, a wall you walk towards barely does, and a wall you stand
// still in front of does not at all — because the velocity IS the speed, so "speed based" needs no
// separate term.
vec2 screen_velocity(vec3 world_point, ivec2 pixel, float aspect) {
    vec3 relative = world_point - push.prev_origin.xyz;
    float ahead = dot(relative, push.prev_forward.xyz);
    // Behind the old camera, or so close to its plane that the projection blows up. A point that
    // was not on screen last frame has no velocity worth trusting, and inventing one streaks the
    // edge of the screen every time the player turns.
    if (ahead < 1e-3) return vec2(0.0);

    float across = dot(relative, push.prev_right.xyz);
    float upward = dot(relative, push.prev_up.xyz);
    vec2 ndc = vec2(across / (ahead * push.lens.x * aspect), -upward / (ahead * push.lens.x));
    vec2 was = (ndc * 0.5 + 0.5) * vec2(push.resolution.xy);
    return (vec2(pixel) + 0.5) - was;
}

// The accumulated mean at a pixel, which is what a tap of the blur reads.
vec3 accum_mean(ivec2 at) {
    vec4 total = imageLoad(accum, at);
    return total.rgb / max(total.w, 1.0);
}

// Smeared along the direction it is travelling.
//
// Taps are spread over the streak and averaged evenly, which is what a shutter open for a fixed
// fraction of the frame actually does. The tap COUNT follows the length so a short streak costs
// almost nothing and a long one is still bounded: the cost of this is proportional to how fast
// the player is turning, and a player who is standing still pays for one compare.
vec3 motion_blurred(ivec2 pixel, vec3 mean, vec2 velocity) {
    float shutter = push.motion.x;
    if (shutter <= 0.0) return mean;

    vec2 streak = velocity * shutter;
    float length_px = length(streak);
    if (length_px < 0.75) return mean;   // less than a pixel is not a blur, it is a rounding error

    float longest = max(push.motion.y, 1.0);
    if (length_px > longest) {
        streak *= longest / length_px;
        length_px = longest;
    }

    int taps = int(min(length_px, 15.0)) + 1;
    ivec2 limit = ivec2(push.resolution.xy) - 1;
    vec3 sum = mean;
    float weight = 1.0;
    for (int i = 1; i <= taps; ++i) {
        // Backwards along the streak: the smear trails where the pixel came FROM, which is what a
        // shutter records. Forwards as well would smear into places the surface has not been and
        // reads as a glow ahead of everything.
        float where = float(i) / float(taps);
        ivec2 at = clamp(pixel - ivec2(streak * where + 0.5), ivec2(0), limit);
        sum += accum_mean(at);
        weight += 1.0;
    }
    return sum / weight;
}

void write_pixel(ivec2 pixel, uint sample_index, vec3 radiance_in, float primary_t, float aspect) {
    // The air between the eye and the first surface, applied here for the same reason the glass
    // tint is applied here: main returns early three times before its own end -- a summary hit,
    // a pixel not refining this frame, the debug views -- and the middle one is most of the
    // screen most of the time. A medium applied at the end of main reaches about a quarter of
    // the pixels, which on the beam scene read 197 where every pixel getting it reads 241, with
    // the un-fogged pixels showing through the shaft. See pt_media.glsl.
    vec3 eye_dir = eye_ray(pixel, aspect);
    vec3 radiance = apply_media(g_prefix + g_throughput * radiance_in, push.origin.xyz, eye_dir,
                                primary_t);

    // Cloud, over EVERYTHING and not only over the sky.
    //
    // It was composited in the sky branch, which meant it was drawn only on rays that hit nothing.
    // That is a backdrop, not a volume: fly above the deck and look down and there is no cloud
    // between you and the ground, because every one of those rays hit the ground and took the
    // other path. Standing under it and looking up was the one case that worked.
    //
    // Here it is applied exactly the way the air already is, a few lines above — integrated over
    // the distance to whatever the ray actually reached. A ray that hit nothing carries the far
    // plane and gets the whole sky's worth; a ray that hit a roof gets the cloud in front of the
    // roof and no more. Same call, same place, and the special case disappears.
    if (push.sky_cloud.x > 0.0) {
        float through = 1.0;
        vec3 cloud = cloud_march(push.origin.xyz, eye_dir, world_height_metres(push.origin.xyz),
                                 primary_t / kVoxelsPerMetre, through);
        // One non-finite sample spreads into a box the size of the bloom kernel and stays there,
        // a mean that has had a NaN added to it being a NaN for ever.
        if (any(isnan(cloud)) || any(isinf(cloud))) cloud = vec3(0.0);
        if (isnan(through) || isinf(through)) through = 1.0;
        radiance = cloud + through * radiance;
    }
    vec4 total = (sample_index == 0u) ? vec4(0.0) : imageLoad(accum, pixel);
    if (total.w >= kAccumWindow) {
        total *= (kAccumWindow - 1.0) / kAccumWindow;
    }
    // Geometry arriving or leaving resets this outright, on the CPU side, by zeroing the
    // sample index. Halving it instead was tried, and it is the better-looking option Ã¢â‚¬â€ the
    // room scene's speckle went from 7.3 to 4.7, because during streaming chunks arrive on
    // almost every frame and a reset means the average never gets past one sample.
    //
    // It is not the correct option. Halving leaves a residue that nothing clears once
    // streaming stops, and a sealed box with no lights in it settled at a mean of 2.9 instead
    // of zero. A room with no way into it has to be black, so the reset stays and the noise
    // during streaming is the price.
    //
    // The outlier test goes here, before the sum and not after it, because the sum is what it
    // exists to protect: a spike that gets in is in for the length of the window.
    total.rgb += reject_outlier(pixel, radiance, total);
    total.w += 1.0;
    imageStore(accum, pixel, total);

    vec3 mean = total.rgb / max(total.w, 1.0);

    // Blurred before exposure and tone mapping, so the streak is an average of LIGHT rather than
    // of pixels that have already been through a curve. Averaging tone-mapped values darkens a
    // streak past a bright window, because the curve is concave and the mean of the mapped values
    // is below the mapped mean.
    if (push.motion.x > 0.0 && primary_t > 0.0) {
        vec3 world_point = push.origin.xyz + eye_dir * primary_t;
        mean = motion_blurred(pixel, mean, screen_velocity(world_point, pixel, aspect));
    }

    bool plain = (push.resolution.z == 10u);
    float exposure = frame_exposure();
    vec3 exposed = mean * exposure;
    if (!plain) exposed += gather_bloom(pixel, exposure);

    vec3 mapped = pow(plain ? tonemap_plain(exposed) : tonemap_aces(exposed), vec3(1.0 / 2.2));

    // The tools, over the top, exactly as the real-time pipeline draws them. After tone mapping
    // and outside the accumulation, because a preview is interface rather than light: it must
    // not be averaged in, must not converge, and must not be dimmed by an exposure curve.
    // Unjittered, so an outline is a line rather than a smeared band.
    //
    // Pinhole as well, whatever the aperture is. A tool that went soft when it drifted off the
    // focal plane would be unusable, and it is not in the scene to be photographed.
    mapped = draw_preview(mapped, push.origin.xyz, eye_dir, primary_t);

    imageStore(out_colour, pixel, vec4(mapped, 1.0));

    // ---------------------------------------------------------------------------------------
    // And what this pixel tells the next frame.
    //
    // Measured before exposure, because the meter is measuring the scene and not its own last
    // answer. Metering the exposed image would be a loop chasing a fixed post-exposure grey,
    // which settles in the same place but takes the long way round and oscillates getting there.

    // Whole workgroups only. A group hanging off the edge of the frame contributes fewer than
    // 64 pixels but would still count as one group, and the mean would come out low by however
    // much of the frame the last row of groups is missing -- 0.9 percent at 760x460, which is a
    // tenth of a stop for nothing. Dropping those groups costs the meter the outermost seven
    // pixels of the picture and no accuracy at all.
    uvec2 group_first = gl_WorkGroupID.xy * gl_WorkGroupSize.xy;
    if (group_first.x + gl_WorkGroupSize.x > push.resolution.x ||
        group_first.y + gl_WorkGroupSize.y > push.resolution.y) {
        return;
    }

    // Each pixel adds a 64th of its own value, so the total is exactly the sum of per-workgroup
    // means that gpu/render_params.hpp describes, and the bound it computes there holds.
    //
    // A 64th per pixel and not a group reduction in shared memory, because write_pixel is
    // called from three places in main and every one of them is inside divergent control flow.
    // A barrier there is undefined and hangs on some hardware; an atomic does not care.
    //
    // Rounded rather than truncated. Truncation loses up to a whole unit of 128 every time and
    // always downwards, which is a systematic eighth of a stop; rounding leaves an error of half
    // a unit either way that averages out over the hundreds of thousands of pixels in a frame.
    float stops = clamp(log2(max(face_luminance(mean), 1e-8)), kMeterFloor, kMeterCeiling);
    float value = (stops + kLogLuminanceBias) * kLogLuminanceUnit;
    atomicAdd(stats.slots[0].log_luminance, uint(value / 64.0 + 0.5));
    if (gl_LocalInvocationIndex == 0u) atomicAdd(stats.slots[0].groups, 1u);

    // The exposure this frame settled on, so the next one has something to move away from
    // rather than snapping to each new measurement. One invocation writes it, and every
    // invocation computed the same number, so which one is arbitrary.
    if (gl_GlobalInvocationID.x == 0u && gl_GlobalInvocationID.y == 0u) {
        stats.slots[0].exposure = uint(clamp(exposure, kExposureMin, kExposureMax) *
                                       kExposureUnit);
    }

    // And where the lens should focus: the centre pixel says how far away whatever it is
    // pointed at is, and the next frame's rays are built through that. Sky reads as the far
    // plane, which is focus at infinity and correct -- a camera pointed at the sky does the
    // same thing, and with a wide aperture it blurs everything in the foreground exactly as
    // this does.
    //
    // Snapped, and NOT eased the way exposure is, which is the one thing about this that had to
    // be found by rendering it. Exposure is applied to the average after the fact, so moving it
    // between frames costs nothing: the samples underneath are untouched. The focus is not like
    // that. It changes the *ray*, so every frame's samples are taken through whatever lens was
    // in force when they were traced, and the accumulator keeps all of them. Easing the focus
    // over a hundred frames therefore does not rack the focus, it averages a hundred different
    // lenses together, and the picture comes out uniformly hazy with nothing sharp anywhere in
    // it. That is what the first version of this drew.
    //
    // The rule underneath is the same one the cloud deck in pt_sky.glsl is frozen for, and the
    // same one the camera already obeys: anything that changes what a ray is may not drift while
    // the accumulator is running. If a slow focus rack is ever wanted, the CPU has to zero the
    // sample index when the focus moves, exactly as it does when the camera does.
    if (pixel == ivec2(push.resolution.xy) / 2) {
        float measured = clamp(primary_t, 1.0 / kFocusUnit, kFocusMax / kFocusUnit);
        stats.slots[0].focus = uint(clamp(measured * kFocusUnit, 1.0, kFocusMax));
    }
}
