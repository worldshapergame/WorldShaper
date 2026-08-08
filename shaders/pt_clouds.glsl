// Cloud, as a volume.
//
// This is the second attempt. The first one was a thresholded value-noise with a height profile on
// it, and it produced something that was undeniably in the sky and undeniably not a cloud: flat
// slabs with soft edges, no interior, and no sense of mass. The lesson was not in the tuning. It
// was that three separate things have to be right before a volume reads as one, and it had none of
// them.
//
//   THE SHAPE HAS TO BILLOW. A cumulus is not a lump of fog, it is a stack of packed round bulges
//   with creases where they meet, because it is made of convection cells. Perlin noise alone
//   cannot produce that at any frequency — it makes smooth blobs. Cellular noise can, because it
//   IS cells, and the standard trick is to remap one by the other rather than to add them.
//
//   THE EDGES HAVE TO ERODE, AND NOT UNIFORMLY. The base of a cumulus is flat and hard, cut off at
//   the condensation level; the top is ragged and wispy where it is still convecting into dry air.
//   The same erosion everywhere gives a uniform fizz that reads as noise on a shape rather than as
//   the shape being made of anything.
//
//   THE MARCH HAS TO SPEND ITS SAMPLES INSIDE. This is the one that mattered most and the one that
//   is invisible in a still description. A uniform step through a fourteen-hundred-metre deck at
//   the step size a horizon ray needs puts three or four samples inside a cumulus, and three or
//   four samples cannot describe an interior — you get its silhouette and a flat fill, which is
//   exactly what the first attempt looked like. The march below takes long strides through empty
//   sky and short ones the moment it finds anything, and that single change is most of the
//   difference between a painted cloud and one you can see into.
//
// # What decides where clouds are
//
// A weather map: a slow two-dimensional field over the world that says, for each place, how much
// cloud there is and what KIND. Real weather is organised horizontally — a front, a shower, a
// clear patch — and driving everything from one three-dimensional noise gives an evenly speckled
// sky with no weather in it. The type value moves continuously from stratus through cumulus to a
// towering cumulonimbus, and it chooses the vertical profile, so one sky can hold a flat sheet at
// one end and a tower at the other without either being a special case.
//
// # And the three étages
//
// Height decides what a cloud is made of, so the decks are separate and behave differently:
//
//   LOW, 600 to 2200 m. Water. Cumulus and stratus. Dense, sharply defined, and the only ones that
//   put a real shadow on the ground.
//
//   MIDDLE, 2600 to 6000 m. Altocumulus: smaller cells, flatter, in rows.
//
//   HIGH, 6500 to 12000 m. Ice, not water. Cirrus: streaked hard along the wind, thin enough to
//   see the sun through, and it never shadows anything.

// # A note on the units, because they cost an hour
//
// `--cam` takes METRES. Everything inside these shaders is in VOXELS, at thirty-two to the metre,
// relative to the camera's own chunk. `world_position` is where the two meet, and it converts all
// THREE axes — height alone was converted once, which pinned the whole sky to the player
// horizontally while leaving it correct vertically.
//
// The previous commit message claims that looking down from three thousand metres shows no cloud
// and calls it an undiagnosed fault. It is not a fault. The test was written as `--cam
// "0,96000,..."` in the belief that the argument was voxels, which put the camera ninety-six
// kilometres up — above every deck, where the slab test correctly finds nothing and returns
// immediately. At three thousand metres the same view shows exactly what it should: cumulus tops
// catching the sun, shadowed flanks, and the deck receding to the horizon.
//
// Recorded here rather than quietly dropped, because the claim is in the history and somebody will
// otherwise go looking for a bug that was never there.

// --- constants ---------------------------------------------------------------------------------

const float kVoxelsPerMetre = 32.0;
float metres(float m) { return m * kVoxelsPerMetre; }

// Where the decks sit, in metres. Numbers out of a meteorology table, kept recognisable as such.
const float kLowBase = 600.0;
const float kLowTop = 2200.0;
const float kMidBase = 2600.0;
const float kMidTop = 6000.0;
const float kHighBase = 6500.0;
const float kHighTop = 12000.0;

// Extinction per metre through density one. A hundred metres of fair-weather cumulus leaves about
// a quarter of the light, which is why a thin edge shows the sun's disc and a core does not.

// Droplets scatter nearly everything they intercept and absorb almost nothing, which is why clouds
// are white. Under one so a deep core still darkens.

// Octaves of the multiple-scattering approximation. A photon in a cloud bounces thousands of
// times; three octaves, each dimmer and broader and less directional, gets the property that
// matters — a thick interior glows instead of going black.

// What the octaves cannot reach.
//
// A cloud is bright because light inside it scatters hundreds of times, not three. Three octaves
// sum to under twice the single-scattering term, and a surface lit by the same sun through a
// Lambert lobe returns about a third of the incident light — so a physically-written cloud with
// three octaves comes out DARKER than a grey wall, which is the one thing everybody knows a cloud
// is not. This stands in for the orders that are not simulated. It is a fudge and it is labelled
// as one; the honest alternative is hundreds of octaves nobody can afford.

