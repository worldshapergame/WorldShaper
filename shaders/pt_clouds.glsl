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
const float kExtinction = 0.06;

// Droplets scatter nearly everything they intercept and absorb almost nothing, which is why clouds
// are white. Under one so a deep core still darkens.
const float kAlbedo = 0.95;

// Octaves of the multiple-scattering approximation. A photon in a cloud bounces thousands of
// times; three octaves, each dimmer and broader and less directional, gets the property that
// matters — a thick interior glows instead of going black.
const int kScatterOctaves = 3;

// What the octaves cannot reach.
//
// A cloud is bright because light inside it scatters hundreds of times, not three. Three octaves
// sum to under twice the single-scattering term, and a surface lit by the same sun through a
// Lambert lobe returns about a third of the incident light — so a physically-written cloud with
// three octaves comes out DARKER than a grey wall, which is the one thing everybody knows a cloud
// is not. This stands in for the orders that are not simulated. It is a fudge and it is labelled
// as one; the honest alternative is hundreds of octaves nobody can afford.
const float kScatterBoost = 5.0;

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
const float kLightExtinction = 0.16;

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
const float kCloudFarMetres = 45000.0;

// Over what distance the erosion detail fades out, in metres.
const float kDetailFadeMetres = 9000.0;

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

// Cellular noise: the distance to the nearest of a set of scattered points, inverted.
//
// This is the one that makes a cloud a cloud. Its level sets are packed round cells with creases
// between them, which is what convection actually builds — a cumulus is a heap of rising bubbles,
// and a heap of bubbles is exactly what this is. Perlin at any frequency cannot make it, because
// smooth interpolation between lattice values has no cell structure in it at all.
//
// Two by two by two neighbours, not three by three by three.
//
// The textbook version searches twenty-seven cells, because a feature point placed anywhere in a
// neighbouring cell can be the nearest one. Twenty-seven hashes per sample, three octaves per
// fbm, two fbms per density sample and a hundred-odd samples per ray is what took the frame to
// eight a second.
//
// Confining each feature to the middle half of its own cell makes eight enough: a point in this
// cell can then only be beaten by a feature in a cell it shares a corner with, and there are eight
// of those. The cells come out slightly more regular for it, which on a cumulus is invisible —
// convection cells are fairly regular anyway.
float worley(vec3 p) {
    vec3 cell = floor(p);
    vec3 f = p - cell;
    // Which eight: the ones on the side of each axis the point is nearest to.
    vec3 towards = step(vec3(0.5), f) * 2.0 - 1.0;
    float nearest = 1.0;
    for (int i = 0; i < 8; ++i) {
        vec3 neighbour = vec3(float(i & 1), float((i >> 1) & 1), float((i >> 2) & 1)) * towards;
        vec3 feature = neighbour + 0.18 + cloud_hash3(cell + neighbour) * 0.64;
        nearest = min(nearest, length(feature - f));
    }
    // Normalised by how far the nearest feature can actually GET. Confining features to the middle
    // of their cells is what makes eight neighbours enough, and it also means the nearest is never
    // far: the raw distance barely passes two thirds, so the output sat in the top third of its
    // range and the carving it drives never reached full strength.
    return 1.0 - clamp(nearest * (1.0 / 0.72), 0.0, 1.0);
}

// Two octaves rather than three. The third is a fifth of the amplitude and a third of the cost of
// the whole function, and at cloud scale nothing in the picture depends on it.
// Every octave TURNED as well as scaled.
//
// Scaling alone leaves all of them on the same cubic lattice, differing only in spacing. Their
// features line up along the world axes and reinforce each other there, so the sum carries a faint
// grid — which reads as the sky repeating, because the eye finds the axes even when it cannot find
// the period. It was reported as clouds that "repeat slightly", and this is where that comes from:
// not from a period that is too short, but from a lattice that is never hidden.
//
// A fixed rotation between octaves costs six multiplies and two adds and puts each one on its own
// axes. Irrational-ish angles, so no two octaves ever come back into alignment.
const mat3 kOctaveTurn = mat3( 0.00,  0.80,  0.60,
                              -0.80,  0.36, -0.48,
                              -0.60, -0.48,  0.64);

