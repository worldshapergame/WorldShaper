// Cloud, as weather rather than as wallpaper.
//
// What was here before was a two-dimensional noise projected onto a dome at infinity. It earned
// its place — a perfectly clear sky is the one thing no photograph of a building has ever had —
// and it could not do any of the things a cloud does. It had no inside, so nothing could fly
// through it or be lit by it; it was at infinity, so it never passed overhead; it was static, so
// it never moved; and it cast no shadow, so the ground below it was as bright at noon under an
// overcast as under a clear sky.
//
// This is a real volume in the world, at real altitudes, marched.
//
// # The three étages, because that is how the sky is actually arranged
//
// Meteorology divides cloud by height and so does this, for the same reason: the height decides
// what the cloud is made of and therefore what it looks like.
//
//   LOW, 600 to 2000 m. Water droplets. Cumulus with flat bases and cauliflower tops, and
//   stratocumulus when they spread and join. Dense, sharply defined, and the only ones that put a
//   hard shadow on the ground.
//
//   MIDDLE, 2500 to 6000 m. Supercooled water. Altocumulus: smaller, flatter, often in rows or a
//   mackerel pattern, and thin enough that the sun shows through as a bright patch rather than
//   being blocked.
//
//   HIGH, 6000 to 12000 m. Ice crystals, not droplets. Cirrus: streaked out by the wind at that
//   altitude into fibrous hooks, almost transparent, and they scatter forward hard enough to put a
//   halo round the sun rather than a silver lining round themselves.
//
// All three can be in the sky at once and at different heights, which is what makes a real sky
// read as deep rather than as a backdrop: the low deck moves visibly faster than the high one
// because it is nearer, and nothing had to be told to do that — it falls out of them being at
// different distances in a perspective projection.
//
// # How the light works
//
// A cloud is not a surface and cannot be shaded like one. What arrives at the eye is the sum, over
// every step along the ray, of the light scattered towards it from that point, attenuated by
// everything in front. Each term needs three things:
//
//   TRANSMITTANCE to the sun. Marched, because a cloud's own body is what shadows its underside,
//   and that self-shadowing is the whole reason a cumulus reads as a solid object rather than as a
//   patch of fog. Beer's law, exp(-density * distance).
//
//   THE PHASE FUNCTION. Water droplets scatter strongly forward, so a cloud between the eye and
//   the sun is far brighter at its rim than a cloud beside the sun — the silver lining, and it is
//   not a texture, it is the phase function. Henyey-Greenstein, with a backward lobe mixed in
//   because a real droplet does both and a purely forward one makes clouds away from the sun read
//   as flat grey.
//
//   MULTIPLE SCATTERING. A photon in a cloud bounces thousands of times, and marching that is not
//   affordable. The standard approximation — several octaves with the extinction, the scattering
//   and the phase each scaled down per octave — costs three light marches and gets the property
//   that matters: a thick cloud's interior glows rather than going black, which single scattering
//   alone gets badly wrong.
//
// # And the shadows on the ground
//
// The same march, from a point on the ground towards the sun. It is deliberately NOT part of the
// cached shadow term: that cache averages over hundreds of frames and cloud shadows move, so a
// cached one would smear into a permanent grey rather than sweep across the courtyard. It is
// computed per sample and coarsely, which is affordable because a cloud shadow has no fine detail
// in it to lose.

// --- how much light a cloud takes and gives back ----------------------------------------------
//
// Extinction is per metre of path through density 1, and 0.06 is a real figure for a fair-weather
// cumulus: a hundred metres of it leaves about a quarter of the light, which is why you can see
// the sun's disc through a thin edge and not through the middle.
const float kCloudExtinction = 0.06;

// Cloud droplets scatter almost everything they intercept and absorb almost nothing, which is why
// clouds are white rather than grey. Slightly under one so a very thick cloud still darkens.
const float kCloudScatter = 0.96;

// How many octaves of multiple scattering. Three is where it stops changing the picture.
const int kScatterOctaves = 3;

