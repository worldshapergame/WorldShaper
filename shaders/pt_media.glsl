// Light that is scattered on the way rather than at the end of the journey.
//
// Everything in this renderer assumes light travels through a vacuum between surfaces. It does
// not: a shaft of sun through a high window is visible *in the air*, and that image -- the beam
// coming down through the oculus of the rotunda onto the floor -- is the single picture the
// test facility exists to produce. Without media it is a bright patch on the floor and nothing
// in between.
//
// The rules this was written under, which still stand:
//   - it costs a march it cannot have. The way in is the same one transmission took: march once
//     inside a loop, or sample the medium along a ray that is already being traced rather than
//     tracing a new one.
//   - it must be per voxel face wherever it caches anything, like everything else here.
//   - a scene with no medium in it must pay nothing. The common case is clear air.
//
// ---------------------------------------------------------------------------------------
// What was built, and what was rejected.
//
// Single scattering: the eye's ray is attenuated over its length, and along the way each parcel
// of air it passes through sends a share of the sun towards the eye. Two things decide how much:
// how dense the air is there, and whether the sun reaches it at all. The first is a formula. The
// second is a shadow ray, and a shadow ray per step is the whole difficulty, because this shader
// carries three inlined copies of march and a fourth loses the device.
//
// The loop trick transmission used does not work here and it is worth saying why it does not.
// That loop continues *one* ray: the primary ray goes on past a pane of glass, so one mention of
// march inside a loop serves every segment of it. A medium wants a ray pointing somewhere else --
// at the sun, from a point in mid-air -- and there is nowhere to hang that on the ray already
// being traced. It would have to be a second mention of `occluded`, which is a fourth march, and
// the fourth march is measured rather than feared.
//
// So the sun's visibility in the air is cached, exactly as the sun's visibility on a surface
// already is, in the same table and keyed the same way: a voxel node, a level, a face normal and
// a kind. It is a property of a parcel of air and the sun's direction and of nothing else -- not
// of the eye, not of the pixel, not of how far the camera is standing back -- so it is the same
// entry from every viewpoint, and a beam somebody has looked at once is still there when they
// look back. A cache keyed on where the viewer is standing is not a cache of the world.
//
// Filling an entry still needs an occlusion test, but a cached quantity is only filled by a
// trickle: at most one test per pixel per frame, and none at all once the parcel knows itself.
// The test is twenty lines of stepped occupancy -- chunk, brick, voxel -- and not a march: it
// asks a yes-or-no question of level 0 with no detail selection, no summary tiers, no streaming
// feedback and no materials, which is everything march is large for.

// ---- where the numbers come from --------------------------------------------------------
//
// Two push-constant vec4s in the Trace block, mirrored by TracePush in src/app/main.cpp.
//
// Per voxel rather than per metre because a voxel is the unit every length in this shader is
// already in, and the metre is a property of the clip rather than of the renderer -- see the
// `metre` line at the top of clips/mirror_test.clip. The caller divides its authored per-metre
// figure by the clip's voxels per metre once, on the CPU, instead of every shader doing it.
//
// Both read zero until somebody sets them, every scene is then clear air, and the picture is
// bit-for-bit what it was before this file did anything -- which is what the no-medium test
// asks for.
vec4 media_fog()       { return trace.fog; }
vec4 media_fog_shape() { return trace.fog_shape; }

// ---- and where apply_media has to be called from -----------------------------------------
//
// Every path that writes a pixel, which is not the same thing as the end of main.
//
// main returns early three times before it gets there -- a summary hit, a pixel that is not
// refining this frame, and the debug views -- and the second of those is most of the screen most
// of the time, because that is the whole point of the face cache. A medium applied only at the
// end therefore reaches about a quarter of the pixels, and the picture is a beam of the right
// shape at a quarter of its brightness with the un-fogged pixels showing through it: measured on
// the beam scene, the shaft read 197 against the 241 it reads when every pixel gets it.
//
// So it belongs in write_pixel, beside the glass tint, for the reason pt_post.glsl already gives
// for putting the glass tint there: a dozen places write a pixel and threading anything through
// all of them is how one gets missed.