float worley_fbm(vec3 p, float lacunarity) {
    return worley(p) * 0.72 + worley(kOctaveTurn * p * lacunarity) * 0.28;
}

float perlin_fbm(vec3 p, int octaves) {
    float sum = 0.0, amplitude = 0.5, total = 0.0;
    for (int i = 0; i < octaves; ++i) {
        sum += perlin_noise(p) * amplitude;
        total += amplitude;
        p = kOctaveTurn * p * 2.02;   // turned as well as scaled; see kOctaveTurn
        amplitude *= 0.5;
    }
    return sum / max(total, 1e-4);
}

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

// --- the weather map -----------------------------------------------------------------------
//
// A slow two-dimensional field over the world, in kilometres rather than metres, because weather
// is organised at that scale: a front, a shower, a clear patch over the next valley. Driving
// everything from one three-dimensional noise gives an evenly speckled sky with no weather in it.
//
// x  how much cloud is here, 0 to 1
// y  what KIND, 0 stratus, 0.5 cumulus, 1 a tower
struct Weather { float coverage; float type; };

Weather weather_at(vec3 p) {
    // ONE octave each, and this is a hot function: it is evaluated at every sample of the view
    // march, the light march and the shadow march. Five octaves here was most of a frame.
    //
    // It costs nothing to lose them. This field is deliberately slow — it varies over kilometres,
    // because that is the scale weather is organised at — so its high octaves were being computed
    // and then almost entirely ignored by a smoothstep.
    vec2 km = p.xz / metres(1000.0);
    // Two octaves, averaged. One raw octave has a much wider spread than an fbm of the same
    // field — an fbm concentrates around a half — so swapping one for the other silently moved the
    // average coverage up and turned a fair-weather day into a solid overcast.
    float broad = (perlin_noise(vec3(km * 0.35, 0.0)) +
                   perlin_noise(vec3(km * 0.70, 3.0))) * 0.5;
    float patchy = perlin_noise(vec3(km * 1.30, 11.0));

    Weather w;
    // The player's setting is the average and the map is the variation about it, so turning the
    // weather up thickens what is there rather than switching to a different sky.
    // Calibrated against what the base shape actually produces, which is the step that was
    // missing. `shape` averages about two thirds, and the coverage sets the threshold it has to
    // beat — so a coverage of a half means "cloud almost everywhere", not "half the sky". The
    // first three renders of this were solid overcast for exactly that reason and no other.
    //
    // At the default setting this gives a threshold the shape clears in roughly a fifth of the
    // sky, which is a fair-weather day.
    // Calibrated against what the base shape actually produces, which is arithmetic rather than
    // taste. `shape` is remap(perlin, cells - 1, 1, 0, 1), so with both noises averaging a half it
    // averages two thirds and reaches one only where a high Perlin meets a low cell. The coverage
    // sets the threshold that has to be beaten, at 1 - coverage — so a coverage of 0.19 asks the
    // shape to beat 0.81, which it almost never does, and a coverage of 0.5 asks it to beat 0.5,
    // which it nearly always does. The useful range is narrow and sits around a third.
    //
    // At the default setting this centres the threshold near 0.7 and swings it either side with
    // the weather map, so the sky has cloudy quarters and clear ones instead of a uniform dusting.
    // The threshold the shape has to beat is 1 - coverage, and `shape` averages about two thirds.
    // Set the threshold near that average and only isolated PEAKS of the noise clear it, which is
    // why the sky came back as a scatter of small flecks: thresholding near the top of a
    // distribution leaves islands, not clouds. Set it a little below the average and whole
    // contiguous regions clear it, which is what a cumulus field is.
    float asked = clamp(push.sky_cloud.x, 0.0, 1.0);
    // Follows the noise: `shape` is remap(perlin, cells - 1, 1, 0, 1), so when `cells` can reach
    // nought its lower bound reaches minus one and the shape's average rises from about two thirds
    // to three quarters. The threshold is 1 - coverage, so the same number would pass most of the
    // sky. This has to be re-derived whenever the noise changes.
    w.coverage = clamp(asked * 0.40 + 0.04 + (broad - 0.5) * 0.30 + (patchy - 0.5) * 0.12,
                       0.0, 1.0);
    // Towers where the coverage is already high, which is what actually happens: a cumulonimbus
    // grows out of the middle of a crowded field of cumulus, not out of a clear sky.
    w.type = clamp(perlin_noise(vec3(km * 0.22, 5.0)) * 0.8 + w.coverage * 0.5, 0.0, 1.0);
    return w;
}