// The extinction the SUN march uses, as a fraction of the real one.
//
// The real figure is right for how much a cloud hides what is behind it and catastrophically wrong
// for how much light gets into it. Six taps growing from forty metres reach about sixteen hundred
// metres, and sixteen hundred metres at the true extinction is an optical depth near a hundred —
// exp of which is nought to forty decimal places. Every point that was not within a few metres of
// the surface received exactly no sun, so every cloud rendered flat grey with a thin bright rim.
//
// A real cloud is not dark inside, and the reason is the thing this whole file approximates: light
// that enters anywhere diffuses everywhere. Marching straight lines at the true extinction models
// a cloud made of soot. This is the single number that decides whether clouds are white or grey.

// How much of the sky fills a cloud from below and behind. What stops the shaded side going black
// and what makes an overcast bright grey rather than dark.
const float kSkyFill = 0.5;

// The least sunlight a cloud shadow leaves on the ground. A shadow on a real day is a stop or two,
// not a blackout: the rest of the sky still lights the ground by a path with no cloud in it.
const float kShadowFloor = 0.20;

// How far a sky ray is worth marching, in metres.
// Forty-five kilometres, not ninety. Aerial perspective has taken cloud most of the way to the sky
// colour by then — see kAirVisibility — so what is beyond it costs steps and shows nothing. Cutting
// at a FIXED distance also means every ray stops at the same place, which is what keeps the
// sampling rate from varying between neighbouring pixels.
const float kCloudFarMetres = 150000.0;

// How many full-resolution pixels wide a cloud sample is. See the head of shaders/clouds.comp:
// four by four is sixteen times fewer marches, and cloud is the one thing on screen with no detail
// at the pixel level to lose.
const int kCloudScale = 4;

// Over what distance the erosion detail fades out, in metres.

// How far the march may travel before it asks the weather again. See cloud_density.

// How far the march may travel before it walks to the sun again. See the note in cloud_march.

// The Earth's radius, in metres, and it is here for the horizon.
//
// A flat infinite deck reaches the horizon line exactly and piles up against it: a ray a degree
// above horizontal crosses tens of kilometres of cloud and comes back solid, so the sky ends in a
// hard white band with the deck's whole depth compressed into it. That band is not a rendering
// artefact, it is what an infinite flat deck genuinely looks like — and the reason nobody has seen
// one is that the real deck curves away.
//
// Dropping the deck by d²/2R sinks it below the horizon at distance, exactly as the real one does:
// at fifty kilometres the drop is nearly two hundred metres and at a hundred it is almost eight
// hundred, which is more than the base altitude. So the band thins and then ends, and there is a
// horizon with sky under it.
const float kEarthRadius = 6371000.0;

// How far cloud stays visible through the air between it and the eye, in metres.
//
// Aerial perspective, and its absence is the other half of the horizon band. Air scatters, so a
// cloud twenty kilometres off is paler and bluer than the same cloud overhead, and eventually it
// is the sky. Without it every cloud is as saturated at the horizon as at the zenith, which reads
// as a wall rather than as distance.
// Eighteen kilometres, and it is doing two jobs.
//
// The first is aerial perspective for its own sake. The second is hiding the far edge of the
// march: cloud is cut off at a fixed distance so that every ray stops in the same place, and if
// the air has not taken it most of the way to the sky colour by then, that cut is a visible line
// of clouds sliced off in mid-air. At eighteen kilometres of visibility a cloud at the
// forty-five-kilometre limit is down to eight per cent of itself, which nothing can see going.
const float kAirVisibility = 18000.0;

// --- the world's own position ------------------------------------------------------------------
//
// Everything else in these shaders works in voxels relative to the CAMERA'S CHUNK, which moves as
// the player walks. That is the right space for geometry — it keeps the numbers small near the
// eye — and it is the wrong space for weather, which has to stay where it is while the player
// moves through it.
//
// The height was already converted, because a deck at two kilometres obviously cannot be measured
// from a camera that is itself at two kilometres. The horizontal axes were NOT, and the result is
// the fault this exists to fix: the local x and z reset by a chunk every time the player crosses
// a boundary, so the sampling position never leaves one eight-metre chunk however far anybody
// walks. The whole sky is pinned to the camera and travels with it — and it did not travel
// vertically, because that axis had the correction, which is what made the report so precise.
//
// So the march runs in absolute world voxels on all three axes. The numbers are larger, and the
// integer hashing underneath does not care: that is exactly why it was changed away from a sine.
vec3 world_position(vec3 local) {
    return local + vec3(push.camera_chunk.xyz) * float(kChunkEdge);
}

// The height of an ABSOLUTE position, in metres.
float world_height_metres(vec3 world) {
    return world.y / kVoxelsPerMetre;
}