// ---- what this file is, and what it deliberately is not -----------------------------------
//
// Fog and haze in the world the player is standing in. NOT a planetary atmosphere.
//
// That distinction was learned by writing the other thing first. A full Rayleigh-Mie-ozone model
// went in here, with the measured coefficients, and it produced a black sky and a white ground. Two
// reasons, and both are structural rather than tuning:
//
//   - pt_sky.glsl ALREADY computes atmospheric colour for every direction. A second atmosphere
//     applied on top of it does not add realism, it double-counts: the sky's own light is
//     extinguished by air it has already been through.
//
//   - A ray that hits nothing carries the far plane as its length. Integrating sea-level air over
//     that distance gives an optical depth in the hundreds, and the sky goes to black.
//
// So the species here are the ones that live near the ground and that the sky shader knows nothing
// about. The real-world numbers below are for AUTHORING them, which is where they belong.

// ---- Koschmieder's law, which is how fog is actually specified ----------------------------
//
// Meteorological visibility is defined as the distance at which a black object against the horizon
// falls to two per cent contrast. That fixes the relationship between what an observer reports and
// what the renderer needs:
//
//     extinction = ln(1 / 0.02) / visibility = 3.912 / visibility
//
// It is the honest way to author fog because it is how fog is MEASURED. Every published figure is a
// visibility band, and each of these is a real definition rather than a preset:
//
//     fog          under 1 km      extinction over 3.9e-3 per metre
//     mist         1 to 2 km       2.0e-3 to 3.9e-3
//     haze         2 to 5 km       7.8e-4 to 2.0e-3
//     clear        over 10 km      under 3.9e-4
//     exceptional  over 50 km      under 7.8e-5
//
// --fog takes extinction per metre and main.cpp divides by the clip's voxels-per-metre once, so
// everything below is per VOXEL and no conversion happens in this file. Converting here with the
// cloud module's fixed thirty-two was tried and is wrong: the metre is a property of the clip.

// Droplets are tens of times the wavelength of light, which decides two things that are usually got
// wrong. Fog is GREY -- Mie scattering off particles that large is colourless to within a per cent,
// so there is no wavelength term here and adding one would be inventing physics. And water barely
// absorbs visible light, so nearly everything scattered comes back out: fog is the BRIGHTEST thing
// in an overcast landscape, not a grey veil that darkens it.
const float kFogAlbedo = 0.99;

// Haze sits above the fog and is thinner, broader and made of dry aerosol rather than droplets --
// dust, salt, smoke. Its particles are smaller, so it is more forward-scattering still and it does
// absorb a little.
const float kHazeAlbedo = 0.90;
const float kHazeOfFog = 0.12;      // how much haze comes with a given fog, by extinction
const float kHazeHeightScale = 8.0; // and how much further up it reaches

struct Air {
    float fog_extinct;    // per voxel, at the reference height
    float fog_g;
    float fog_height;     // e-folding, voxels; zero or less means uniform
    float fog_base;       // absolute world height in voxels, where the coefficient is quoted
    float haze_extinct;
    float haze_height;
    bool any;
};

Air world_air() {
    vec4 fog = media_fog();
    vec4 shape = media_fog_shape();

    Air a;
    a.fog_extinct = max(fog.w, 0.0);
    // Just short of one either way: at exactly one the phase function's denominator is zero in the
    // forward direction, which is an infinity in a pixel rather than a bright one.
    a.fog_g = clamp(shape.x, -0.95, 0.95);
    a.fog_height = shape.y;
    a.fog_base = shape.z;

    // Haze is derived rather than authored. Fog does not occur in clean air -- the droplets need
    // something to condense on -- so a scene with fog in it has aerosol above the fog by
    // construction, and asking for both separately would be asking for a combination that does not
    // happen. It is what keeps a distant hill from being perfectly sharp above a valley full of fog.
    a.haze_extinct = a.fog_extinct * kHazeOfFog;
    a.haze_height = a.fog_height > 0.0 ? a.fog_height * kHazeHeightScale : 0.0;

    a.any = a.fog_extinct > 0.0;
    return a;
}

