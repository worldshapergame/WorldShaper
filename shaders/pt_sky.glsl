// The sky: a daylight model driven by the sun's elevation, a night with a moon and stars, and
// cloud.
//
// What a ray sees when it hits nothing, and the only light in most scenes. Its own module
// because a real sky - turbidity, a sun that sets, a night, cloud - is a large piece of work
// that has nothing to do with anything else in the renderer.
//
// Three rules everything below is written under, and none of them are style.
//
// IT IS A PURE FUNCTION OF DIRECTION. Every cache here is keyed to a voxel face, and a face's
// irradiance is the average of the sky it can see. A sky that also depended on where the eye
// was standing would be right where it was measured and wrong everywhere else - the
// cache-keyed-to-the-viewer mistake arriving by the back door. So the cloud deck is at infinity
// in direction space rather than at a height above the camera, and push.origin is never read.
//
// THE SUN STAYS AN ORDER OF MAGNITUDE ABOVE THE SKY. Written as a colour to look at rather than
// as radiance, a sky lights every surface flatly from every direction, shadows stop reading,
// and the picture turns to the grey wash a first attempt at this always produces. Everything
// below is calibrated in radiance against trace.sun_colour; nothing here was picked by eye.
//
// NOTHING BRIGHT AND SMALL GOES IN THE LIGHT PATH. An irradiance entry pools 512 samples. A
// point six hundred times the sky around it is caught by one or two of them, so one face in a
// few hundred settles at twice its neighbours - and a cache does not wash out, so it stays. The
// stars and the moon's disc are therefore in sky_view, which only the eye calls, and not in
// sky_radiance, which the face cache calls. The sun's disc is only forty times the day sky, so
// one sample in 512 moves a face by six per cent, and it stays in both.

// Light and materials

// ---------------------------------------------------------------------------------------
// Calibration
//
// main.cpp fixes the sun at (0.4, 0.85, 0.3) normalised - 59.5 degrees up - and chose
// trace.sun_colour so a 0.5-albedo surface facing it lands near mid-grey. That one hour is the
// zero of everything here: at 59.5 degrees this file reproduces the sky it replaces, and every
// other hour moves against it. Should a time of day ever arrive as a push constant these two
// numbers do not follow it - they are a statement about what sun_colour already means, not
// about where the sun is now.
const float kReferenceAirmass = 1.1595;

// Preetham's kilocandelas per square metre into this renderer's radiance. Its zenith at the
// reference hour is 8.107 kcd/m^2, and 0.0097 puts that at 0.0784 - exactly the luminance of
// the hand-picked zenith colour this file replaces, and close in hue too: 0.054/0.079/0.146
// against the old 0.045/0.080/0.155. Nothing tuned under the old sky at noon has to be
// re-tuned; everything that was not noon now moves.
const float kSkyUnit = 0.0097;

// A clean day with a little haze. Sea level with a city on the horizon is 3 to 4, a desert is
// 2. Wanted as a push constant - see the report - because it is the one dial that turns a
// hard blue sky into a milky one and there is no way to reach it from here.
const float kTurbidity = 2.5;

// ---------------------------------------------------------------------------------------
// Preetham's clear sky
//
// Chosen over Hosek-Wilkie because Hosek's nine coefficients are a fitted table - a hundred and
// eighty numbers per turbidity step - and this is evaluated on every ray that escapes, which is
// most of them. Preetham's five are linear in turbidity, so at a fixed turbidity they fold to
// constants at compile time and the whole model is two exponentials and a divide.
//
// What that choice costs, so the next person does not have to find it: Preetham's *chromaticity*
// fit is poor along the horizon and lays a faint magenta on it - green about six per cent under
// red and blue there, measured off a noon frame after tone mapping. It reads as haze and it is
// the price of not carrying the table. Fixing it properly is Hosek-Wilkie, whose whole
// improvement over this is the horizon.
//
// The five Perez coefficients for all three quantities at once: .x luminance, .y chromaticity
// x, .z chromaticity y. Evaluating them as vectors is why the model costs two exp() rather
// than six.
const vec3 kPerezA = vec3( 0.1787 * kTurbidity - 1.4630,
                          -0.0193 * kTurbidity - 0.2592,
                          -0.0167 * kTurbidity - 0.2608);