// --- noise ---------------------------------------------------------------------------------
//
// Two kinds, and the difference between them is the difference between fog and cloud.

// Hashed as INTEGERS, and this is not a style preference.
//
// The obvious hash is `fract(sin(dot(cell, some_vector)) * 43758.5)`, and it works beautifully
// near the origin and falls apart a long way from it. A sky ray runs to ninety kilometres — three
// million voxels — so the detail noise is evaluated at cell coordinates in the hundreds and the
// dot product reaches several hundred thousand. A 32-bit float carries about seven digits, so at
// that magnitude there is almost no fractional precision left for the sine to work with: adjacent
// cells hash to the SAME value, the noise stops varying, and what shows through instead is the
// lattice it was built on — clouds cut off along flat, axis-aligned, right-angled edges.
//
// Integer mixing has no such range. The cell coordinate is exact as an int, the mixing is exact,
// and the result is as good at three million voxels as at three.
uint cloud_mix(uint h) {
    h ^= h >> 16; h *= 0x7feb352du;
    h ^= h >> 15; h *= 0x846ca68bu;
    h ^= h >> 16;
    return h;
}

uint cloud_key(ivec3 cell) {
    return cloud_mix(uint(cell.x) * 0x9E3779B9u ^ cloud_mix(uint(cell.y) * 0x85EBCA6Bu ^
                                                            cloud_mix(uint(cell.z) * 0xC2B2AE35u)));
}

float cloud_hash1(vec3 cell) {
    return float(cloud_key(ivec3(cell)) >> 8) * (1.0 / 16777216.0);
}

vec3 cloud_hash3(vec3 cell) {
    uint h = cloud_key(ivec3(cell));
    return vec3(float(h & 0x3FFu), float((h >> 10) & 0x3FFu), float((h >> 20) & 0x3FFu)) *
           (1.0 / 1023.0);
}

// Ordinary gradient-ish value noise: smooth, and on its own it makes fog.
float perlin_noise(vec3 p) {
    vec3 cell = floor(p);
    vec3 f = p - cell;
    f = f * f * (3.0 - 2.0 * f);
    float n000 = cloud_hash1(cell + vec3(0, 0, 0));
    float n100 = cloud_hash1(cell + vec3(1, 0, 0));
    float n010 = cloud_hash1(cell + vec3(0, 1, 0));
    float n110 = cloud_hash1(cell + vec3(1, 1, 0));
    float n001 = cloud_hash1(cell + vec3(0, 0, 1));
    float n101 = cloud_hash1(cell + vec3(1, 0, 1));
    float n011 = cloud_hash1(cell + vec3(0, 1, 1));
    float n111 = cloud_hash1(cell + vec3(1, 1, 1));
    return mix(mix(mix(n000, n100, f.x), mix(n010, n110, f.x), f.y),
               mix(mix(n001, n101, f.x), mix(n011, n111, f.x), f.y), f.z);
}

// The turn between octaves, kept because the detail field still uses several.
//
// Scaling alone leaves every octave on the same cubic lattice, differing only in spacing. Their
// features line up along the world axes and reinforce each other there, so the sum carries a faint
// grid -- which reads as the sky repeating, because the eye finds the axes even when it cannot find
// the period. A fixed rotation costs six multiplies and puts each octave on its own axes.
const mat3 kOctaveTurn = mat3( 0.00,  0.80,  0.60,
                              -0.80,  0.36, -0.48,
                              -0.60, -0.48,  0.64);

// Remapping one range onto another, which is how the pieces below are combined.
//
// Combining noises by ADDING them averages away exactly the structure each was chosen for: add a
// billow to a shape and you get a blurrier shape. Remapping instead uses the second to carve the
// first — the shape decides where there is cloud at all, and the billow decides what its surface
// does — and that is what keeps a solid core with a bulging rim.
// CLAMPED, and leaving that out is what made the sky a solid slab.
//
// Every use of this is a ramp: nought below here, one above there, and something in between. The
// unclamped version keeps going. `remap(h, 0.0, 0.12, 0.0, 1.0)` is the flat base of a cumulus and
// it reaches one at a height of 0.12 — and then eight and a third at the top of the deck, because
// nothing stopped it. Multiplied by the other factor, which had run negative for the same reason,
// the profile came out saturated at one across the whole deck instead of describing a shape.
//
// So the decks had no vertical structure at all: no flat base, no rounded top, no thinning at the
// edges. Every one was a uniform slab, and a uniform slab with noise in it is exactly what the
// renders looked like — an overcast with cells stamped on it rather than clouds with tops and
// bottoms.
float remap(float value, float from_low, float from_high, float to_low, float to_high) {
    float t = clamp((value - from_low) / max(from_high - from_low, 1e-5), 0.0, 1.0);
    return to_low + t * (to_high - to_low);
}