// The density of an exponential layer at a height, relative to its reference.
float layer_density(float y, float base, float scale) {
    if (scale <= 0.0) return 1.0;
    return exp(clamp((base - y) / scale, -40.0, 40.0));
}

// The integral of that layer along a ray, in voxel-units of reference density.
//
// Closed form, and keeping it closed form is the most important property of this file.
//
// Density is an exponential in height and height is linear in distance along a ray, so the integral
// is another exponential: one call to exp for a quantity that would otherwise need a march. It is
// what makes the whole model a handful of instructions rather than a loop, and it is also what stops
// the fog's thickness depending on where any samples happened to land. Transmittance multiplies the
// entire scene behind it, so noise in it is noise in everything.
float layer_integral(float y0, float dy, float length, float base, float scale) {
    if (length <= 0.0) return 0.0;
    float rho = layer_density(y0, base, scale);
    if (scale <= 0.0) return rho * length;

    float k = dy / scale;
    // Near-horizontal, or a scale height so large the ramp cannot be told from a straight line. The
    // closed form divides by k, so this is a correctness guard and not only a cheaper path.
    if (abs(k) * length < 1.0e-3) return rho * length;
    return rho * (1.0 - exp(clamp(-k * length, -40.0, 40.0))) / k;
}

// Henyey-Greenstein, normalised over the sphere, so it redistributes what the medium scatters and
// never adds to it. Forward-heavy for both species here, which is what makes looking into a low sun
// through fog blinding and looking away from it merely grey.
float henyey_greenstein(float cos_theta, float g) {
    float gg = g * g;
    float d = max(1.0 + gg - 2.0 * g * cos_theta, 1.0e-4);
    return (1.0 - gg) / (4.0 * kPi * d * sqrt(d));
}

// ---------------------------------------------------------------------------------------