const vec3 kPerezB = vec3(-0.3554 * kTurbidity + 0.4275,
                          -0.0665 * kTurbidity + 0.0008,
                          -0.0950 * kTurbidity + 0.0092);
const vec3 kPerezC = vec3(-0.0227 * kTurbidity + 5.3251,
                          -0.0004 * kTurbidity + 0.2125,
                          -0.0079 * kTurbidity + 0.2102);
const vec3 kPerezD = vec3( 0.1206 * kTurbidity - 2.5771,
                          -0.0641 * kTurbidity - 0.8989,
                          -0.0441 * kTurbidity - 1.6537);
const vec3 kPerezE = vec3(-0.0670 * kTurbidity + 0.3703,
                          -0.0033 * kTurbidity + 0.0452,
                          -0.0109 * kTurbidity + 0.0529);

// Below five degrees Preetham is outside the range it was fitted over: its zenith luminance
// turns back upwards and its chromaticity runs away into a colour no sunset has. So the model
// is frozen at five degrees and a separate twilight falloff carries it down from there. One
// knee, one reason.
const float kModelFloorElevation = 5.0;   // degrees

float sky_perez_gamma(vec3 dir, vec3 sun) { return acos(clamp(dot(dir, sun), -1.0, 1.0)); }

vec3 sky_perez(float cos_theta, float gamma) {
    float cg = cos(gamma);
    return (1.0 + kPerezA * exp(kPerezB / max(cos_theta, 0.01))) *
           (1.0 + kPerezC * exp(kPerezD * gamma) + kPerezE * (cg * cg));
}

// Luminance and chromaticity straight up, which is what the Perez function is a ratio against.
vec3 sky_zenith(float sun_theta) {
    float t = sun_theta;
    float t2 = t * t;
    float t3 = t2 * t;

    float chi = (4.0 / 9.0 - kTurbidity / 120.0) * (kPi - 2.0 * t);
    float luminance = (4.0453 * kTurbidity - 4.9710) * tan(chi) - 0.2155 * kTurbidity + 2.4192;

    float x = kTurbidity * kTurbidity *
                  ( 0.00166 * t3 - 0.00375 * t2 + 0.00209 * t) +
              kTurbidity *
                  (-0.02903 * t3 + 0.06377 * t2 - 0.03202 * t + 0.00394) +
                  ( 0.11693 * t3 - 0.21196 * t2 + 0.06052 * t + 0.25886);
    float y = kTurbidity * kTurbidity *
                  ( 0.00275 * t3 - 0.00610 * t2 + 0.00317 * t) +
              kTurbidity *
                  (-0.04214 * t3 + 0.08970 * t2 - 0.04153 * t + 0.00516) +
                  ( 0.15346 * t3 - 0.26756 * t2 + 0.06670 * t + 0.26688);
    return vec3(luminance, x, y);
}

// The model speaks in luminance and chromaticity; the renderer speaks in linear sRGB.
//
// Clamped at zero, and that is not tidiness. A deep twilight chromaticity falls outside the
// sRGB gamut and the conversion answers with a negative blue. Negative radiance handed to a
// face's mean is light *subtracted* from a direction, which is a surface being taught that
// part of the sky takes light away - the same class of mistake as averaging in a non-answer.
vec3 xyY_to_rgb(float x, float y, float luminance) {
    float scale = luminance / max(y, 1e-4);
    vec3 XYZ = vec3(x * scale, luminance, (1.0 - x - y) * scale);
    return max(vec3(dot(XYZ, vec3( 3.2406, -1.5372, -0.4986)),
                    dot(XYZ, vec3(-0.9689,  1.8758,  0.0415)),
                    dot(XYZ, vec3( 0.0557, -0.2040,  1.0570))), vec3(0.0));
}