// --- the deck ----------------------------------------------------------------------------------
//
// This model is Derivative's, ported from its CloudVolumeDensity. What follows is the reasoning
// behind each piece rather than a transcription, because two of the pieces are the answer to faults
// this file spent a long time failing to fix by other means.
//
// One PARAMETERISED deck rather than three hand-written ones. Base, thickness, coverage, density
// and noise scale are the whole description of a cloud layer -- a stratus and a cumulonimbus differ
// in those five numbers and in nothing structural -- so three decks cost what one costs, because no
// two of them overlap in height and only ever one is evaluated.
struct Deck {
    float base;         // metres
    float thickness;    // metres
    float coverage;     // 1.0 asks for no threshold at all; above it adds cloud, below it removes
    float density;
    float noise_scale;  // per metre
    float lit;          // how much sunlight this deck takes
    float sky;          // and how much skylight
};

// Roughly two and a half kilometres to the largest feature, which is about right for a cumulus
// field: the deck reads as a scatter of separate heaps rather than as one texture.
const float kNoiseScale = 4e-4;

// The wind, and it is applied ONCE in world space rather than per octave.
//
// The original subtracts it again at every octave, so each octave drifts at a different rate and the
// cloud churns as it travels instead of sliding rigidly. It looks good and it is incompatible with
// how this renderer draws clouds: fifteen pixels in sixteen are carried from the previous frame and
// re-projected, and a re-projection can only follow a field that MOVES rigidly. A field that churns
// cannot be followed by any transform, so every edge disagreed with its own history for ever and
// the disagreement showed as a permanent stipple along it -- reported as dithering, and it never
// settled no matter how long the camera was held still.
//
// Applied once, the whole field is a pure translation. The reprojection is then exact, and the
// stipple has nothing to feed on.
const vec3 kWindNoise = vec3(2e-3, 2e-4, 1e-3);

// Chosen by how it looks, not by arithmetic. The world's clock runs sixty times the player's, and
// honest cumulus at honest speed would cross the sky at a few hundred metres a second.
const float kCloudSpeed = 0.08;

vec3 cloud_wind() { return kWindNoise * push.sky_cloud.y * kCloudSpeed; }

// How fast the pattern crosses the world, in metres per second of game time. The reprojection needs
// this to follow a cloud rather than the pixel it used to be under, and it is derived here rather
// than passed in because it depends on the noise scale, which is a property of this file.
vec2 cloud_drift() { return kWindNoise.xz * kCloudSpeed / kNoiseScale; }

// The three decks, as the port left them.
//
// A more thorough version of this was written and taken out: real etage altitudes, a weather field
// per level so the decks were independent, altocumulus given its own much smaller cell size, cirrus
// drawn out into filaments along a jet-stream wind. Every piece of it was defensible and the result
// was worse -- the middle deck came out as a lid over the whole sky and the cumulus underneath, the
// thing actually worth looking at, was never visible again. It is recorded here rather than in the
// history because the next person to have those ideas should know they have been tried.
Deck deck_at(float height_m) {
    Deck d;
    float asked = clamp(push.sky_cloud.x, 0.0, 1.0);
    if (height_m <= kLowTop) {
        // The cumulus deck, and the one the model was tuned for.
        d.base = kLowBase;
        d.thickness = kLowTop - kLowBase;
        d.coverage = 0.98 + asked * 0.55;
        d.density = 1.0;
        d.noise_scale = kNoiseScale;
        d.lit = 1.0;
        d.sky = 1.0;
    } else if (height_m <= kMidTop) {
        // Altocumulus: the same model with a thinner deck and a coarser noise, which is what the
        // difference between the two actually is.
        d.base = kMidBase;
        d.thickness = kMidTop - kMidBase;
        d.coverage = 0.82 + asked * 0.45;
        d.density = 0.55;
        d.noise_scale = kNoiseScale * 0.55;
        d.lit = 1.1;
        d.sky = 1.0;
    } else {
        // Cirrus. Ice, so it is thin and it lets far more light straight through.
        d.base = kHighBase;
        d.thickness = kHighTop - kHighBase;
        d.coverage = 0.74 + asked * 0.36;
        d.density = 0.22;
        d.noise_scale = kNoiseScale * 0.30;
        d.lit = 1.4;
        d.sky = 1.2;
    }
    return d;
}

// --- density -------------------------------------------------------------------------------
//
// `steps` is how many octaves of the shape to take: five for the view march, fewer for the light
// marches, where the question is only how much is in the way.

// The weather, and this is the piece that was missing entirely.
//
// A very low frequency field over the world -- one feature every few hundred kilometres -- that
// scales the whole density before anything else touches it. Without it a sky driven by one noise is
// evenly busy from horizon to horizon: no clear quarter, no bank of cloud over there and blue
// overhead, which is what a real sky always has and what every render of the old model lacked.
//
// It also pays for itself. Below a tenth it returns nothing at all, and that early exit skips the
// entire octave loop over whatever fraction of the sky happens to be clear.
float local_coverage(vec3 world_p) {
    vec2 at = (world_p.xz / kVoxelsPerMetre - cloud_drift() * push.sky_cloud.y) * 2e-7;
    float v = perlin_noise(vec3(at * 256.0, 0.0));
    return clamp(v * 3.0 - 0.4, 0.0, 1.0) * 0.5 + 0.5;
}

