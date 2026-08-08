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

struct Medium {
    vec3 scatter;    // sigma_s, per voxel of path, at the reference height
    float extinct;   // sigma_t, per voxel of path, at the reference height
    float g;         // Henyey-Greenstein asymmetry: 0 isotropic, positive forward
    float height;    // e-folding height in voxels; zero or less means uniform density
    float base;      // the world height, in absolute voxels, the coefficients are quoted at
};

Medium world_medium() {
    vec4 fog = media_fog();
    vec4 shape = media_fog_shape();
    Medium m;
    m.extinct = max(fog.w, 0.0);
    // Scattering cannot exceed extinction: the difference is what the medium absorbs, and a
    // negative absorption is a medium that creates light. Clamped rather than trusted, because
    // the parameters are authored and an authored number is eventually wrong.
    m.scatter = clamp(fog.xyz, vec3(0.0), vec3(m.extinct));
    // Just short of one either way. At exactly one the phase function's denominator is zero in
    // the forward direction, which is an infinity in a pixel rather than a bright one.
    m.g = clamp(shape.x, -0.95, 0.95);
    m.height = shape.y;
    m.base = shape.z;
    return m;
}

// ---- density, and the optical depth of a straight line through it ------------------------
//
// Density falls off exponentially with height, so fog pools in a valley and thins above a roof.
// One e-folding scale and one reference height is the whole model, and it is the model every
// atmosphere is written with, because the alternative -- a density that is a function of the
// world -- is a second volume to stream.
float media_density(Medium med, float y) {
    if (med.height <= 0.0) return 1.0;
    // Clamped because the exponent is a world height over a scale height and both are authored.
    // Below the reference height this grows, and at forty it is already 2.4e17.
    return exp(clamp((med.base - y) / med.height, -40.0, 40.0));
}

// The optical depth of `length` voxels of medium, starting at world height `y` on a ray whose
// vertical component is `dy`.
//
// Closed form rather than summed over the steps below, because the density is an exponential in
// height and height is linear in t, so the integral is another exponential. It costs one call to
// exp and it removes the whole class of error where the same fog is a different thickness at
// four steps and at sixteen -- which matters here, because the steps below are drawn at random
// and a transmittance that depended on where they landed would be noise in the one quantity that
// has no business being noisy.
float media_depth(Medium med, float y, float dy, float length) {
    if (length <= 0.0) return 0.0;
    float rho = media_density(med, y);
    if (med.height <= 0.0) return med.extinct * rho * length;

    float k = dy / med.height;
    // Near-horizontal, or a scale height so large the ramp over the segment cannot be told from
    // a straight line. The closed form divides by k, so this is a correctness guard as well as a
    // cheaper path.
    if (abs(k) * length < 1.0e-3) return med.extinct * rho * length;
    return med.extinct * rho * (1.0 - exp(clamp(-k * length, -40.0, 40.0))) / k;
}

// And the inverse: how far along the ray a given optical depth is reached. This is what lets a
// sample be drawn from the transmittance itself rather than spread evenly along a ray that is
// mostly empty -- see the loop in apply_media.
float media_distance_at(Medium med, float y, float dy, float depth) {
    float rho = media_density(med, y);
    float a = med.extinct * rho;
    if (a <= 0.0) return 0.0;
    if (med.height <= 0.0) return depth / a;

    float k = dy / med.height;
    if (abs(k) * (depth / a) < 1.0e-3) return depth / a;
    return -log(max(1.0 - k * depth / a, 1.0e-30)) / k;
}

// Henyey-Greenstein, so the medium can be forward-scattering the way real haze is: looking into
// the sun through it is bright and looking away from it is not, which is most of what tells haze
// apart from a flat grey wash laid over the distance.
//
// Normalised over the sphere, so it is a redistribution of the light the medium scatters and
// never an addition to it.
float henyey_greenstein(float cos_theta, float g) {
    float gg = g * g;
    float d = max(1.0 + gg - 2.0 * g * cos_theta, 1.0e-4);
    return (1.0 - gg) / (4.0 * kPi * d * sqrt(d));
}