// --- the vertical profiles -------------------------------------------------------------------
//
// Where in its deck a cloud of a given kind has any body, as a function of height through the
// deck. Three shapes, blended continuously by the type, so a sky can hold a flat sheet at one end
// and a tower at the other with nothing in the code treating either as a case.

// Stratus: a low flat sheet, all of its mass in the bottom fifth.
float profile_stratus(float h) {
    return remap(h, 0.0, 0.07, 0.0, 1.0) * remap(h, 0.20, 0.30, 1.0, 0.0);
}

// Cumulus: a hard flat base at the condensation level, a swelling body, a rounded top.
//
// The flat base is the single most recognisable thing about a cloud and it is not a stylisation:
// it is the height at which rising air has cooled to its dew point, and it is the same height
// everywhere on a given day. That is why a field of cumulus looks like it is resting on glass.
float profile_cumulus(float h) {
    // The body reaches higher than it did. A cumulus is roughly as tall as it is wide, and with a
    // horizontal feature scale near a kilometre a body confined to h 0.12 to 0.55 of the deck is
    // six hundred metres of cloud under eleven hundred metres of width — a pancake. From the
    // ground that reads as flat cut-outs pasted on the sky, which is what it looked like.
    return remap(h, 0.0, 0.10, 0.0, 1.0) * remap(h, 0.72, 1.0, 1.0, 0.0);
}

// Cumulonimbus: all the way up, and still solid near the top where it is about to spread into an
// anvil.
float profile_tower(float h) {
    return remap(h, 0.0, 0.08, 0.0, 1.0) * remap(h, 0.85, 1.0, 1.0, 0.0);
}

float height_profile(float h, float type) {
    float stratus = profile_stratus(h);
    float cumulus = profile_cumulus(h);
    float tower = profile_tower(h);
    float a = mix(stratus, cumulus, clamp(type * 2.0, 0.0, 1.0));
    return clamp(mix(a, tower, clamp(type * 2.0 - 1.0, 0.0, 1.0)), 0.0, 1.0);
}

// --- wind -------------------------------------------------------------------------------------
//
// Faster higher up, which is true and is also what sells the depth: the low deck crawls while the
// cirrus above it streams. Veered as well, because wind turns with altitude, and a sky whose decks
// all drift the same way looks like one deck drawn three times.
vec3 wind_offset(float height_m) {
    vec2 low = push.sky_wind.xy;
    float faster = 1.0 + height_m * (1.0 / 2500.0);
    float turn = height_m * (1.0 / 30000.0);
    float c = cos(turn), s = sin(turn);
    vec2 veered = vec2(low.x * c - low.y * s, low.x * s + low.y * c);
    return vec3(veered.x, 0.0, veered.y) * (faster * kVoxelsPerMetre * push.sky_cloud.y);
}

// --- density ------------------------------------------------------------------------------
//
// `detail` is false for the light march and the shadow march, which skips the expensive erosion
// octaves. Their job is to know roughly how much cloud is in the way, and the fine structure of an
// edge two hundred metres away changes that by nothing an eye could find.