float cloud_volume_density(vec3 world_p, float height_m, int steps, float detail) {
    float local = local_coverage(world_p);
    if (local < 0.1) return 0.0;

    Deck deck = deck_at(height_m);

    vec3 drift = vec3(cloud_drift().x, 0.0, cloud_drift().y) * push.sky_cloud.y;
    vec3 position = (world_p / kVoxelsPerMetre - drift) * deck.noise_scale;

    // TURNED and offset between octaves, which does the decorrelating the per-octave wind used to
    // do without costing the rigidity.
    //
    // Scaling alone leaves every octave on the same cubic lattice. Their features then line up along
    // the world axes and reinforce each other there, which does two things: it puts a faint grid on
    // the sky, and it raises the peaks enough that far more of the field clears the coverage
    // threshold. The second is expensive rather than ugly -- every extra lit sample pays for a sun
    // march -- and it took the frame from ten milliseconds to sixty-one.
    //
    // A rotation and a fixed offset per octave cost six multiplies and three adds. Crucially the
    // whole field is still a function of (position - drift) alone, so it remains a rigid translation
    // and the reprojection can still follow it.
    float density = detail * 0.03, weight = 0.5;
    for (int i = 0; i < steps; ++i, weight *= 0.5) {
        density += weight * perlin_noise(position);
        position = kOctaveTurn * position * 3.0 + vec3(19.37, 7.11, 31.73);
    }
    density += 0.5 / 3.0 / float(steps);

    if (density < 1e-6) return 0.0;
    density *= local;

    // The vertical shape, and it is SUBTRACTIVE, which is the whole trick.
    //
    // Every previous version of this file multiplied a noise by a height profile, and a noise times
    // a profile is an EXTRUSION: the profile decides the deck's silhouette from the side and every
    // cloud in it starts and ends at the same two heights. That is where "layers stacked on top of
    // each other" came from, and no amount of retuning the profile could fix it, because the fault
    // was the multiplication and not the profile.
    //
    // Subtracting a height-dependent amount instead makes the THRESHOLD rise with height. Near the
    // base almost any noise clears it; near the top almost none does; and where a given cloud stops
    // depends on how strong the noise happens to be at that spot -- so two clouds side by side have
    // different tops, which is the difference between a field of heaps and a slab with texture on
    // it. Flat bases and rounded tops fall out of the arithmetic rather than being drawn.
    float h = clamp((height_m - deck.base) / deck.thickness, 0.0, 1.0);
    float attenuation = clamp(h * 6.6, 0.0, 1.0) * clamp((1.0 - h) * 2.0, 0.0, 1.0);

    density = deck.coverage == 1.0
                  ? density
                  : clamp((density - 1.0 + deck.coverage) / deck.coverage, 0.0, 1.0);

    density *= attenuation * 1.9;
    density -= attenuation * 0.9 + h * 0.5 + 0.1;

    return clamp(density * 3.0 * deck.density, 0.0, 1.0);
}

// The original has a detail field keyed to the view DIRECTION here, mixed in at distance, and it is
// deliberately NOT ported.
//
// It is a sound trick where it comes from: one evaluation seeds the whole ray with high-frequency
// variation, and it is only used far away, where a world-space octave of the same frequency would be
// under a pixel anyway. But it is fixed to the view sphere rather than to the world, so it turns
// with the camera — and it is read through a temporal history that assumes the opposite, that what a
// pixel saw last frame is still out there in the world. Under rotation the two disagree, and what
// comes out is a pattern of rings centred on the view axis that swims as you look around.
//
// Reported as "circles, or a circular level of detail". The distance mix is kept below with a
// constant, which is what the near field used in any case.

// --- lighting ---------------------------------------------------------------------------------

float phase_hg(float cos_angle, float g) {
    float gg = g * g;
    float d = 1.0 + gg - 2.0 * g * cos_angle;
    return (1.0 - gg) / (4.0 * kPi * max(d * sqrt(max(d, 1e-4)), 1e-4));
}

// Cornette-Shanks: Henyey-Greenstein with the Rayleigh (1 + cos^2) term folded in. It keeps a
// sharper peak at small angles than HG at the same g, which is what puts the bright edge on a cloud
// that sits between the eye and the sun.
float phase_cs(float cos_angle, float g) {
    float gg = g * g;
    float a = (1.0 - gg) / (2.0 + gg) * 3.0 / kPi;
    float d = 1.0 + gg - 2.0 * g * cos_angle;
    return a * (1.0 + cos_angle * cos_angle) * pow(max(d, 1e-4), -1.5) * 0.125;
}