// ---- does the sun reach this parcel of air? ----------------------------------------------

// The fourth kind of entry in the face table, beside irradiance, shadow and radiance.
//
// Its normal is the sun's dominant axis rather than a surface's, because the face it belongs to
// is the side of the parcel the sun is on. That makes it one value per node per frame, the same
// from wherever it is read, which is the property that matters.
const uint kFaceMedia = 3u;

// Four voxels, and fixed -- never chosen from how far away the camera is.
//
// Fixed for the reason the shadow's level and the radiance bin's level are both fixed: a level
// taken from the pixel's footprint makes the *key* a function of where you are standing, so
// walking towards a beam re-keys every parcel in it at once. That fault has been shipped twice
// in this renderer, as wedges in the shadows and as a grid of cubes sliding over reflections.
//
// Four rather than one because a parcel of air is a volume and volumes are expensive: entries go
// as the cube of the level, where a surface's go as the square. Four voxels is an eighth of a
// metre in the test scenes, which is finer than the edge of a real beam is sharp, and the
// jittered lookup below softens the boundary between two of them into a ramp of that width.
const uint kMediaLevel = 2u;

// How many samples a parcel wants before the pixels passing through it stop testing for it.
// The same order as the sun's own entries want on a surface and for the same reason: this is a
// yes-or-no answer sampled over half a degree of sky, and a few dozen of them already resolve
// the edge of a beam finer than the eye can see.
const uint kMediaSamples = 32u;

// A coarser parcel to lean on while a fine one is still thin was written here and taken out
// again, and the reason is worth keeping so nobody adds it back on the same reasoning.
//
// The sun's entries on a surface have one, and the argument transfers on paper: down a long view
// a parcel is thinner than a pixel, gets one yes-or-no measurement, and ought to defer to the
// larger piece of air it belongs to. Measured on the beam scene it bought nothing -- the shaft
// read 41.8 against 42.5 at forty frames and 113.0 against 113.3 at four hundred, which is
// noise -- and it made the room around the beam slightly worse, 10.1 against 8.9, because a
// two-metre parent straddles the edge of a shaft and hands some of it to the dark air outside.
// It also costs a second claim and a second set of atomics on every measurement.
//
// The difference from the surface case is that a face is found by *one* ray and a parcel by
// four, so a parcel a pixel wide is still measured four times by that pixel alone, and its
// neighbours cross it too. It reaches an answer without help.

// How far the occlusion test may step before it gives up and calls the parcel lit.
//
// It steps a whole chunk at a time where the world holds nothing, a whole brick at a time through
// a chunk that is empty here, and a voxel at a time only inside a brick that holds something --
// so ninety-six steps crosses several chunks of open air and still resolves a one-voxel roof.
const uint kMediaShadowSteps = 96u;

// How many parcels a single ray samples. Four, not sixteen, because the samples are drawn from
// the transmittance itself: each one lands where the fog actually is rather than being spread
// evenly along a ray that is mostly empty, and this tracer averages a thousand frames of them
// over a stationary camera. Each one costs a probe into a quarter-gigabyte table, which is what
// the number is really buying.
const uint kMediaSteps = 4u;

// Three answers, not two. See media_sun_reaches for why the third one has to exist.
const int kMediaBlocked = 0;
const int kMediaClear = 1;
const int kMediaUnknown = -1;

// How far to the far side of the axis-aligned cell of edge `cell` containing `p`. The skip that
// makes the walk affordable: one of these crosses an empty brick, or a whole empty chunk, in a
// single step.
float media_exit(vec3 p, vec3 dir, float cell) {
    vec3 base = floor(p / cell) * cell;
    vec3 far = base + mix(vec3(0.0), vec3(cell), step(vec3(0.0), dir));
    // A component of dir at zero would divide by nothing; replaced by something small enough
    // that its boundary is never the nearest one.
    vec3 safe = mix(dir, vec3(1.0e-6), lessThan(abs(dir), vec3(1.0e-6)));
    vec3 hit = (far - p) / safe;
    // A nudge past the boundary, or a ray starting exactly on one never leaves the cell. A
    // thousandth of a voxel is far below anything the picture can resolve.
    return max(min(hit.x, min(hit.y, hit.z)), 0.0) + 1.0e-3;
}