float low_deck_density(vec3 p, float height_m, float detail) {
    float h = clamp((height_m - kLowBase) / (kLowTop - kLowBase), 0.0, 1.0);
    vec3 q = p - wind_offset(height_m);

    Weather w = weather_at(q);
    if (w.coverage <= 0.01) return 0.0;

    float profile = height_profile(h, w.type);
    if (profile <= 0.0) return 0.0;

    // The base shape: Perlin remapped by a Worley fbm, which is the piece that turns fog into
    // billows. The Worley is what the shape's own value is measured AGAINST, so the cloud exists
    // where the smooth field rises above the cell structure — and the boundary between them is a
    // surface of packed bulges rather than a smooth contour.
    //
    // `detail` gates the EROSION and nothing else. It must not gate this.
    //
    // The coarse path is what the striding march uses to decide there is nothing here and take
    // another three hundred metres, and what the light and shadow marches use to count what is in
    // the way. If it disagrees with the detailed answer about WHERE there is cloud, the strider
    // walks through clouds it never sees and stops in clear air — which came out as a sky that was
    // solid overcast in one place and hard vertical slabs in another, and a ground in shadow from
    // cloud that was not above it.
    //
    // So the base shape is the same function for everyone, one Worley octave cheaper. What the
    // coarse path skips is the erosion, which only ever REMOVES density — so the coarse answer is
    // an over-estimate of the fine one, and an over-estimate is the safe direction for a strider:
    // it stops early and creeps, which costs a step and cannot miss anything.
    // ISOTROPIC, and the vertical squash that used to be here was the worst fault in the file.
    //
    // The density is shape(p) times profile(height). Squashing the noise vertically makes `shape`
    // vary slowly in y — nearly a two-dimensional pattern — and a two-dimensional pattern
    // multiplied by a height function is an EXTRUSION. Every cloud became a vertical curtain hung
    // from a flat ceiling, all of them starting and ending at the same two heights, which from
    // inside the deck looks like a roof with stalactites and from below looks like sheets stacked
    // on sheets. It was reported as "layers on top of each other" and that is exactly what the
    // arithmetic was building.
    //
    // Isotropic noise varies as fast vertically as horizontally, so the profile shapes the deck
    // without extruding it: two clouds at the same place have different tops, because `shape` is
    // different at their tops and not merely scaled.
    vec3 base_p = q * (1.0 / metres(850.0));
    float perlin = perlin_fbm(base_p, (detail > 0.0) ? 3 : 2);
    float cells = (detail > 0.0) ? worley_fbm(base_p * 2.4, 2.1) : worley(base_p * 2.4);
    float shape = remap(perlin, cells - 1.0, 1.0, 0.0, 1.0);

    // Coverage carves the SHAPE, and the profile tapers the result. That order is the whole of it.
    //
    // Multiplying by the profile first and thresholding after makes the threshold height-dependent:
    // shape has to beat 1 - coverage after being scaled down by the profile, so only the height
    // where the profile is near one can clear it at all. The cloud is then a slab at the profile's
    // peak with nothing above or below, however tall the profile said the deck was — which is why
    // they stayed flat lozenges after the extrusion was fixed.
    //
    // Carving first lets the coverage decide WHERE there is cloud, in three dimensions, and leaves
    // the profile to decide how it thins towards its base and its top. Same two numbers, and the
    // difference between a slab and a cumulus.
    float density = remap(shape, 1.0 - w.coverage, 1.0, 0.0, 1.0) * profile;
    if (density <= 0.0) return 0.0;

    if (detail > 0.0) {
        // Erosion, and it is STRONGER AT THE BOTTOM. The base of a cumulus is cut off flat and
        // hard at the condensation level while the top is still convecting into dry air and comes
        // apart into wisps — so the high-frequency detail is subtracted where the cloud is thin
        // and turned into billows where it is deep. The same erosion everywhere is a uniform fizz
        // that reads as noise laid over a shape rather than as the shape being made of something.
        float fine = worley_fbm(q * (1.0 / metres(130.0)), 2.2);
        float wispy = mix(1.0 - fine, fine, clamp(h * 4.0, 0.0, 1.0));
        // Scaled by how thin the cloud already is, so it carves EDGES and leaves cores alone.
        //
        // Unscaled it gutted them. A remap that lifts the floor to half the wispy value takes a
        // density of 0.3 down to 0.03 — a tenth of what was there — so every cloud was eroded to a
        // wisp before it was ever drawn, and what reached the screen was the handful of peaks that
        // had enough density to survive being cut by ninety per cent. Erosion is meant to be the
        // ragged rim on a solid thing, not a filter that only the strongest tenth passes.
        density = remap(density, wispy * 0.45 * (1.0 - density) * detail, 1.0, 0.0, 1.0);
    }

    // And more mass higher up. A cumulus is heaviest where it has risen furthest, which is what
    // makes its top read as solid and its skirts as thin.
    return clamp(density, 0.0, 1.0) * mix(0.55, 1.3, h) * 1.1;
}