// FOUR phase functions, not one, and this is the piece that carries the multiple scattering.
//
// A photon that has bounced once still remembers where it came from and scatters sharply forward. A
// photon that has bounced twenty times has forgotten entirely and scatters almost uniformly, and it
// has been attenuated far less than its path length suggests, because it went round rather than
// through. Modelling that with a single phase and a single extinction gives a cloud with a lit rim
// and a dead flat interior -- which is what a photograph of a cumulus is emphatically not.
//
// Each lobe below is broader and dimmer than the one before it, and each is paired further down
// with its own extinction multiplier. Together they are a cheap stand-in for the whole bounce
// series, and they are the difference between a grey shape and a cloud with light living inside it.
vec4 cloud_phases(float cos_angle) {
    const float forward = 0.6, backward = -0.4;
    const float back_weight = 0.25, peak = 0.1;
    vec4 p;
    p.x = phase_hg(cos_angle, forward)       * 0.70 + phase_hg(cos_angle, backward)       * back_weight        + phase_cs(cos_angle, 0.9) * peak;
    p.y = phase_hg(cos_angle, forward * 0.7) * 0.35 + phase_hg(cos_angle, backward * 0.7) * back_weight * 0.60 + phase_cs(cos_angle, 0.6) * peak * 0.5;
    p.z = phase_hg(cos_angle, forward * 0.5) * 0.17 + phase_hg(cos_angle, backward * 0.5) * back_weight * 0.30 + phase_cs(cos_angle, 0.4) * peak * 0.2;
    p.w = phase_hg(cos_angle, forward * 0.3) * 0.08 + phase_hg(cos_angle, backward * 0.3) * back_weight * 0.20 + phase_cs(cos_angle, 0.2) * peak * 0.1;
    return p;
}

// How much cloud stands between a point and the sun.
//
// Four samples with a step that DOUBLES each time. Six hundred metres of reach out of four
// evaluations, and the reach is what matters: the first short steps decide whether this point is
// just under the surface or deep inside -- the difference between a bright rim and a dark core --
// while the long ones only need the rough total from there to the top of the deck.
float sun_optical_depth(vec3 p, float height_m, vec3 to_sun, float thickness) {
    const int kSteps = 4;
    float length_m = thickness * (0.2 / float(kSteps));
    float step_m = length_m;
    float travelled = 0.0;
    float optical = 0.0;
    for (int i = 0; i < kSteps; ++i) {
        step_m *= 2.0;
        travelled += step_m;
        optical += cloud_volume_density(p + to_sun * metres(travelled),
                                        height_m + to_sun.y * travelled, 5, 1.0);
    }
    return optical * length_m * 0.12;
}

// And how much stands between it and the open sky, which is straight up. Two samples: skylight
// arrives from every direction at once, so it is already an average and does not repay precision.
float sky_optical_depth(vec3 p, float height_m, float thickness) {
    const int kSteps = 2;
    float length_m = thickness * (0.2 / float(kSteps));
    float step_m = length_m;
    float travelled = 0.0;
    float optical = 0.0;
    for (int i = 0; i < kSteps; ++i) {
        step_m *= 2.0;
        travelled += step_m;
        optical += cloud_volume_density(p, height_m + travelled, 3, 1.0);
    }
    return optical * length_m * 0.04;
}