// How much of the sky's own light fills a cloud from below and behind. It is what stops the shaded
// side going black, and it is why an overcast is bright grey rather than dark.
const float kCloudAmbient = 0.55;

// How much of the powder darkening to apply. Full strength overdoes it into a charcoal rim.
const float kCloudPowder = 0.6;

// The least sunlight a cloud shadow may leave. A shadow on a real day is a stop or two, not a
// blackout: the rest of the sky is still lighting the ground, and that light reaches a surface by
// a path with no cloud term in it.
const float kShadowFloor = 0.18;

// --- where the decks are, in metres ------------------------------------------------------------
//
// Written in metres and converted, because these are numbers out of a meteorology table and should
// be recognisable as such.
const float kLowBase = 600.0;
const float kLowTop = 2000.0;
const float kMidBase = 2500.0;
const float kMidTop = 6000.0;
const float kHighBase = 6500.0;
const float kHighTop = 11000.0;

// Voxels to a metre, matching the clip. `metre 32` at the top of clips/facility/_contract.clip.
const float kVoxelsPerMetre = 32.0;

float metres(float m) { return m * kVoxelsPerMetre; }

// How far a sky ray is worth marching, in metres. Past the far side of the top deck at a grazing
// angle there is nothing left to integrate, and a ray along the horizon would otherwise run to the
// edge of the float range looking for cloud it has already passed under.
const float kCloudFarMetres = 120000.0;

// The height of a point above the world's own origin, in metres.
//
// Everything in these shaders is in voxels relative to the CAMERA'S CHUNK, which moves as the
// player walks. Cloud decks are at absolute altitudes and must not move with the camera, so this
// is where the two are reconciled — and getting it wrong is not subtle: the weather follows the
// player up a staircase.
float world_height_metres(vec3 p) {
    float absolute_voxels = p.y + float(push.camera_chunk.y) * float(kChunkEdge);
    return absolute_voxels / kVoxelsPerMetre;
}

// --- noise ---------------------------------------------------------------------------------
//
// Value noise in three dimensions, because the cloud is a volume. The two-dimensional noise the
// old sky used cannot be lifted into one: a cloud sampled from a plane is a column, and a sky full
// of columns reads as a corrugated roof the moment you look along it rather than up at it.

float cloud_hash(vec3 cell) {
    return fract(sin(dot(cell, vec3(127.1, 311.7, 74.7))) * 43758.5453123);
}

float cloud_noise(vec3 p) {
    vec3 cell = floor(p);
    vec3 f = p - cell;
    f = f * f * (3.0 - 2.0 * f);

    float n000 = cloud_hash(cell + vec3(0, 0, 0));
    float n100 = cloud_hash(cell + vec3(1, 0, 0));
    float n010 = cloud_hash(cell + vec3(0, 1, 0));
    float n110 = cloud_hash(cell + vec3(1, 1, 0));
    float n001 = cloud_hash(cell + vec3(0, 0, 1));
    float n101 = cloud_hash(cell + vec3(1, 0, 1));
    float n011 = cloud_hash(cell + vec3(0, 1, 1));
    float n111 = cloud_hash(cell + vec3(1, 1, 1));

    return mix(mix(mix(n000, n100, f.x), mix(n010, n110, f.x), f.y),
               mix(mix(n001, n101, f.x), mix(n011, n111, f.x), f.y), f.z);
}

float cloud_fbm(vec3 p, int octaves) {
    float sum = 0.0;
    float amplitude = 0.5;
    float total = 0.0;
    for (int i = 0; i < octaves; ++i) {
        sum += cloud_noise(p) * amplitude;
        total += amplitude;
        p *= 2.02;              // not exactly two, so the octaves do not line up their lattices
        amplitude *= 0.5;
    }
    return sum / max(total, 1e-4);
}