// ---------------------------------------------------------------------------------------
// Twilight
//
// What Preetham stops doing. Its zenith luminance falls only sixfold between noon and sunset
// and then flattens; the real one falls to a twentieth by sunset and then off a cliff, and it
// is the cliff that makes dusk read as dusk rather than as a dim afternoon.
//
// It is not a single exponential - twilight falls faster the deeper it gets, because the shadow
// of the Earth is climbing through the atmosphere - so the exponent is quadratic in how far the
// sun is below the knee. Fitted through published clear-sky zenith luminances, as a fraction of
// this model's own value at noon: sunset 0.075, astronomical twilight 2.5e-5. Civil twilight at
// six degrees down falls out at 0.0032 against a measured 0.002, which is as close as two
// numbers get.
float sky_twilight(float elevation_degrees) {
    float below = max(kModelFloorElevation - elevation_degrees, 0.0);
    return exp2(-(0.207 * below + 0.0343 * below * below));
}

// ---------------------------------------------------------------------------------------
// The sun, as it arrives
//
// How much of the sun survives the air it came through, which is the whole of why a sunset is
// red: the blue is scattered out of the beam first and what is left is what reaches the eye.
//
// Rayleigh at 0.008735 * lambda^-4.08 and aerosol at beta * lambda^-1.3, evaluated at the sRGB
// primaries' dominant wavelengths (685, 532, 440 nm) with Preetham's beta = 0.04608 T - 0.04586.
const vec3 kOpticalDepth = vec3(0.1543, 0.2717, 0.4499);

// Kasten-Young air mass: 1/cos of a flat atmosphere, corrected near the horizon where the
// curve of the Earth is what stops the path being infinite. 1.0 overhead, 38 at the horizon.
float sky_airmass(float sun_sin_elevation) {
    float elevation = degrees(asin(clamp(sun_sin_elevation, -1.0, 1.0)));
    if (sun_sin_elevation > 0.0) {
        return 1.0 / (sun_sin_elevation +
                      0.50572 * pow(96.07995 - (90.0 - elevation), -1.6364));
    }
    // Below the horizon the fit stops being a fit to anything. Continued by hand and steeply,
    // from the horizon's own value so there is no step at the moment of sunset.
    return 37.95 * (1.0 - elevation);
}

// Beer-Lambert with one optical depth per channel takes far too much out at a large air mass,
// and the reason is that a channel is not a wavelength: by the time the beam has crossed twenty
// atmospheres everything still in the red channel is at the transparent edge of it, and stops
// being removed. A single exponential cannot know that and keeps removing.
//
// Measured against what it does to a picture: at two degrees of elevation the honest
// exponential left the sun's disc at a fifth of the glow immediately around it - a black spot
// in a gold sky - where a real low sun is plainly the brightest thing in the frame. Saturating
// the excess air mass towards six brings the disc back to about eight times its own aureole,
// and leaves noon untouched, because at the reference air mass the excess is zero.
const float kAirmassKnee = 6.0;

vec3 sun_transmittance(float airmass) {
    float excess = airmass - kReferenceAirmass;
    float effective = excess / (1.0 + max(excess, 0.0) / kAirmassKnee);
    return exp(-kOpticalDepth * effective);
}

// The horizon is geometry and not an optical depth. An air mass, however large, is a smooth
// number, and it says nothing about the Earth's own edge crossing in front of the disc - which
// is what actually ends a sunset, inside about a degree. Without this the saturation above
// would leave a sun burning three degrees underground.
//
// `dip` is how far below the observer's horizon the beam still clears the Earth at some
// altitude: nothing at ground level, about a degree and a half at the two kilometres a cloud
// deck stands at. That single number is the whole reason the last colour in an evening sky is
// on the undersides of clouds and not in the air.
float sun_horizon_gate(float elevation_degrees, float dip) {
    return smoothstep(-0.9 - dip, 0.35 - dip, elevation_degrees);
}