// What a cloud is worth along a ray.
//
// A FIXED step count, and the significance of that is worth stating plainly, because this file has
// been through four different adaptive schemes and every one of them left a mark on the picture:
// a horizontal seam across the sky, hair-like gaps where neighbouring rays disagreed, faces sliced
// flat where the march stepped back into itself. Each fault was a different symptom of one cause --
// two neighbouring pixels sampling the same volume at different rates.
//
// A fixed count cannot do that. Every ray divides its own span into the same number of pieces, so
// neighbouring rays always agree, and the entire class of artefact goes away by construction rather
// than by tuning. It is affordable here only because the density function above is cheap enough and
// shaped sharply enough that sixteen samples resolve an edge -- with a smooth threshold and no
// subtractive carve it would need the seventy-two the old one used.
vec3 cloud_march(vec3 origin, vec3 dir, float height_origin_m, float max_distance_m,
                 out float transmittance) {
    transmittance = 1.0;
    vec3 scattered = vec3(0.0);

    // Where the ray is inside the slab that holds every deck. Rays that never enter it -- most of
    // them, looking at the ground or along the horizon below the base -- leave immediately.
    float enter_m = 0.0;
    float leave_m = min(max_distance_m, kCloudFarMetres);
    if (abs(dir.y) < 1e-4) {
        if (height_origin_m < kLowBase || height_origin_m > kHighTop) return scattered;
    } else {
        float to_base = (kLowBase - height_origin_m) / dir.y;
        float to_roof = (kHighTop - height_origin_m) / dir.y;
        enter_m = max(min(to_base, to_roof), 0.0);
        leave_m = min(max(to_base, to_roof), leave_m);
        if (leave_m <= enter_m) return scattered;
    }

    // The step is a function of DISTANCE TRAVELLED and of nothing else, and this file has learned
    // that the hard way four separate times.
    //
    // Dividing the ray's own span by a fixed count — which is what the original does, and what this
    // port did at first — makes the step depend on where the ray LEAVES the slab. That distance
    // jumps across the horizon and it explodes when the eye is between the decks: a ray a degree
    // off horizontal runs the full forty kilometres while its neighbour a degree up exits in two,
    // so the two are integrated at a hundred times the sampling rate of each other. From inside the
    // deck that came out as streaks radiating from the vanishing point, which is exactly what the
    // horizon seam and the hair artefacts were before, in a new geometry.
    //
    // A schedule in absolute distance cannot do that. Every ray in the frame uses the SAME step at
    // the same point along itself, so neighbours always agree no matter how their spans differ.
    // Short-span rays simply finish early, which costs nothing.
    // Forty steps on this schedule reach a hundred and thirty kilometres, which is past where the
    // deck meets the true horizon: a base at six hundred metres disappears over the curve at about
    // eighty-seven kilometres, and the high deck at twelve at a good deal more. Reaching it is what
    // lets the cloud run all the way down to the horizon line rather than stopping at a fixed
    // distance and leaving a ring of sky under it.
    //
    // The far steps are nearly free. Past the first cloud the transmittance test ends the ray, and
    // where there is no cloud the weather field rejects the sample before any octave is evaluated.
    const int kMaxSteps = 44;
    const float kStepNear = 160.0;   // metres at the eye
    const float kStepGrow = 1400.0;  // and doubling roughly every this many
    // CAPPED, because unbounded growth reaches the horizon by sampling it at nothing. At forty
    // kilometres the schedule had run out to a stride of four and a half thousand metres, and a
    // cumulus is one thousand across — so the whole horizon band was being described by strides
    // several clouds long, which came out as horizontal smearing all along it. Past the cap the
    // march advances evenly, and it still reaches far enough that the haze has taken the cloud to
    // within a few per cent of the sky colour before the ray ends.
    const float kStepFar = 1400.0;
    // Where the sun and sky marches stop being worth their cost. See the light marches below.
    const float kFullLightMetres = 14000.0;
    int steps = kMaxSteps;

    float cos_angle = dot(dir, trace.sun.xyz);
    vec4 phases = cloud_phases(cos_angle);
    vec3 sunlight = sun_radiance();
    vec3 ambient = sky_radiance(vec3(0.0, 1.0, 0.0)) * kSkyFill;
    // The air's own colour in this direction, for the aerial perspective. Once, outside the loop:
    // it does not change along the ray and it is not cheap.
    vec3 air_colour = sky_radiance(dir);

    float scattering_sun = 0.0;
    float scattering_sky = 0.0;
    float travelled = enter_m;

    for (int i = 0; i < steps; ++i) {
        if (transmittance < 0.01 || travelled >= leave_m) break;
        // Ends the ray when the AIR has ended it, which is what makes the distance read as infinite
        // rather than as far. Past here a step's contribution has been replaced by the sky's own
        // colour to within a few per cent, so what remains is the sky whether it is marched or not
        // -- and unlike a cut at a fixed distance, this leaves no edge, because there is nothing on
        // either side of it to differ.
        if (exp(-travelled / kAirVisibility) < 0.04) break;
        float step_m = min(kStepNear * (1.0 + travelled * (1.0 / kStepGrow)), kStepFar);
        travelled += step_m;

        vec3 at = origin + dir * metres(travelled);
        // Curving away with distance. See kEarthRadius: this is what puts an end to the deck
        // instead of letting it pile up against the horizon line.
        float h = height_origin_m + dir.y * travelled -
                  travelled * travelled / (2.0 * kEarthRadius);
        if (h < kLowBase || h > kHighTop) continue;

        float density = cloud_volume_density(at, h, 5, 1.0);
        if (density < 1e-4) continue;

        // The light marches, and past a certain distance they are simply not worth doing.
        //
        // They are twenty-six of the thirty-one noise evaluations a lit step costs. What they buy is
        // the internal shading of a cloud -- bright top, dark base, the light living inside it --
        // and by fifteen kilometres the haze has already replaced more than half of that with flat
        // sky colour, by thirty over eighty per cent. Marching towards the sun at forty kilometres
        // computes a shading gradient and then throws almost all of it away.
        //
        // Beyond the fade the optical depths are estimated from the local density instead, which is
        // the right shape and the wrong detail, and the detail is what the haze is removing anyway.
        float thickness = deck_at(h).thickness;
        bool near_enough = travelled < kFullLightMetres;
        float sun_od = near_enough ? sun_optical_depth(at, h, trace.sun.xyz, thickness)
                                   : density * 6.0;

        // The powder term, written as a ratio rather than as a subtraction.
        //
        // On the sunlit side of an edge a point has little cloud above it but a great deal around
        // it, so what reaches it has been scattered sideways out rather than straight through, and
        // it comes out darker than the simple exponential says. This form rises steeply and then
        // runs away as the density approaches one, which brightens dense cores far more than a
        // linear powder does -- and a dense core that is BRIGHTER than its edge is exactly what a
        // sunlit cumulus looks like and what the old model never managed.
        float bounce = (1.0 - exp(-density * 36.0)) * 0.82;
        bounce /= max(1.0 - bounce, 1e-3);

        // The four lobes, each attenuated by its own multiple of the optical depth. The first is
        // nearly the true single-scattering answer; the last has almost stopped attenuating at all.
        float sun_energy = exp(-sun_od * 2.0) * phases.x
                         + exp(-sun_od * 0.8) * phases.y
                         + exp(-sun_od * 0.3) * phases.z
                         + exp(-sun_od * 0.1) * phases.w;

        float sky_od = near_enough ? sky_optical_depth(at, h, thickness) : density * 2.0;
        // The last term is a FLOOR, and it is not a fudge.
        //
        // Four lobes of sun and a two-sample skylight still both go to nothing in the middle of a
        // thick cloud lit from a low angle, and the model then paints it pure black -- which showed
        // up as hard black specks along the shaded rims, because a rim is exactly where the sun
        // march runs the whole length of the cloud. Real cloud never does that: a photon in there
        // has bounced hundreds of times and some fraction of it comes out of every face. This is
        // that fraction, and it is what stops the interior being a hole in the sky.
        float sky_energy = exp(-sky_od) + exp(-sky_od * 0.1) * 0.1 + 0.06;

        float step_t = exp(-density * 0.12 * step_m);
        float taken = bounce * transmittance * (1.0 - step_t);
        scattering_sun += sun_energy * taken;
        scattering_sky += sky_energy * taken;
        transmittance *= step_t;
    }

    if (transmittance > 0.999) return scattered;

    // The twenty-two of the original is in ITS illuminance units, and mine are not those. The bounce
    // term reaches four and a half where the original's did too, but sun_radiance() here is already
    // a physical radiance rather than a normalised illuminance — so the same number came out about
    // twenty times over and every cloud was a white cut-out. Matched against what the previous model
    // produced at the same exposure, which is the only calibration available.
    Deck deck = deck_at(clamp(height_origin_m + dir.y * travelled, kLowBase, kHighTop));
    scattered = scattering_sun * 1.15 * deck.lit * sunlight;
    scattered += scattering_sky * 0.9 * deck.sky * ambient;

    // Aerial perspective. Air scatters, so a cloud twenty kilometres off is paler and bluer than the
    // same cloud overhead and eventually it IS the sky. Without this the horizon is a wall.
    float air = exp(-travelled / kAirVisibility);
    scattered = scattered * air + air_colour * (1.0 - transmittance) * (1.0 - air);
    return scattered;
}