vec3 apply_media(vec3 radiance, vec3 origin, vec3 dir, float distance) {
    Air air = world_air();
    // Clear air, which is what almost every scene is. One compare against a push constant, uniform
    // across the whole dispatch, so the branch is free and nothing below it is reached.
    if (!air.any) return radiance;

    // A ray that hit nothing carries no distance -- main leaves it at zero -- and the air in front
    // of it runs to the far plane. That is correct here in a way it was not for a planetary model:
    // a fog layer a hundred metres deep integrates to a finite depth along any upward ray, and along
    // a horizontal one it integrates to opaque, which is exactly what fog looks like at the horizon.
    float length = (distance > 0.0) ? distance : push.lens.y;
    if (length <= 0.0) return radiance;

    float y0 = origin.y + float(push.camera_chunk.y * kChunkEdge);

    float fog_path = layer_integral(y0, dir.y, length, air.fog_base, air.fog_height);
    float haze_path = layer_integral(y0, dir.y, length, air.fog_base, air.haze_height);

    float depth = air.fog_extinct * fog_path + air.haze_extinct * haze_path;
    float transmittance = exp(-min(depth, 40.0));

    // What the surface behind still delivers.
    vec3 result = radiance * transmittance;

    float scattered = 1.0 - transmittance;
    if (scattered <= 1.0e-4) return result;

    // ---- and what the air itself sends towards the eye ------------------------------------
    //
    // Single scattering, integrated ANALYTICALLY rather than sampled.
    //
    // The exact integral carries the sun's transmittance to every point along the ray, which has no
    // closed form. It is evaluated once, at the middle of the scattering, and taken out of the
    // integral. That is the one approximation here and it is a mild one: the sun's path to two
    // points a few hundred metres apart differs by very little, because the two paths share nearly
    // all of their length.
    //
    // What this replaces drew four samples and traced an occlusion ray at each of them. It cost
    // twenty milliseconds of a thirty millisecond frame, and it is what made --fog unusable. Nothing
    // below traces anything, and the whole of it measures as six hundredths of a millisecond.
    //
    // The price is that there are no shafts through GEOMETRY -- no beam through a doorway. Shafts
    // through cloud survive, because the deck is sampled below. That is the deliberate trade for
    // fog that can be left on.
    float cos_theta = dot(dir, trace.sun.xyz);

    // Where along the ray the scattering actually HAPPENS, which is not the halfway point.
    //
    // In-scatter is weighted by the transmittance in front of it, so in thick air most of what
    // reaches the eye was scattered close to the eye and the far half of the ray contributes almost
    // nothing. The median of that distribution is where 1 - e^-ot reaches half of 1 - e^-oL, which
    // for thick air is a small fraction of the way along and for thin air is the midpoint.
    //
    // Sampling the deck at the halfway point instead put the sample kilometres away on a grazing
    // ray, and since the ray's own length varies hugely across a view of flat ground, neighbouring
    // pixels sampled the cloud shadow at wildly separated places. That is the diagonal streaking the
    // first render of this showed: a high-contrast field point-sampled and then used as though it
    // were the average over a long path.
    float sigma = depth / max(length, 1.0e-6);
    float median = sigma > 1.0e-8 ? min(-log(max(1.0 - 0.5 * scattered, 1.0e-30)) / sigma,
                                        length)
                                  : length * 0.5;
    vec3 mid = origin + dir * median;
    vec3 mid_world = world_position(mid);
    // And softened towards one, because a point sample is standing in for an AVERAGE over the whole
    // scattering path. A cloud edge crossing that path dims part of it, never all of it, so taking
    // the point value whole gives an in-scatter that swings five to one where the truth barely
    // moves.
    float deck = mix(1.0, cloud_shadow(mid_world, world_height_metres(mid_world)), 0.55);

    // The sun, dimmed by the deck overhead. A shaft of sun through fog is a shaft only because the
    // light around it was stopped by something, and at this scale that something is cloud. Without
    // it an overcast sky still put full sunbeams through the fog underneath it, and those beams were
    // the brightest thing in a scene whose ground had correctly gone dark.
    vec3 sunlight = sun_radiance() * deck;

    // Skylight, and its absence is what made thick fog go BLACK.
    //
    // The model this replaces lit the air with the sun alone. Put fog under an overcast, or at dusk,
    // and there is no sun -- so the air scattered nothing while still extinguishing everything
    // behind it, and the ground went to black with the sampling noise standing on top of it. That is
    // backwards. Fog is bright in an overcast precisely because it is lit by the WHOLE SKY rather
    // than by one direction, and droplets return nearly all of what reaches them.
    //
    // Isotropic, because a hemisphere averaged over every direction has no lobe left in it.
    vec3 skylight = sky_radiance(vec3(0.0, 1.0, 0.0));
    const float kIsotropic = 1.0 / (4.0 * kPi);

    // Each species with its own coefficient, albedo and phase, weighted by how much of it the ray
    // actually passed through.
    float fog_share = air.fog_extinct * fog_path;
    float haze_share = air.haze_extinct * haze_path;
    float total_share = max(fog_share + haze_share, 1.0e-8);

    vec3 lit = vec3(0.0);
    lit += (fog_share / total_share) * kFogAlbedo *
           (sunlight * henyey_greenstein(cos_theta, air.fog_g) + skylight * kIsotropic * 4.0 * kPi);
    lit += (haze_share / total_share) * kHazeAlbedo *
           (sunlight * henyey_greenstein(cos_theta, 0.76) + skylight * kIsotropic * 4.0 * kPi);

    // Times how much of the beam was scattered at all. This is the change of variable the sampled
    // version did by drawing from the transmittance: the density and the extinction cancel exactly,
    // and what is left is albedo times phase times one minus the transmittance.
    return result + lit * scattered;
}