// Does the sun reach this point? Level 0 only, and silent.
//
// No detail selection, no summary tiers, no streaming feedback, no materials, no normals: a
// parcel of air asks whether anything is in the way and nothing else, and this is the whole of
// what answering it takes. That is why it is not a fourth march.
//
// It has three answers rather than two, and that third one is not fastidiousness -- it is the
// difference between a beam and a room full of milk. A chunk with no record is either empty
// world or world that has not streamed in yet, and calling both of them clear means that during
// the second or two a scene takes to arrive, every walk in it comes back lit. Those answers go
// into the cache, and the cache outlives them: the accumulator is reset when geometry arrives,
// so the picture starts again clean, and the parcels do not. Measured on the beam scene, that
// left a dark sealed room reading 13% lit for hundreds of frames after it had finished
// streaming -- a uniform glow with no cause in the world at all.
//
// So the two cases are told apart, by the same occupancy grid march uses to decide whether a
// missing chunk is worth reporting: level 0 of it answers "does the world have anything here".
// Nothing there means carry on; something there that has not arrived means this walk has no
// answer, and a non-answer is not contributed. Zero and "not yet" are different.
int media_sun_reaches(vec3 from, vec3 towards) {
    float t = 0.0;
    for (uint step_index = 0u; step_index < kMediaShadowSteps; ++step_index) {
        vec3 p = from + towards * t;
        ivec3 voxel = ivec3(floor(p));
        ivec3 chunk = ivec3(floor_div(voxel.x, kChunkEdge), floor_div(voxel.y, kChunkEdge),
                            floor_div(voxel.z, kChunkEdge));
        // Left the resident box, which march treats as empty by definition -- so nothing further
        // along this walk can block anything. Without it the walk carries on asking the occupancy
        // grid about world the renderer does not have, and that grid answers "aliased, assume
        // occupied" for a cell describing somewhere else entirely, which would turn a clear sky
        // into a permanent non-answer.
        if (any(lessThan(chunk, push.bounds_min.xyz)) ||
            any(greaterThan(chunk, push.bounds_max.xyz))) return kMediaClear;

        uint record = find_record(push.camera_chunk.xyz + chunk);
        if (record == kNoRecord) {
            if (coarse_occupied(0, push.camera_chunk.xyz + chunk)) return kMediaUnknown;
            t += media_exit(p, towards, float(kChunkEdge));   // genuinely empty: cross it whole
            continue;
        }
        ivec3 local = voxel - chunk * kChunkEdge;
        ivec3 brick = local >> 3;
        if (!mask_bit(record, 0u, brick)) {
            t += media_exit(p, towards, 8.0);
            continue;
        }
        if (voxel_solid(brick_slot(record, brick), local & 7)) return kMediaBlocked;
        t += media_exit(p, towards, 1.0);
    }
    // Out of steps. Clear, because every step that got here crossed empty world -- an empty
    // brick, or a whole empty chunk -- so a walk that runs out has crossed a great deal of
    // nothing. It is the same answer march gives a shadow ray that reaches kMaxSteps.
    return kMediaClear;
}

// The sun's face of a parcel: which side of it the light comes in through. Constant over a
// frame, so every parcel in the world keys the same way and two rays crossing meet the same
// entry.
ivec3 media_face(vec3 towards) {
    vec3 a = abs(towards);
    if (a.x > a.y && a.x > a.z) return ivec3(towards.x > 0.0 ? 1 : -1, 0, 0);
    if (a.y > a.z) return ivec3(0, towards.y > 0.0 ? 1 : -1, 0);
    return ivec3(0, 0, towards.z > 0.0 ? 1 : -1);
}

// Is this point outside the world the renderer has? march clips every ray to the resident box
// and calls everything beyond it empty by definition, so a parcel out there has nothing that can
// stand between it and the sun.
bool media_outside_world(vec3 p) {
    ivec3 chunk = ivec3(floor_div(int(floor(p.x)), kChunkEdge),
                        floor_div(int(floor(p.y)), kChunkEdge),
                        floor_div(int(floor(p.z)), kChunkEdge));
    return any(lessThan(chunk, push.bounds_min.xyz)) ||
           any(greaterThan(chunk, push.bounds_max.xyz));
}