// Billowing, which is what makes a cumulus a cumulus.
//
// Ordinary noise gives soft blobs; a cumulus is made of packed round bulges with creases between
// them. Inverting the noise turns the smooth maxima into the packed bulges, and it is the cheapest
// thing that reads as convection rather than as fog.
float cloud_billow(vec3 p, int octaves) {
    float sum = 0.0;
    float amplitude = 0.5;
    float total = 0.0;
    for (int i = 0; i < octaves; ++i) {
        sum += abs(cloud_noise(p) * 2.0 - 1.0) * amplitude;
        total += amplitude;
        p *= 2.02;
        amplitude *= 0.5;
    }
    return 1.0 - sum / max(total, 1e-4);
}

// --- the shape of each deck ------------------------------------------------------------------

// Where in its deck a height sits, 0 at the base and 1 at the top.
float deck_fraction(float height, float base, float top) {
    return clamp((height - base) / max(top - base, 1.0), 0.0, 1.0);
}

// A cumulus profile: a flat base, a body that swells, and a top that rounds off.
//
// The flat base is not a stylisation. It is the condensation level — the height at which rising
// air has cooled to its dew point — and it is the same height everywhere on a given day, which is
// why a field of cumulus looks like it is resting on a sheet of glass. Getting that one edge right
// does more for a sky than any amount of noise.
float cumulus_profile(float f) {
    return smoothstep(0.0, 0.08, f) * (1.0 - smoothstep(0.45, 1.0, f));
}

// Altocumulus: a thinner slab, softer at both ends, sitting in the middle of its deck.
float altocumulus_profile(float f) {
    return smoothstep(0.0, 0.25, f) * (1.0 - smoothstep(0.5, 0.95, f));
}

// Cirrus: thin, high in its own deck, and fading gently because ice does not have a condensation
// level to sit on.
float cirrus_profile(float f) {
    return smoothstep(0.0, 0.35, f) * (1.0 - smoothstep(0.55, 1.0, f));
}

// --- density -----------------------------------------------------------------------------------

// The wind at a height, in voxels a second.
//
// Faster higher up, which is true and is also what sells the depth: the low deck crawls, the cirrus
// above it streams. The high deck is turned as well, because wind veers with altitude, and a sky
// whose decks all drift the same way looks like one deck drawn three times.
vec3 cloud_wind(float height_m) {
    vec2 low = push.sky_wind.xy;
    float faster = 1.0 + height_m * (1.0 / 3000.0);
    float turn = height_m * (1.0 / 40000.0);       // radians, a gentle veer with height
    float c = cos(turn), s = sin(turn);
    vec2 veered = vec2(low.x * c - low.y * s, low.x * s + low.y * c);
    return vec3(veered.x, 0.0, veered.y) * faster * kVoxelsPerMetre;
}

// How much of the sky each deck is allowed. `sky_cloud.x` is the overall weather, 0 clear and 1
// overcast; the shares below are what a fair-weather day looks like when it is turned up.
float deck_coverage(int deck) {
    float weather = clamp(push.sky_cloud.x, 0.0, 1.0);
    if (deck == 0) return clamp(weather * 1.15 - 0.10, 0.0, 1.0);
    if (deck == 1) return clamp(weather * 0.85 - 0.05, 0.0, 1.0);
    return clamp(weather * 0.70 + 0.05, 0.0, 1.0);
}