float mid_deck_density(vec3 p, float height_m, float detail) {
    float h = clamp((height_m - kMidBase) / (kMidTop - kMidBase), 0.0, 1.0);
    vec3 q = p - wind_offset(height_m);
    Weather w = weather_at(q * 1.7);
    float coverage = clamp(w.coverage * 0.8 - 0.05, 0.0, 1.0);
    if (coverage <= 0.01) return 0.0;

    // Flatter and in rows: the horizontal scale is squashed across the wind, which is what puts
    // altocumulus into the bands and mackerel patterns a middle deck actually forms.
    vec3 base_p = q * vec3(1.0, 2.2, 0.55) * (1.0 / metres(520.0));
    float perlin = perlin_fbm(base_p, (detail > 0.0) ? 3 : 2);
    float cells = (detail > 0.0) ? worley_fbm(base_p * 2.0, 2.0) : worley(base_p * 2.0);
    float shape = remap(perlin, cells - 1.0, 1.0, 0.0, 1.0);
    float profile = remap(h, 0.0, 0.25, 0.0, 1.0) * remap(h, 0.45, 0.9, 1.0, 0.0);
    float density = remap(shape, 1.0 - coverage, 1.0, 0.0, 1.0) * profile;
    if (detail > 0.0 && density > 0.0) {
        float fine = worley_fbm(q * (1.0 / metres(90.0)), 2.0);
        density = remap(density, fine * 0.45 * detail, 1.0, 0.0, 1.0);
    }
    return clamp(density, 0.0, 1.0) * 0.45;
}

float high_deck_density(vec3 p, float height_m) {
    float h = clamp((height_m - kHighBase) / (kHighTop - kHighBase), 0.0, 1.0);
    vec3 q = p - wind_offset(height_m);
    // Ice, streaked hard along the wind into fibrous hooks. Six to one, which is what turns a
    // round blob into a cirrus rather than into a smeared cumulus.
    // Streaked along the wind, but nothing like as hard as the first attempt. At six to one a
    // cirrus at eight kilometres spans the entire sky in one direction and the picture reads as
    // horizontal smearing rather than as cloud — the streaks are longer than anything else in
    // frame, so the eye takes them for a rendering fault. Two and a half to one still reads as
    // fibrous and stays inside the picture.
    vec3 base_p = q * vec3(0.40, 3.0, 1.0) * (1.0 / metres(700.0));
    float shape = perlin_fbm(base_p, 4);
    float profile = remap(h, 0.0, 0.3, 0.0, 1.0) * remap(h, 0.5, 1.0, 1.0, 0.0);
    // And far less of it. Cirrus is a garnish on a sky, not a layer of it.
    float coverage = clamp(push.sky_cloud.x * 0.45, 0.0, 1.0);
    return clamp(remap(shape, 1.0 - coverage, 1.0, 0.0, 1.0) * profile, 0.0, 1.0) * 0.12;
}