// What the sun is worth after the air, relative to the reference hour trace.sun_colour was
// chosen at. It is also the answer to something the old sky had no opinion about: a sun a
// degree under the horizon lit every downward-facing surface in the scene at full strength.
vec3 sun_radiance() {
    float elevation = degrees(asin(clamp(trace.sun.y, -1.0, 1.0)));
    return trace.sun_colour.rgb * sun_transmittance(sky_airmass(trace.sun.y)) *
           sun_horizon_gate(elevation, 0.0);
}

// The same beam where a cloud deck stands in it. Two kilometres of altitude is most of the
// aerosol and a fifth of the air.
const float kCloudAirmassShare = 0.45;
const float kCloudHorizonDip = 1.4;   // degrees, the horizon dip at two kilometres

vec3 cloud_sunlight() {
    float elevation = degrees(asin(clamp(trace.sun.y, -1.0, 1.0)));
    return trace.sun_colour.rgb *
           sun_transmittance(sky_airmass(trace.sun.y) * kCloudAirmassShare) *
           sun_horizon_gate(elevation, kCloudHorizonDip);
}

// ---------------------------------------------------------------------------------------
// Noise, for the cloud and the moon's face
//
// Value noise on a hash rather than a texture: this file is meant to be reachable from a ray
// that escapes anywhere, and a sampler bound for the sky would be a binding every other shader
// in the pipeline has to know about.
float sky_hash(vec2 cell) {
    ivec2 i = ivec2(cell);
    uint h = hash_u32(uint(i.x + 4096) * 0x9E3779B9u ^ uint(i.y + 4096) * 0x85EBCA6Bu);
    return float(h) * (1.0 / 4294967296.0);
}

float sky_noise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = p - i;
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(sky_hash(i),                 sky_hash(i + vec2(1.0, 0.0)), f.x),
               mix(sky_hash(i + vec2(0.0, 1.0)), sky_hash(i + vec2(1.0, 1.0)), f.x), f.y);
}

// Four octaves. Three leaves cloud edges visibly smooth and wrong - a cloud's silhouette is
// the one part of it everybody has looked at - and five costs another four hashes for detail
// finer than the deck's own perspective compression preserves.
float sky_fbm(vec2 p) {
    float sum = 0.0;
    float weight = 0.5;
    for (int i = 0; i < 4; ++i) {
        sum += weight * sky_noise(p);
        p *= 2.17;   // not 2: an integer lacunarity lines every octave's cells up on the same
                     // lattice and prints a square grid through the result
        weight *= 0.5;
    }
    return sum * (1.0 / 0.9375);
}

// ---------------------------------------------------------------------------------------
// Cloud
//
// Crude, and it still earns its place: a perfectly clear sky is the one thing no photograph of
// a building has ever had.
//
// The deck is projected as dir.xz / (dir.y + 0.32). The offset in the denominator is not a
// guard against dividing by zero, it is the whole shape of the thing: a true 1/y projection
// puts unbounded detail at the horizon, which aliases into a band that no amount of
// accumulation clears. This compresses the deck towards the horizon the way perspective does
// and then stops, so the last few degrees are thin haze rather than moire.
//
// Static, deliberately. The accumulator averages hundreds of frames of the same sky; a cloud
// that moved between them would smear rather than drift. Animating it needs a time push
// constant AND the CPU resetting the accumulation when it changes, on exactly the rule the
// camera already obeys.
const float kCloudScale = 2.6;
const float kCloudLevel = 0.50;   // fbm above this is cloud: about a third of the sky
const float kCloudSoft  = 0.26;   // and this much of the fbm's range is the edge of one
const float kCloudSunlit = 0.10;  // a sunlit top comes out near five times the zenith sky,
                                  // which is where a real cumulus sits against a real one

// Density of cloud in a direction, 0 clear and 1 a thick core.
float cloud_density(vec3 dir) {
    vec2 p = dir.xz / (dir.y + 0.32) * kCloudScale;
    return smoothstep(kCloudLevel, kCloudLevel + kCloudSoft, sky_fbm(p));
}