// How much sun reaches a point on the ground through whatever is above it.
//
// Only the LOW deck, and that is a decision rather than a saving: a cumulus at a kilometre throws
// a shadow with an edge on it, altocumulus at four kilometres does no more than dim, and you can
// read a newspaper under cirrus. Marching all three would spend most of its samples on the two
// that contribute nothing an eye could find.
//
// Deliberately not folded into the cached shadow term. That cache averages hundreds of frames and
// these move; cached, a cloud shadow settles into a permanent grey over the whole courtyard
// instead of sweeping across it.
float cloud_shadow(vec3 p, float height_m) {
    if (push.sky_cloud.x <= 0.0) return 1.0;
    vec3 to_sun = trace.sun.xyz;
    if (to_sun.y <= 0.02) return 1.0;
    if (height_m > kLowTop) return 1.0;

    float enter_m = max((kLowBase - height_m) / to_sun.y, 0.0);
    float leave_m = (kLowTop - height_m) / to_sun.y;
    if (leave_m <= enter_m) return 1.0;

    // A step is a LENGTH and the span is not bounded — at a low sun the ray crosses the deck
    // almost horizontally and the distance runs to tens of kilometres. One sample of density times
    // a kilometre is an optical depth in the thousands, exp of which is nought, and the first
    // version of this turned every surface in the world black at noon under a clear sky.
    const int kSteps = 6;
    const float kStepMax = 320.0;
    float span_m = min(leave_m - enter_m, kStepMax * float(kSteps));
    float step_m = span_m / float(kSteps);

    float optical = 0.0;
    for (int i = 0; i < kSteps; ++i) {
        float travelled = enter_m + step_m * (float(i) + 0.5);
        optical += cloud_volume_density(p + to_sun * metres(travelled),
                                        height_m + to_sun.y * travelled, 4, 1.0) * step_m;
    }
    return mix(kShadowFloor, 1.0, exp(-optical * 0.12));
}