// What this parcel of air knows about the sun, and -- on the one sample per pixel per frame that
// is allowed to -- one more measurement towards knowing it.
//
// Nothing is invented. A parcel with no entry yet returns nought and scatters nothing, which is
// the same answer a surface with no face entry gives: waiting is honest, and an estimate averaged
// in from somewhere else teaches a volume that light is where it is not. The beam therefore
// arrives over the first few frames rather than being wrong immediately.
float media_visibility(vec3 p, ivec2 pixel, uint frame, bool may_measure, inout uint state) {
    // Answered outright, and never cached.
    //
    // A ray that hits nothing is integrated to the far plane, so most of its samples land in open
    // sky hundreds of metres out. Those are parcels no second ray will ever visit: each would
    // claim an entry, be measured once, and never be read again -- a table filling with
    // single-sample answers to a question that has no second asker.
    //
    // And the answer is already known. Outside the resident box the walk terminates on its first
    // step, so there is nothing to measure and nothing worth remembering. This is not an
    // assumption about the sky; it is the same statement march makes about the same region.
    if (media_outside_world(p)) return 1.0;

    // Jittered by up to half a node before it is quantised.
    //
    // A parcel is four voxels across and its answer is one number, so read straight the boundary
    // between two of them is a step. Offsetting the lookup at random inside the node turns that
    // step into a ramp one node wide once the frame average has done its work -- the same trick
    // the marcher's ordered dither plays on level boundaries, for the same reason. It costs one
    // lookup, where interpolating the eight surrounding entries would cost eight.
    float node = float(1u << kMediaLevel);
    vec3 offset = (vec3(rand(state), rand(state), rand(state)) - 0.5) * node;
    ivec3 voxel = ivec3(floor(p + offset)) + push.camera_chunk.xyz * kChunkEdge;

    ivec3 face = media_face(trace.sun.xyz);
    uint lo, hi;
    face_key_node(voxel >> int(kMediaLevel), face, kMediaLevel, kFaceMedia, lo, hi);
    uint slot = face_slot(lo, hi, may_measure, frame);

    uint have = (slot != 0xFFFFFFFFu) ? faces.items[slot].count : 0u;
    // A trickle carries on for ever, so a parcel is never frozen: build a wall across a beam and
    // the air in it notices without anything having to clear the table.
    if (may_measure && (have < kMediaSamples || rand(state) < 0.02)) {
        // A different point on the sun's disc per pixel, stratified across the pixels that share
        // a parcel exactly as the surface shadow is. That is what makes the edge of a beam a
        // penumbra rather than a hard boundary: many pixels crossing one parcel, each testing a
        // different part of the sun, and the entry holding the mean of them.
        // Salt 6, and the number is load bearing. 1 is the surface sun disc, 2 the diffuse
        // hemisphere bounce, 3 the lamp cone. Under the old hash a shared salt only partly
        // correlated two draws; stratified_pair is now a pure function of (pixel, frame, salt),
        // so sharing one makes them bit-identical -- a beam and the bounce off the floor under
        // it would have sampled the same point of the sun in lockstep for ever.
        int reaches = media_sun_reaches(p, sun_sample_at(stratified_pair(pixel, frame, 6u, state)));
        if (reaches != kMediaUnknown) {
            contribute_shadow(slot, float(reaches),
                              in_edited_region(voxel) ? kEditWindow : kFaceWindow);
        }
    }
    // Read back after contributing, so this pixel's own measurement is in the average it shades
    // with -- the same order the surface shadow reads in, and for the same reason. A parcel the
    // table refused, or one nobody has asked about yet, reads nought and scatters nothing.
    return read_shadow(slot);
}