// What a cloud is worth, given the clear sky it is standing in.
//
// Lit from above by the sun and filled from below and behind by the sky, which is the whole of
// why a cloud goes orange at sunset and grey under an overcast one: it is not painted, it is
// the colour of whatever is lighting it. Nothing here is a fixed colour, so there is no
// constant that has to be kept in step with the time of day.
vec3 cloud_radiance(vec3 dir, vec3 clear, float thickness) {
    // Scattered without preference for wavelength, so a cloud is much less blue than the air
    // around it even when the air is all that is lighting it.
    float grey = dot(clear, vec3(0.2126, 0.7152, 0.0722));
    vec3 filled = mix(clear, vec3(grey), 0.6);

    // Thin edges take the sun almost undimmed and thick cores are in their own shadow, which
    // is the difference between a cumulus and a grey blanket.
    vec3 body = cloud_sunlight() * (kCloudSunlit * mix(1.0, 0.22, thickness)) +
                filled * mix(2.0, 0.85, thickness);

    // Forward scattering: the rim that lights up when a cloud is between the eye and the sun.
    float forward = max(dot(dir, trace.sun.xyz), 0.0);
    body += cloud_sunlight() * (0.05 * pow(forward, 14.0) * (1.0 - 0.7 * thickness));
    return body;
}

// ---------------------------------------------------------------------------------------
// Night
//
// A physically scaled moonless night is about 2.5e-7 of noon. With no exposure adaptation
// anywhere in the pipeline - the tone map is a plain Reinhard with a gamma, on purpose - that
// is pure black, and a game about building outdoors would have nothing to look at for half of
// its day. So the night is lifted to about a twenty-second of the day sky, four and a half
// stops down, which is as dark as the screen goes and still shows a roofline against it. It is
// the one deliberate lie in this file and it is here rather than spread through it.
//
// Blue because the eye's rods take over below about a hundredth of a candela and they are
// blue-shifted; a night that photographs grey genuinely does look blue to somebody standing in
// it. Not decoration.
const vec3 kNightZenith  = vec3(0.0023, 0.0035, 0.0074);
const vec3 kNightHorizon = vec3(0.0035, 0.0047, 0.0078);

// Grass, gravel, pale stone: everything a building gets put on sits between 0.15 and 0.3.
// Wanted as a push constant along with turbidity - see the report - since it is the one thing
// about the ground the sky is entitled to know.
const float kGroundAlbedo = 0.25;

// ---------------------------------------------------------------------------------------
// The moon
//
// Placed opposite the sun, which is not a guess - a moon opposite the sun *is* a full moon, and
// at midnight it stands where the sun stood at noon. It is the moon that is exactly right one
// night in twenty-nine and never absurd on the others. A phase, and an orbit that is not the
// sun's, want a trace.moon push constant; see the report.
//
// It lights nothing. The disc is six hundred times the night sky around it and cannot go in the
// light path (see the top of this file); what stands in for moonlight is the night gradient
// itself. The glow around it is smooth and low-contrast, so that much is safe to leave in.
const float kMoonDisc = 2.2;      // reads at about 215 of 255 through the tone map
// Narrow on purpose. A wide halo was the first thing tried and it drew a hazy blob thirty
// degrees across with a pin-prick in the middle of it, which reads as a fault in the sky rather
// than as the moon. At this exponent the glow is spent by about ten degrees out.
const float kMoonGlow = 0.045;
const float kMoonGlowFalloff = 110.0;

// ---------------------------------------------------------------------------------------
// Stars
//
// Hashed per cell of a cube around the eye rather than carried as a catalogue, and at most one
// star to a cell so a direction only ever looks at the cell it is in. Nine lookups for a
// neighbourhood would be nine times the cost of a field that is meant to be sparse.
//
// Sixty cells to a cube face is a star every degree and a half at most, and one cell in ten
// carries one: about two thousand, which is what a good sky gives the naked eye. The dot is a
// tenth of a cell across - about a pixel at the game's field of view - and the path tracer's
// own jitter is what antialiases it, so a star that lands between two pixels lands on both,
// dimmer, exactly as a real one does.
const float kStarCells   = 30.0;   // half-width in cells, so sixty across a face
const float kStarDensity = 0.10;
const float kStarRadius  = 0.0033; // in cube coordinates, roughly 0.19 degrees
const float kStarPeak    = 1.6;