float cloud_density(vec3 p, float height_m, float detail) {
    if (height_m < kLowBase || height_m > kHighTop) return 0.0;
    if (height_m <= kLowTop) return low_deck_density(p, height_m, detail);
    if (height_m >= kMidBase && height_m <= kMidTop) return mid_deck_density(p, height_m, detail);
    if (height_m >= kHighBase) return high_deck_density(p, height_m);
    return 0.0;   // the clear air between decks, which is most of the sky's depth
}

// --- lighting ---------------------------------------------------------------------------------

float phase_hg(float cos_angle, float g) {
    float gg = g * g;
    float d = 1.0 + gg - 2.0 * g * cos_angle;
    return (1.0 - gg) / (4.0 * kPi * max(d * sqrt(max(d, 1e-4)), 1e-4));
}

// Forward and backward together, because a droplet does both. A purely forward lobe gives a
// beautiful silver lining and makes every cloud not between the eye and the sun read as grey card.
float cloud_phase(float cos_angle) {
    return mix(phase_hg(cos_angle, 0.80), phase_hg(cos_angle, -0.25), 0.28);
}

// How much sun reaches a point inside the cloud.
//
// Marched with a step that GROWS, because the first hundred metres decide whether this point is
// just under the surface or deep inside — the difference between a bright rim and a dark core, and
// the thing that makes a cumulus read as an object rather than a patch of fog — while a kilometre
// further on only the rough total matters. Six taps this way reach further than twelve even ones.
//
// No detail octaves: see cloud_density. What matters here is how much is in the way, not what its
// edge looks like.
float light_transmittance(vec3 p, float height_m, vec3 to_sun) {
    const int kSteps = 4;
    float step_m = 60.0;
    float travelled = 0.0;
    float optical = 0.0;
    for (int i = 0; i < kSteps; ++i) {
        travelled += step_m;
        optical += cloud_density(p + to_sun * metres(travelled),
                                 height_m + to_sun.y * travelled, 0.0) * step_m;
        step_m *= 1.8;
    }
    return exp(-optical * kExtinction * kLightExtinction);
}