// ---- structure in the fog ----------------------------------------------------------------
//
// Adapted from Derivative's volumetric fog, which shapes its density with several octaves of noise
// drifting on the wind instead of leaving it a smooth function of height. The difference is between
// haze -- an even wash that thickens with distance -- and WEATHER: banks of it lying in the low
// ground, thinner patches you can see through, an edge that moves.
//
// # Where it is applied, and why not where the original applies it
//
// The original multiplies its density by this and marches the result. That cannot be done here, and
// the reason is worth setting down because it is the good property of this file.
//
// The transmittance and the sampling above are CLOSED FORM. Density is an exponential in height and
// height is linear along a ray, so the optical depth integrates analytically and the scattering
// point can be drawn from the transmittance exactly. That is what makes four samples enough, and
// what stops the fog's thickness depending on where the samples happened to land. Multiplying a
// noise into the density destroys all of it: no closed form, no exact inverse, and the fog's
// opacity becomes an estimate with variance in it -- which is noise in the one quantity that must
// not be noisy, because it multiplies the entire scene behind it.
//
// So the noise shapes what each parcel SCATTERS rather than how much it extinguishes. The sampler
// stays exact, the fog behind stays smooth, and the structure appears where it can be seen -- in
// the light the fog sends to the eye, which is the whole of what fog looks like when it is lit.
// The extinction is then slightly smoother than the scattering implies. For fog thin enough to see
// structure in, the two are close, and it is the honest approximation of the pair.
float fog_structure(vec3 local_p) {
    vec3 world_m = world_position(local_p) / kVoxelsPerMetre;

    // Drifting, at a fraction of the cloud deck's speed. Fog sits in the boundary layer where the
    // ground drags on the wind, so it moves noticeably slower than anything above it.
    vec2 wind = cloud_drift() * push.sky_cloud.y * 0.12;
    vec3 at = (world_m - vec3(wind.x, 0.0, wind.y)) * (1.0 / 78.0);

    // Four octaves at four to one. Fog has no fine structure worth resolving -- it is diffusion
    // acting on a temperature field, and diffusion is a low-pass filter -- so the octaves that
    // matter are the ones several metres across and up.
    float noise = 0.0, weight = 0.5;
    for (int i = 0; i < 4; ++i, weight *= 0.5) {
        noise += weight * perlin_noise(at);
        at = kOctaveTurn * at * 4.0;
    }

    // Contrast, then normalised so the AVERAGE is one. Without that last step, turning structure on
    // would silently change how thick the fog is, and the setting for it would stop meaning what it
    // says. This redistributes the fog it was already given and never adds any.
    float shaped = smoothstep(0.34, 0.63, noise);
    return mix(0.30, 1.78, shaped) * (1.0 / 1.04);
}

// ---------------------------------------------------------------------------------------