vec3 star_field(vec3 dir) {
    vec3 a = abs(dir);
    float dominant = max(a.x, max(a.y, a.z));
    vec3 cube = dir / max(dominant, 1e-6);

    uint face;
    vec2 uv;
    float along;
    if (a.x >= a.y && a.x >= a.z)      { face = 0u; uv = cube.yz; along = dir.x; }
    else if (a.y >= a.z)               { face = 1u; uv = cube.xz; along = dir.y; }
    else                               { face = 2u; uv = cube.xy; along = dir.z; }
    face = face * 2u + ((along < 0.0) ? 1u : 0u);

    vec2 cell = uv * kStarCells;
    vec2 base = floor(cell);
    uint h = hash_u32(uint(int(base.x) + 4096) * 0x9E3779B9u ^
                      uint(int(base.y) + 4096) * 0x85EBCA6Bu ^ (face * 0x27D4EB2Du));

    float pick = float(h & 0xFFFFu) * (1.0 / 65536.0);
    if (pick > kStarDensity) return vec3(0.0);

    // Kept off the cell's edges so no neighbour ever has to be consulted.
    vec2 at = 0.5 + (vec2(float((h >> 16u) & 0xFFu), float((h >> 24u) & 0xFFu)) *
                     (1.0 / 255.0) - 0.5) * 0.66;
    float d = length((cell - base) - at) / kStarCells;

    // Cubed because star counts run about two and a half to the magnitude: for every one worth
    // looking at there are a dozen that are only there to stop the sky being empty.
    float rank = 1.0 - pick / kStarDensity;
    float bright = rank * rank * rank;
    vec3 tint = mix(vec3(1.00, 0.84, 0.68), vec3(0.76, 0.85, 1.00),
                    float((h >> 8u) & 0xFFu) * (1.0 / 255.0));
    return tint * (kStarPeak * bright * (1.0 - smoothstep(0.0, kStarRadius, d)));
}

// ---------------------------------------------------------------------------------------