// What a cloud is worth along a ray.
//
// # The march, which is where the volume comes from
//
// Two step sizes and a state machine between them. Long strides through empty sky; the moment
// anything is found, back up one stride and creep. That is the whole difference between a cloud
// with an inside and a cloud with a silhouette: a uniform step big enough to cross forty
// kilometres of horizon puts three samples through a cumulus, and three samples can only describe
// where it starts and stops.
//
// The cost is not what it looks like. Empty sky is most of every ray and it is crossed in a dozen
// strides; only the pixels actually looking at cloud pay for the fine steps, which is exactly the
// distribution you want.
vec3 cloud_march(vec3 origin, vec3 dir, float height_origin_m, float max_distance_m,
                 out float transmittance) {
    transmittance = 1.0;
    vec3 scattered = vec3(0.0);

    // Where the ray is inside the slab that holds every deck. Rays that never enter it — most of
    // them, looking at the ground or along the horizon below the base — leave immediately.
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

    // Enough that the coarse schedule reaches well past anything worth marching, with room left
    // over for creeping. Striding alone covers a hundred kilometres in these steps, and the far
    // distance is forty-five — so no ray runs out of budget before it runs out of cloud, which is
    // the condition that stops the sampling rate varying from pixel to pixel.
    const int kMaxSteps = 72;
    const float kCoarse = 350.0;    // metres, striding through clear air
    const float kFine = 55.0;       // and creeping through cloud
    const int kEmptyBeforeStriding = 6;

    float cos_angle = dot(dir, trace.sun.xyz);
    vec3 sunlight = sun_radiance();
    vec3 ambient = sky_radiance(vec3(0.0, 1.0, 0.0)) * kSkyFill;
    // The air's own colour in this direction, for the aerial perspective. Once, outside the loop:
    // it does not change along the ray and it is not cheap.
    vec3 air_colour = sky_radiance(dir);

    float travelled = enter_m;
    // The furthest point that has been INTEGRATED. The strider may step back to look more closely
    // at an edge it overshot, but never back past this: anything before it has already contributed
    // to `scattered`, and walking it again adds the same cloud to the picture a second time.
    float integrated_to = enter_m;
    // Where the PREVIOUS sample was taken, which is where to step back TO. It is one stride behind
    // and it was clear air, so nothing between it and here has been integrated.
    float last_sample = enter_m;
    bool creeping = false;
    int empty_run = 0;

    for (int i = 0; i < kMaxSteps && travelled < leave_m; ++i) {
        if (transmittance < 0.01) break;

        // The step is a function of DISTANCE TRAVELLED and of nothing else.
        //
        // That single property is what both faults come down to, and each previous version broke
        // it in a different direction.
        //
        // A floor of span/budget made it depend on where the ray LEAVES the deck — and `leave_m`
        // jumps across the horizon, because a ray tilted a hair downward exits through the bottom
        // a few kilometres away while one tilted a hair upward exits through the top tens of
        // kilometres away. The step jumped with it, the two halves of the sky were integrated at
        // different resolutions, and the seam between them was a horizontal line across the sky.
        //
        // Growing by the ITERATION INDEX made it depend on how many steps had been spent, which
        // creeping consumes at a rate that varies per ray. Forty metres of fine step becomes four
        // hundred once forty iterations have gone, so a ray that met cloud early sampled the rest
        // of its length coarsely and its neighbour did not — which is the hair, exactly as before.
        //
        // Distance is the only quantity that is the same for every ray in the frame at the same
        // point in space. Near the eye the steps are short because detail is visible there; far
        // away they are long because it is not; and two neighbouring pixels always agree.
        //
        // The growth rate is a budget argument rather than taste. Striding has to reach the far
        // distance in as few steps as it can, because every step it does not spend is a step left
        // for CREEPING, and creeping is what resolves an interior. At this rate the strides cover
        // forty-five kilometres in about thirty-seven, leaving half the budget for cloud — without
        // which a ray looking up through a tall cloud exhausts its steps and the top is cut flat.
        float step_m = (creeping ? kFine : kCoarse) * (1.0 + travelled * (1.0 / 6000.0));

        last_sample = travelled;
        vec3 at = origin + dir * metres(travelled);
        // Curving away with distance. See kEarthRadius: this is what puts an end to the deck
        // instead of letting it pile up against the horizon line.
        float h = height_origin_m + dir.y * travelled -
                  travelled * travelled / (2.0 * kEarthRadius);

        // The cheap density first, and the expensive one only where there is something to erode.
        float coarse = cloud_density(at, h, 0.0);
        if (coarse <= 0.0) {
            if (creeping) {
                ++empty_run;
                if (empty_run >= kEmptyBeforeStriding) { creeping = false; empty_run = 0; }
            }
            travelled += step_m;
            continue;
        }

        if (!creeping) {
            // Found the edge with a long stride. Step back to where the stride began and creep
            // from there, or the whole first stride of the cloud is missed and its face is cut off
            // flat three hundred metres in.
            //
            // Never back past what has already been integrated. `step_m` grows with distance, so
            // `travelled - step_m` is not the previous sample's position — it can land further
            // back than that, inside cloud this march has already added, which then gets added
            // again on the way through.
            //
            // Back to the previous SAMPLE, not back by the current step. Those are different
            // numbers because the step grows with distance, and using the step meant landing
            // somewhere the march had never looked — sometimes inside cloud it had already
            // integrated, which drew that cloud twice, and sometimes not far enough back at all.
            //
            // Clamped by what has been integrated as well, which matters for the SECOND cloud
            // along a ray and after: without the clamp the strider could reverse into cloud it had
            // already added. With only the clamp and not the previous sample — which is what was
            // here — it could not reverse at all once anything had been integrated, so every cloud
            // after the first had its face sliced off flat wherever the stride happened to land.
            // At three hundred and fifty metres a stride that is a hard edge on a cloud, and it
            // was reported as clouds cut off at their borders.
            creeping = true;
            empty_run = 0;
            travelled = max(last_sample, integrated_to);
            continue;
        }

        // Detail FADED with distance rather than switched off at one.
        //
        // The erosion octaves are the expensive half of the density and they carve features a
        // hundred metres across. Twenty kilometres away that is well under a pixel and behind most
        // of the haze as well, so it is computed and then averaged out of existence. Faded to
        // nothing over the near few kilometres it costs nothing anybody can see and takes the
        // creeping steps — which is where the frame goes — down to a fraction of the price.
        //
        // Faded and not switched, because a switch at a fixed distance is a ring on the sky.
        float detail = clamp(1.0 - travelled * (1.0 / kDetailFadeMetres), 0.0, 1.0);
        float density = cloud_density(at, h, detail);
        travelled += step_m;

        // The empty run is counted from the DETAILED density and not the coarse one, and that
        // distinction is the second half of the same fault.
        //
        // The coarse density deliberately over-estimates: it skips the erosion, which only ever
        // removes. So everywhere the erosion has carved a hole, the coarse answer says cloud and
        // the detailed one says nothing — and counting the coarse one meant the march never
        // registered an empty step there. It crept at forty metres through air, accumulating
        // nothing, until the step budget was gone. Which is what made the cutoff above fire on
        // almost every ray that touched a cloud edge.
        if (density <= 0.0) {
            if (++empty_run >= kEmptyBeforeStriding) { creeping = false; empty_run = 0; }
            continue;
        }
        empty_run = 0;

        float sun_t = light_transmittance(at, h, trace.sun.xyz);

        // Multiple scattering as octaves: each one dimmer, broader and less directional, which is
        // how a photon that has bounced a dozen times behaves — it has forgotten which way it came
        // and has been attenuated less than a straight line would suggest, because it went round.
        vec3 light = vec3(0.0);
        float attenuation = 1.0, contribution = 1.0, directionality = 1.0;
        for (int octave = 0; octave < kScatterOctaves; ++octave) {
            float t = pow(sun_t, attenuation);
            float p = mix(1.0 / (4.0 * kPi), cloud_phase(cos_angle), directionality);
            light += sunlight * t * p * contribution;
            attenuation *= 0.5;
            contribution *= 0.5;
            directionality *= 0.5;
        }
        light = light * kScatterBoost + ambient;

        // Powder: the darkening on the sunlit side of an edge, where a point has little above it
        // but a great deal around it, so what reaches it has been scattered sideways out rather
        // than straight through. Without it a cumulus lit from behind the camera is flat paper.
        float powder = 1.0 - exp(-density * step_m * kExtinction * 2.0);

        float optical = density * step_m * kExtinction;
        float step_t = exp(-optical);
        // The analytic integral across the step. What is scattered towards the eye is the
        // single-scattering albedo times one minus the step's transmittance — the density and the
        // extinction cancel exactly, and writing the division without the matching multiplication
        // scales every step by one over the extinction, which is sixteen.
        // What this step scatters, then what the air between here and the eye does to it: the
        // further away, the more of it has been replaced by the sky's own light.
        vec3 here = light * kAlbedo * (1.0 - step_t) * mix(0.7, 1.0, powder);
        float air = exp(-travelled / kAirVisibility);
        here = mix(air_colour * (1.0 - step_t), here, air);

        scattered += transmittance * here;
        transmittance *= step_t;
        integrated_to = travelled;
    }
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
        optical += cloud_density(p + to_sun * metres(travelled),
                                 height_m + to_sun.y * travelled, 0.0) * step_m;
    }
    return mix(kShadowFloor, 1.0, exp(-optical * kExtinction));
}