vec3 apply_media(vec3 radiance, vec3 origin, vec3 dir, float distance) {
    Medium med = world_medium();
    // Clear air, which is what the renderer has always assumed and what almost every scene is.
    // One compare against a push constant, uniform across the whole dispatch, so the branch is
    // free and everything below it is never reached.
    if (med.extinct <= 0.0) return radiance;

    // A ray that hit nothing carries no distance -- main leaves it at zero -- and the air in
    // front of it runs to the far plane.
    float length = (distance > 0.0) ? distance : push.lens.y;

    float y = origin.y + float(push.camera_chunk.y * kChunkEdge);
    float depth = media_depth(med, y, dir.y, length);
    float transmittance = exp(-min(depth, 40.0));

    // What the surface behind the fog still delivers.
    vec3 result = radiance * transmittance;

    // ---- and what the fog itself sends towards the eye -----------------------------------
    //
    // The scattering point is drawn from the transmittance rather than picked at even spacing.
    //
    // The integral is of sigma_t * rho(t) * T(t) times what arrives there, and the first three
    // factors are exactly a probability density over t once they are divided by the total
    // (1 - T) -- so drawing from them and estimating the rest is the textbook change of
    // variable, and the estimator that comes out is albedo * phase * visibility * (1 - T) with
    // no density and no transmittance left in it at all. Every sample therefore lands inside the
    // fog instead of a hundred metres past the point where it stopped mattering, which is what
    // lets four of them be enough, and there is no integration range to choose and get wrong.
    float scattered = 1.0 - transmittance;
    if (scattered > 1.0e-4) {
        ivec2 pixel = min(ivec2(gl_GlobalInvocationID.xy), ivec2(push.resolution.xy) - 1);
        // Its own stream, salted off the one main is using so the two do not draw the same
        // numbers in step. Derived rather than passed in, so this needs nothing of main's.
        uint state = hash_u32((uint(pixel.x) * 1973u + uint(pixel.y) * 9277u +
                               trace.control.x * 26699u) ^ 0x4D454449u);
        state |= 1u;

        // The angle between the way the sunlight is travelling and the way the eye's ray is,
        // both as directions of propagation: trace.sun points *at* the sun, and the eye receives
        // along -dir, so the two negatives cancel. Forward scattering therefore brightens looking
        // into the sun, which is what haze does.
        float phase = henyey_greenstein(dot(dir, trace.sun.xyz), med.g);

        // Only one sample per pixel per frame may pay for an occlusion test. Four tests on one
        // pixel is not four times the cost of one, it is a frame that misses its budget; and a
        // parcel needs a few dozen measurements from whichever pixels happen to cross it, which
        // at a screen's worth of rays takes no time at all.
        uint measuring = min(uint(rand(state) * float(kMediaSteps)), kMediaSteps - 1u);

        // The cloud shadow, ONCE for the ray rather than once per sample.
        //
        // It is a six-step march through the deck and it was by far the most expensive thing added
        // here -- with four samples each paying for it the frame went from 10 ms to 29. What it
        // returns varies over KILOMETRES, because that is the size of the cloud casting it, while
        // the four samples of one ray are spread over a few hundred metres of fog. Asking it four
        // times returned four copies of one answer at four times the price.
        //
        // Taken at the median of the scattering distribution, which is where the fog's contribution
        // is actually centred.
        float median_t = media_distance_at(med, y, dir.y,
                                           -log(max(1.0 - 0.5 * scattered, 1.0e-30)));
        vec3 shade_at = world_position(origin + dir * min(median_t, length));
        float deck = cloud_shadow(shade_at, world_height_metres(shade_at));

        float gathered = 0.0;
        for (uint i = 0u; i < kMediaSteps; ++i) {
            // Stratified over the transmittance, so the four samples cover the fog between them
            // rather than landing where chance puts them.
            float u = (float(i) + rand(state)) / float(kMediaSteps);
            float target = -log(max(1.0 - u * scattered, 1.0e-30));
            float t = media_distance_at(med, y, dir.y, target);
            vec3 at = origin + dir * min(t, length);

            // Cloud shadow ON THE AIR, not only on the ground. A shaft of sun through fog is only a
            // shaft because the light around it was stopped by something; with the deck ignored
            // here, an overcast sky still put full sunbeams through the fog under it, and the beams
            // were the brightest thing in a scene whose ground had correctly gone dark.
            gathered += media_visibility(at, pixel, trace.control.z, i == measuring, state) *
                        deck * fog_structure(at);
        }

        // Divided by extinction rather than multiplied by a density: what survives the change of
        // variable is the single-scattering albedo, which is the fraction of what the medium
        // takes out of the beam that it puts back rather than absorbing.
        //
        // The sun is not dimmed on its way in, and that is a decision rather than an oversight.
        // A medium thick enough for it to matter is one where the light that reaches the eye has
        // bounced inside the fog several times, and single scattering is already the wrong model
        // there -- charging a second exponential would make thick fog dark, which is the exact
        // opposite of what thick fog looks like. Under-lighting it slightly is the honest error.
        //
        // The beam is sun_radiance() and not trace.sun_colour, which is the same thing at noon
        // and nothing like it at dusk. Every surface in the scene is lit through that function
        // now (see pt_sky.glsl), so fog lit by the raw constant would be a shaft of noon
        // hanging in a red room -- and worse, a sun a degree under the horizon would still put
        // a beam through the air after the ground it lights had gone dark.
        result += (med.scatter / med.extinct) * sun_radiance() * phase * scattered *
                  (gathered / float(kMediaSteps));
    }
    return result;
}