// The whole sky. `points` adds what only the eye may see: the stars and the moon's disc.
vec3 sky_evaluate(vec3 dir, bool points) {
    float elevation = degrees(asin(clamp(trace.sun.y, -1.0, 1.0)));

    // Below the horizon there is ground: too far off ever to be streamed as geometry. Sampled
    // at a fixed slope in the same bearing, because what a distant plain is worth is the sky
    // over it rather than the sky mirrored in it.
    bool below = dir.y < 0.0;
    vec3 d = below ? normalize(vec3(dir.x, 0.5, dir.z)) : dir;

    // The clear sky. Frozen at the elevation Preetham was fitted down to, with the twilight
    // falloff carrying it from there.
    //
    // The angle to the sun is measured against where the sun actually is, not against the
    // frozen model sun, and the difference is the whole of dusk: a sun three degrees under the
    // horizon puts the nearest point of the sky to it *on* the horizon in its own azimuth, so
    // the aureole sits down there as a band and not overhead as a dome.
    float model_elevation = max(elevation, kModelFloorElevation);
    float sun_theta = radians(90.0 - model_elevation);
    vec3 zenith = sky_zenith(sun_theta);
    vec3 ratio = sky_perez(d.y, sky_perez_gamma(d, trace.sun.xyz)) /
                 sky_perez(1.0, sun_theta);
    vec3 clear = xyY_to_rgb(zenith.y * ratio.y, zenith.z * ratio.z,
                            zenith.x * ratio.x * kSkyUnit * sky_twilight(elevation));

    // The night underneath it, weighted out of the way while the day is still worth anything.
    // Not a crossfade between two skies: the night sky is always there, it is simply swamped,
    // and the only reason it needs a weight at all is that this one is lifted far above where
    // physics would put it.
    // Written the long way round because smoothstep is undefined when its first edge is the
    // larger one, and half the drivers that get it right today are not a promise.
    float night = 1.0 - smoothstep(-6.0, 2.0, elevation);
    clear += mix(kNightHorizon, kNightZenith, clamp(d.y, 0.0, 1.0)) * night;

    if (below) {
        // A diffuse plain under the sun and the whole sky, not a colour. Written that way
        // because it is then the sun that makes it bright, so it goes out when the sun does
        // instead of staying the one lit thing in a night scene - and because a ground that
        // reads as *ground* at noon is a quarter of the light bouncing back up into everything
        // standing on it, which a hand-picked dark grey was quietly not providing.
        vec3 irradiance = sun_radiance() * max(trace.sun.y, 0.0) + clear * kPi;
        vec3 ground = kGroundAlbedo * irradiance * (1.0 / kPi);
        // Aerial perspective: the last few degrees above the ground are more air than ground,
        // which is what stops the horizon being a drawn line.
        return mix(clear, ground, smoothstep(0.0, 0.22, -dir.y));
    }

    vec3 radiance = clear;

    if (points) {
        // Later than the night gradient, because the first star is out well before the sky has
        // finished going dark and the last one is not visible until the sun is properly down.
        radiance += star_field(d) * (1.0 - smoothstep(-11.0, -1.0, elevation));
    }

    // The moon's glow is smooth enough to be light rather than a point, so it goes in whether
    // the eye is asking or the cache is.
    float to_moon = max(dot(d, -trace.sun.xyz), 0.0);
    radiance += vec3(0.72, 0.78, 1.00) * (kMoonGlow * night * pow(to_moon, kMoonGlowFalloff));

    // Cloud last, over everything the sky itself is made of, because a cloud is in front of the
    // stars and in front of the air.
    float thickness = cloud_density(d);
    if (thickness > 0.0) {
        radiance = mix(radiance, cloud_radiance(d, clear, thickness), thickness);
    }

    if (points && to_moon > trace.sun.w) {
        // The maria, so it reads as the moon and not as a hole punched in the sky. Flat, with
        // no limb darkening: the moon has almost none - its dust throws light back the way it
        // came, which is why a full moon looks like a disc and not like a ball.
        vec3 axis = -trace.sun.xyz;
        vec3 up = (abs(axis.z) < 0.999) ? vec3(0, 0, 1) : vec3(1, 0, 0);
        vec3 tangent = normalize(cross(up, axis));
        vec2 face = vec2(dot(d, tangent), dot(d, cross(axis, tangent))) /
                    sqrt(max(1.0 - trace.sun.w * trace.sun.w, 1e-9));
        float mare = smoothstep(0.30, 0.72, sky_fbm(face * 2.4 + 8.0));
        radiance += vec3(0.95, 1.00, 1.09) * (kMoonDisc * night * mix(0.58, 1.0, mare) *
                                              (1.0 - thickness));
    }

    // The sun's disc, so a mirror reflects something rather than a flat gradient, and drawn
    // over the cloud rather than behind it. That is a lie, and it is the smaller one: the sun's
    // light reaches the scene through a push constant this shader cannot touch, so hiding the
    // disc behind a cloud while its shadows still fall sharp across the floor would read as a
    // fault where a sun in front of a cloud only reads as a thin one.
    float to_sun = dot(dir, trace.sun.xyz);
    if (to_sun > trace.sun.w) {
        radiance += sun_radiance() * smoothstep(trace.sun.w, mix(trace.sun.w, 1.0, 0.15), to_sun);
    }
    return radiance;
}

// The sky as light: what a face's irradiance is the average of, and what a reflection reads.
vec3 sky_radiance(vec3 dir) { return sky_evaluate(dir, false); }

// The sky as seen: the above, plus everything too small and too bright to be averaged.
vec3 sky_view(vec3 dir) { return sky_evaluate(dir, true); }