// Density at a point, for one deck. Zero outside it.
//
// `p` is in voxels in the shader's own space; `height_m` is the absolute height above the world's
// origin in metres, which is what decides which deck a point is in and therefore what kind of
// cloud it is. Those are different things and conflating them puts the weather at the wrong
// altitude the moment the camera moves up a chunk.
float cloud_density_at(vec3 p, float height_m, int deck) {
    float base, top, coverage;
    if (deck == 0)      { base = kLowBase;  top = kLowTop;  }
    else if (deck == 1) { base = kMidBase;  top = kMidTop;  }
    else                { base = kHighBase; top = kHighTop; }
    if (height_m < base || height_m > top) return 0.0;

    coverage = deck_coverage(deck);
    if (coverage <= 0.0) return 0.0;

    float f = deck_fraction(height_m, base, top);
    vec3 drift = cloud_wind(height_m) * push.sky_cloud.y;   // y is the time in seconds
    vec3 q = (p - drift);

    if (deck == 0) {
        // Cumulus. Large shapes from the low octaves, then the billow carves the bulges into
        // them, and the profile puts the flat base on.
        float shape = cloud_fbm(q * (1.0 / metres(900.0)), 3);
        float bulge = cloud_billow(q * (1.0 / metres(220.0)), 3);
        float profile = cumulus_profile(f);
        // The billow ERODES rather than adds, taken off the shape's edges only. Adding it makes a
        // uniform fizz over everything; taking it off the edge is what leaves a solid core with a
        // ragged rim, which is what a cumulus is.
        float body = shape * profile;
        float threshold = 1.0 - coverage;
        float density = smoothstep(threshold, threshold + 0.20, body);
        return density * mix(1.0, bulge, 0.55);
    }

    if (deck == 1) {
        // Altocumulus. Smaller cells, flatter, and stretched slightly along the wind so they read
        // as the rows and mackerel patterns a middle deck actually forms.
        vec3 stretched = q * vec3(1.0, 1.0, 0.55);
        float shape = cloud_fbm(stretched * (1.0 / metres(400.0)), 3);
        float profile = altocumulus_profile(f);
        float threshold = 1.0 - coverage * 0.85;
        return smoothstep(threshold, threshold + 0.26, shape * profile) * 0.55;
    }

    // Cirrus. Streaked hard along the wind — the axis is squashed by six, which turns round blobs
    // into the fibrous hooks ice makes at that altitude — and thin enough to see the sun through.
    vec3 streaked = q * vec3(0.16, 1.0, 1.0);
    float shape = cloud_fbm(streaked * (1.0 / metres(700.0)), 4);
    float profile = cirrus_profile(f);
    float threshold = 1.0 - deck_coverage(2) * 0.8;
    return smoothstep(threshold, threshold + 0.30, shape * profile) * 0.18;
}

// Every deck at once, which is what a ray actually passes through.
float cloud_density_all(vec3 p, float height_m) {
    return cloud_density_at(p, height_m, 0) + cloud_density_at(p, height_m, 1) +
           cloud_density_at(p, height_m, 2);
}

// --- lighting ---------------------------------------------------------------------------------

// Henyey-Greenstein, the standard one-parameter phase function.
float cloud_phase_hg(float cos_angle, float g) {
    float gg = g * g;
    float d = 1.0 + gg - 2.0 * g * cos_angle;
    return (1.0 - gg) / (4.0 * kPi * max(d * sqrt(max(d, 1e-4)), 1e-4));
}

// Forward and backward together, because a droplet does both.
//
// A purely forward lobe gives a beautiful silver lining and makes every cloud that is not between
// the eye and the sun read as flat grey card. The backward lobe is what puts light back out of the
// side of a cloud you are looking at with the sun behind you, which is most of the sky most of the
// time.
float cloud_phase(float cos_angle) {
    return mix(cloud_phase_hg(cos_angle, 0.80), cloud_phase_hg(cos_angle, -0.30), 0.30);
}

// How much sunlight reaches a point inside the cloud.
//
// Marched with a step that GROWS. The first steps decide whether this point is just under the
// surface or deep inside, which is the difference between a bright rim and a dark core and wants
// resolution; a hundred metres further in, all that matters is roughly how much is above, and
// spending equal steps on it buys nothing. Six taps this way reach further than twelve evenly
// spaced ones.
float cloud_sun_transmittance(vec3 p, float height_m, vec3 to_sun) {
    const int kSunSteps = 6;
    float step_m = 60.0;
    float travelled = 0.0;
    float optical = 0.0;

    for (int i = 0; i < kSunSteps; ++i) {
        travelled += step_m;
        vec3 at = p + to_sun * metres(travelled);
        float h = height_m + to_sun.y * travelled;
        optical += cloud_density_all(at, h) * step_m;
        step_m *= 1.65;
    }
    return exp(-optical * kCloudExtinction);
}

// What a cloud looks like along a ray.
//
// Returns the light scattered towards the eye, and leaves the transmittance behind it in
// `transmittance` so the caller can put the sky, the sun and the stars behind what it did not
// block.
vec3 cloud_march(vec3 origin, vec3 dir, float height_origin_m, float max_distance_m,
                 out float transmittance) {
    transmittance = 1.0;
    vec3 scattered = vec3(0.0);

    // Nothing above the highest deck looking up, nothing below the lowest looking down: the whole
    // march is skipped for the majority of rays, which is what makes this affordable at all.
    float enter_m = 0.0;
    float leave_m = max_distance_m;
    if (abs(dir.y) < 1e-4) {
        if (height_origin_m < kLowBase || height_origin_m > kHighTop) return scattered;
    } else {
        float to_bottom = (kLowBase - height_origin_m) / dir.y;
        float to_top = (kHighTop - height_origin_m) / dir.y;
        enter_m = max(min(to_bottom, to_top), 0.0);
        leave_m = min(max(to_bottom, to_top), max_distance_m);
        if (leave_m <= enter_m) return scattered;
    }

    // A step is a LENGTH, and the span is not bounded: a ray along the horizon crosses tens of
    // kilometres of deck, and dividing that by a fixed step count gives steps kilometres long.
    // Sampling a kilometre of cloud at one point and multiplying by a kilometre is what turned the
    // first render into white streaks — the march was aliasing, not the noise.
    //
    // So the step is bounded at both ends and the count follows the span. The step also GROWS as
    // the ray travels, because a cloud twenty kilometres away is a few pixels across and does not
    // deserve the resolution one overhead does.
    const int kViewSteps = 64;
    const float kStepNear = 25.0;     // metres, close enough to resolve a cumulus edge
    const float kStepFar = 400.0;
    float span_m = leave_m - enter_m;
    float step_m = clamp(span_m / float(kViewSteps), kStepNear, kStepFar);

    float cos_angle = dot(dir, trace.sun.xyz);
    float phase = cloud_phase(cos_angle);
    vec3 sunlight = sun_radiance();
    vec3 ambient = sky_radiance(vec3(0.0, 1.0, 0.0)) * kCloudAmbient;

    float travelled = enter_m + step_m * 0.5;
    for (int i = 0; i < kViewSteps; ++i) {
        if (transmittance < 0.01) break;

        vec3 at = origin + dir * metres(travelled);
        float h = height_origin_m + dir.y * travelled;
        float density = cloud_density_all(at, h);
        float this_step = step_m;
        travelled += this_step;
        if (travelled > leave_m) break;
        // Coarser the further out, so a long shallow ray finishes without either aliasing near the
        // eye or spending its whole budget in the first kilometre.
        step_m = min(step_m * 1.06, kStepFar);
        if (density <= 0.001) continue;

        float sun_t = cloud_sun_transmittance(at, h, trace.sun.xyz);

        // Multiple scattering, as octaves. Each one is a dimmer, broader, less directional copy of
        // the first, which is what a photon that has bounced a dozen times behaves like: it has
        // forgotten which way it came from and it has been attenuated less than a straight line
        // through the same cloud would suggest, because it went round.
        vec3 light = vec3(0.0);
        float attenuation = 1.0;
        float contribution = 1.0;
        float phase_mix = 1.0;
        for (int octave = 0; octave < kScatterOctaves; ++octave) {
            float t = pow(sun_t, attenuation);
            float p = mix(cloud_phase(cos_angle * phase_mix), 1.0 / (4.0 * kPi),
                          1.0 - phase_mix);
            light += sunlight * t * p * contribution;
            attenuation *= 0.55;
            contribution *= 0.6;
            phase_mix *= 0.5;
        }
        // NOT multiplied by four pi. The phase function is already normalised so that it
        // integrates to one over the sphere, which is what makes `sunlight * phase` the radiance
        // scattered towards the eye. Multiplying by the sphere's solid angle on top turns a
        // forward lobe into a gain of about nine, and the first render of this came back as a
        // sheet of white with a navy sky behind it.
        light += ambient;

        // Powder: the darkening on the sunlit side of a cloud's edge, where a point has very
        // little above it but a great deal around it, so the light that reaches it has been
        // scattered sideways out rather than straight through. Without it a cumulus lit from
        // behind the camera reads as flat white paper.
        float powder = 1.0 - exp(-density * this_step * kCloudExtinction * 2.0);

        float optical = density * this_step * kCloudExtinction;
        float step_t = exp(-optical);
        // The analytic integral of the in-scattering across the step, rather than a sample at its
        // centre times its length. Same cost, and it stops a thin cloud marched in long steps from
        // being visibly dimmer than the same cloud marched in short ones.
        //
        // The whole of it is the single-scattering albedo. What is scattered towards the eye over
        // the step is (sigma_s / sigma_t) * (1 - exp(-sigma_t * ds)), and sigma_s / sigma_t is
        // kCloudScatter — the density and the extinction cancel exactly. Writing the division
        // without the matching multiplication scales every step by one over the extinction, which
        // is sixteen, and the first render of this came back as a sheet of pure white.
        vec3 integrated = light * kCloudScatter * (1.0 - step_t);
        scattered += transmittance * integrated * mix(1.0, powder, kCloudPowder);
        transmittance *= step_t;
    }
    return scattered;
}

// How much sun reaches a point on the ground, through whatever is above it.
//
// Coarse on purpose. A cloud shadow is enormous and soft — a cumulus a kilometre across throws an
// edge that takes tens of metres to go from lit to shaded — so there is no detail here to lose by
// taking eight steps, and this runs per shading sample rather than per pixel of sky.
//
// Deliberately not folded into the cached shadow term. That cache averages hundreds of frames and
// these move; cached, a cloud shadow would settle into a permanent grey over the whole courtyard
// instead of sweeping across it.
float cloud_shadow(vec3 p, float height_m) {
    if (push.sky_cloud.x <= 0.0) return 1.0;
    vec3 to_sun = trace.sun.xyz;
    if (to_sun.y <= 0.02) return 1.0;      // the sun is on the horizon; no deck is between

    // Only the LOW deck is marched, and that is a decision rather than a saving.
    //
    // A cumulus at a kilometre throws a shadow with an edge on it. Altocumulus at four kilometres
    // is thin enough that what it does to the ground is a slight dimming, and cirrus at eight is
    // not visible on the ground at all — you can read a newspaper under a cirrus sky. Marching all
    // three would spend most of the samples on the two that contribute nothing an eye could find.
    float enter_m = max((kLowBase - height_m) / to_sun.y, 0.0);
    float leave_m = (kLowTop - height_m) / to_sun.y;
    if (leave_m <= enter_m) return 1.0;

    // The step is a LENGTH and the span is not bounded — at a low sun the ray crosses the deck
    // almost horizontally and the distance runs to tens of kilometres. Dividing that by a fixed
    // count gives steps kilometres long, and one sample of density multiplied by a kilometre is an
    // optical depth in the thousands: exp(-1000) is nought, and the first version of this turned
    // every surface in the world black at noon under a clear sky.
    const int kShadowSteps = 10;
    const float kShadowStepMax = 220.0;   // metres
    float span_m = min(leave_m - enter_m, kShadowStepMax * float(kShadowSteps));
    float step_m = span_m / float(kShadowSteps);

    float optical = 0.0;
    for (int i = 0; i < kShadowSteps; ++i) {
        float travelled = enter_m + step_m * (float(i) + 0.5);
        vec3 at = p + to_sun * metres(travelled);
        float h = height_m + to_sun.y * travelled;
        optical += cloud_density_at(at, h, 0) * step_m;
    }

    // Never all the way to black. A cloud shadow on a real day is a change of a stop or two, not a
    // blackout, because the whole sky is still lighting the ground even where the sun is not — and
    // the sky's own contribution reaches this surface by a different path that knows nothing about
    // clouds. Without the floor an overcast reads as night.
    return mix(kShadowFloor, 1.0, exp(-optical * kCloudExtinction));
}

