#ifndef WS_FACE_TERMS_GLSL
#define WS_FACE_TERMS_GLSL
// What a face gives off, from the terms stored on it. One arithmetic, two readers.
//
// # Why this is a file rather than two pieces of code
//
// Until R9 exactly one thing read a face's light: `resolve.comp`, once per pixel. R9 adds a second
// reader inside the light pass itself — a gathering ray that lands on a surface has to know what
// that surface is giving off — and the two must not be able to disagree. If the composite and the
// bounce weighted the sun, the sky and the lamps differently, a room would be lit by one renderer
// and drawn by another, and the difference would read as taste rather than as a fault. That is the
// same argument `face_work_of` in node.glsl carries for the worklist and the shading pass, and it
// was learned there (D420): one function, called twice.
//
// It declares no bindings and reads no buffers on purpose, so it can be included by two shaders
// whose descriptor sets have nothing in common. Every input is passed in, by the caller that owns
// the layout it came out of.
//
// # What it does NOT include, and why
//
// **Emission.** A lamp's own face is bright, and the composite adds that after the albedo multiply
// because emission is not reflected light. A gathering ray must not: emitters are sampled directly
// by the lamp estimator (R3c, `pick_light`), so a bounce that also picked up their emission would
// count every lamp in the building twice — once as a light and once as a bright surface.

// How many words a face's light record is, and it lives HERE because two shaders index that record
// and neither owns it.
//
// It was declared twice — once in node.glsl beside the pass that writes the record, once in
// resolve.comp beside the pass that reads it — with a comment in the second saying the layout is
// documented in the first. Both said nine, which was true, until the record grew to twelve for the
// bounce and only the writer's copy was changed. The reader then indexed every face's record at the
// wrong stride, so each pixel's sun visibility, sky visibility, near field and lamps came out of some
// other face's record: the enclosed camera drew as salt and pepper over a black building, and it
// looked exactly like a bounce estimator with too few samples, which is what it was mistaken for.
//
// A constant that describes a layout must be declared where the layout is, once. See the identical
// argument in node.glsl above `face_work_of`, and the one in resolve.comp above the Params block.
// Twelve accumulated words and four FILTERED ones. The split matters more than the number:
//
//   0-11  what this face measured for itself. Sample counts, the near field and its two gradients,
//         the far field, the lamp sum and its version, and the bounce mean. Every ray this pass
//         casts lands in one of these, and nothing but this face's own rays ever writes them.
//   12    the filtered far field, as a float in [0, 1], with the filter's own effective sample
//         count in the low sixteen bits. That count is nought exactly when this face has no
//         filtered answer at all, which is what tells the composite to read the raw record.
//   13-15 the filtered bounce, as three floats of radiance.
//   16-18 the filtered LAMP irradiance, as three floats. Stored as the MEAN and not as the sum the
//         accumulator keeps in words 5-7, because a weighted blend of neighbours has no single
//         count to divide by afterwards. Word 12's count is its validity too: the first visit of a
//         face writes nought there and takes both its first far ray and its first lamp burst, so
//         the two are never separately absent in practice, and a scene with no emitters at all
//         gives every tap a lamp mean of nought, which is the answer.
//
// The filtered pair is R5a: a face's own estimate blended with its COPLANAR NEIGHBOURS' estimates,
// written by `face_denoise` in shade_faces.comp. It is a separate four words and not a rewrite of
// the twelve above for one reason, and it is the reason a-trous is usually done in ping-pong
// buffers: a filter that reads what it writes is a filter applied again on every visit, so it would
// blur without bound until a wall was one colour. Reading the raw words and writing these makes that
// unrepresentable rather than merely avoided.
//
// **Only the composite reads them.** A gathering ray reads the RAW bounce, deliberately: the bounce
// chain is already a progressive radiosity solve over many frames, and feeding a filtered value back
// into it is the same unbounded blur arriving through the light transport instead of through the
// buffer. The filter is a display of an estimate, not part of the estimator.
//   19    R4b: which WAY the lamps are, as one octahedral direction in sixteen bits. See
//         kFaceLampDir.
//   20    R5b: the SUN VISIBILITY OF THE COARSE FACE STANDING OVER THIS ONE, in sixteen bits with
//         a known bit above them, or nought while this face has enough of its own rays not to
//         need it. It is the only word here that is a measurement of a DIFFERENT face, which is
//         why it is the last one and why it is not called a filtered anything: words 12-18 are
//         this face's own answer smoothed, and this is somebody else's answer standing by. See
//         kFaceSunStandIn.
const uint kFaceLightWords = 21u;
const uint kFaceFilteredSky = 12u;      // the filtered far field
const uint kFaceFilteredBounce = 13u;   // ...and the three words of filtered bounce after it
const uint kFaceFilteredLamp = 16u;     // ...and the three of filtered lamp irradiance after that

// ---- which way the lamps are, which is what a polished surface needs and did not have ---------
//
// **The sconces were not in the lobe at all.** A face's lamp term is an IRRADIANCE — radiance times
// the cosine, integrated over every fitting the estimator drew — and an integral has no direction
// left in it. So the diffuse half of a metal knew the lamps were there and the specular half did
// not, and a gilt patera two feet from a lit sconce showed no highlight of it whatever.
//
// That is most of what "it does not reflect like polished gold" is. A polished metal in a dim room
// is DARK, with a few small brilliant images of whatever is actually emitting — and those images
// are what the eye reads as polish. The wide environment reflection this stage spent three
// sub-steps on is the other, quieter half.
//
// One word, and it costs nothing to fill: `lamp_cast` already has the direction it aimed down, and
// this is a running mean of it weighted by what each sample delivered. The luminance to weight by
// is the lamp sum the record already holds, so nothing new is accumulated either.
//
// **And it is SHARP where the bins are not.** The highlight is evaluated per pixel against the
// material's own roughness, exactly as the sun's is, so gilt at 3.6 degrees gets a 3.6 degree
// highlight rather than the 13.5 the bins quantise everything else to. That is the whole reason
// this reaches "polished" where four times the bins did not (D599): a point source needs a
// direction, not a resolution.
const uint kFaceLampDir = 19u;

// A unit vector in sixteen bits, octahedral. About a degree, which is far inside the narrowest
// highlight this renderer can draw.
vec2 face_oct_wrap(vec2 v) {
    return (vec2(1.0) - abs(v.yx)) *
           vec2(v.x >= 0.0 ? 1.0 : -1.0, v.y >= 0.0 ? 1.0 : -1.0);
}
uint face_oct_pack(vec3 n) {
    n /= max(abs(n.x) + abs(n.y) + abs(n.z), 1e-6);
    const vec2 e = (n.z >= 0.0 ? n.xy : face_oct_wrap(n.xy)) * 0.5 + 0.5;
    const uvec2 q = uvec2(clamp(e * 255.0 + 0.5, vec2(0.0), vec2(255.0)));
    // Never nought, because nought is the sentinel for "no direction recorded yet" and a legal
    // direction must not be able to spell it. Trap 7, in sixteen bits.
    return (q.x | (q.y << 8)) | 0x10000u;
}
bool face_oct_known(uint packed) { return (packed & 0x10000u) != 0u; }
vec3 face_oct_unpack(uint packed) {
    vec2 f = vec2(float(packed & 0xFFu), float((packed >> 8) & 0xFFu)) * (1.0 / 255.0) * 2.0 - 1.0;
    vec3 n = vec3(f.x, f.y, 1.0 - abs(f.x) - abs(f.y));
    const float t = max(-n.z, 0.0);
    n.x += n.x >= 0.0 ? -t : t;
    n.y += n.y >= 0.0 ? -t : t;
    return normalize(n);
}

// ---- R5b: how far a face's own sun ratio is worth believing ------------------------------------
//
// **`kFaceSettled` is ONE, so the composite reads a face that has cast a single shadow ray, and one
// ray is binary.** That is the right rule and it is deliberate — node.glsl's comment on the constant
// works through the alternative and refutes it, because what four samples show INSTEAD is full sun,
// and indoors full sun is not a cautious answer but the worst answer available. What it leaves is
// variance: in a room whose true answer is 0.05, nineteen faces in twenty read black on their first
// ray and the twentieth reads white, and neighbours that have each cast one ray disagree completely.
// Measured at the close camera, **135,071 faces of 589,870 — 23% — have fewer than four samples**,
// and that population is what a player sees as speckle on any surface they have just turned towards,
// walked up to, or revealed by carving. The grain is faces still measuring, not converged faces
// disagreeing.
//
// R9d already built what should stand in for a face with nothing: `visibility.comp` hands the
// composite the coarse face `kFaceAncestorStep` levels up, which five hundred and twelve faces share
// and which is therefore smooth. But it is a HARD SWITCH — no samples at all, or your own
// one-sample answer believed outright, with nothing in between. This is the ramp.
//
// So the reader believes a face's own ratio in proportion to how well measured it is and takes the
// rest from the stand-in, exactly as `bounce_confidence` in resolve.comp already does for the bounce
// and for the whole of R4c's energy split. That constant's own reasoning applies here word for word:
// confidence rises SMOOTHLY, because a threshold puts a discontinuity at the sample count on every
// face that straddles it, and D387 is the standing measurement of what a hard per-face decision does
// — neighbours either side of it disagree by more than the noise ever did.
//
// **It costs no rays at all**, which is the constraint D394 imposes on anything in this class: three
// ways of metering the ambient burst were built and all three made the transient worse, because what
// this pass spends on an unconverged face is mostly the face and not the ray. Nothing here casts,
// paces or reschedules anything. It is a different reading of rays that were already taken.
//
// **And it is not `--sun-seed`** (D660), which gives a newly claimed face three samples of the same
// ancestor's ratio ONCE, at claim time, inside its own counters. The two land on different parts of
// one axis: a seed moves where the estimator STARTS, and this decides how far the estimator is
// BELIEVED while it is still short. **They cannot double-count, and the arithmetic says so rather
// than the intent**: a seeded face arrives holding the ancestor's own ratio at three samples, so
// what this blends is three quarters of that ratio with one quarter of the same ratio, which is the
// ratio. The seed simply carries a face over the first two rungs of the ramp. Both on is a face that
// starts at its ancestor's answer and is fully its own after one ray of its own instead of four.
//
// # Four, and it is kFaceEager's number rather than a figure with room to be tuned
//
// It must match `kFaceEager` in node.glsl (and `kFaceEager` in src/world/face_store.hpp, which
// mirrors it for the audit). It cannot be SPELLED as that constant here, because the include runs
// the other way — node.glsl includes this file — so what holds the three copies together is a test
// rather than a comment: `tests/test_sun_confidence.cpp` reads this line out of this file and fails
// if it and the store's have drifted apart. Four rather than a number chosen against the variance,
// for four reasons, and the last one is the gate:
//
//   - it is where this renderer ALREADY stops treating a face as new. Below `kFaceEager` a face is
//     exempt from the shading stride and casts a ray every frame; at or above it drops to one frame
//     in `face_stride`. So the ramp is exactly co-extensive with the every-frame phase, and the
//     stand-in it blends towards is refreshed on every single frame it is used;
//   - it is the count the audit already calls settled (`sun on the card: N of M settled` counts
//     `face_samples < kFaceEager`), so the population this touches is precisely the 23% the
//     measurement above names, and neither number has to be inferred from the other;
//   - an edit reseeds a face to `kFaceEditSeed` = 8 samples at the ratio it held (D373). Any figure
//     above eight would blend every face inside an edit box towards a stand-in the same edit has
//     just reseeded — a prior taken from a record that is no better informed than the face reading
//     it, which is `kSunSeedMin`'s whole argument arriving through a different door;
//   - **a settled shadow edge is untouched, by construction rather than by tuning.** The sun is
//     deliberately not filtered by R5a's a-trous pass because a shadow edge is a real
//     high-frequency feature, and this must not become that filter by another route. A face in a
//     penumbra reaches four samples within four frames, because a face under `kFaceEager` casts
//     every frame; from there confidence is exactly one and `face_sun_believed` is the identity, to
//     the bit. What a longer ramp would buy is smoothness on faces that are no longer new, and what
//     it would cost is the edge.
const uint kFaceSunStandIn = 20u;    // where the stand-in's ratio lives; see the record above
const uint kFaceSunBelieve = 4u;     // must match kFaceEager in node.glsl

// The stand-in's ratio, packed. Sixteen bits of a quantity whose own estimator tops out at
// `kFaceWindow` = 256 samples, so this is sixty times finer than the number it is storing can ever
// be, and the top bit says it was written at all.
//
// A known bit rather than "nought means none", because nought is a legal sun visibility — it is what
// every face in a sealed room reads — and trap 7 is that "nothing here" and "I could not answer"
// must never be the same value. The word is zeroed with the rest of the record when a slot is handed
// to a new face, so the bit is what tells a reader the difference between a stand-in of nought and a
// slot whose light words have not been visited yet.
const uint kFaceSunKnown = 1u << 31;
uint face_sun_pack(float visible) {
    return uint(clamp(visible, 0.0, 1.0) * 65535.0 + 0.5) | kFaceSunKnown;
}
float face_sun_unpack(uint word) { return float(word & 0xFFFFu) * (1.0 / 65535.0); }

// How far a face of `samples` shadow rays is its own answer, in [0, 1].
float face_sun_confidence(uint samples) {
    return clamp(float(samples) / float(kFaceSunBelieve), 0.0, 1.0);
}

// ...and the read itself. **One arithmetic, two readers**, which is the sentence at the top of this
// file: the composite draws a surface with this and a gathering ray landing on the same surface
// integrates it with this, and if the two weighted an under-measured face differently the room would
// be lit by one renderer and drawn by another (D420).
//
// `stand_in` is word `kFaceSunStandIn` of this face's light record, straight out of the buffer. It is
// nought — no known bit — in four cases and all four mean the same thing to a reader: this face has
// enough of its own rays, nothing above it had a measurement worth taking, the face IS a stand-in, or
// the control arm is on. In every one of them what comes back is the face's own ratio, and it comes
// back BIT-EXACTLY: `mix(x, own, 1.0)` is `x * 0.0 + own * 1.0`, which is `own` for any finite x. So
// `--no-sun-confidence` is not a near-enough control arm, it is the same number.
float face_sun_believed(float own, uint samples, uint stand_in) {
    if ((stand_in & kFaceSunKnown) == 0u) return own;
    return mix(face_sun_unpack(stand_in), own, face_sun_confidence(samples));
}

// The two occlusion terms a face carries, combined the one way that does not darken a crease twice.
//
// `open_sky` is the far field: how much of the dome this face can see at all, out of the unbounded
// rays that reached it. `unoccluded` is one minus the near field: how much of what is left is shut
// out by whatever stands within a metre. Multiplied, never averaged, and each appearing exactly
// once — applying either twice double-darkens every crease in the building, and that failure looks
// like a taste problem rather than like a bug.
float face_sky_visibility(float open_sky, float unoccluded) {
    return open_sky * unoccluded;
}

// The irradiance arriving at a face from the two DIRECT sources, in radiance units.
//
// `lambert` is the cosine at the sun already multiplied by the measured sun visibility, because a
// face that cannot see the sun receives none of it however square it faces.
vec3 face_direct_irradiance(vec3 sun_radiance_in, float lambert, vec3 sky_radiance_in,
                            float sky_visibility, vec3 lamp_irradiance) {
    return sun_radiance_in * lambert + sky_radiance_in * sky_visibility + lamp_irradiance;
}

// And what the face reflects, as RADIANCE, which is what a gathering ray landing on it reads and
// what the composite writes into the frame.
//
// Over PI, and leaving it out is what blew the building white while the sky stayed dark: a
// Lambertian surface lit by an irradiance E reflects albedo * E / PI, and the sky it is being
// compared against is already radiance and needs no such conversion.
//
// `bounce` is a RADIANCE MEAN and is therefore added after the divide rather than inside it. It is
// the average of what the unbounded rays found when they landed on a surface instead of reaching
// sky, so the irradiance it stands for is PI times it, and the two PIs cancel. Writing it inside
// the bracket with a PI stuck on would be the same number and would invite somebody to remove the PI.
vec3 face_outgoing_radiance(vec3 albedo, vec3 direct_irradiance, vec3 bounce, float kPi_in) {
    return albedo * (direct_irradiance * (1.0 / kPi_in) + bounce);
}

// ---- R4c: the lobe, which is the other half of what leaves a face ------------------------------
//
// Everything above this line treats every surface in the world as Lambertian: one albedo, one
// outgoing radiance, the same in every direction. That is why the bronze doors, the gilt paterae,
// the lead flashing and the copper dome read as coloured chalk — the two bytes that say they are
// metal reached the face in R4a and nothing did anything with them.
//
// A surface reflects in two ways and the split between them is `metalness`, read as a quantity and
// never as an identity (§8 R4: *nothing in this stage may branch on a material's identity*). What
// is diffuse goes through the arithmetic above, unchanged. What is specular is a **lobe**: a
// distribution over outgoing direction, narrow where the surface is smooth and wide where it is
// rough, and it is what makes a reflection move across a surface as you walk past it.
//
// # The two halves of a lobe, and only one of them needs storing
//
// - **the sun**, which is one direction and is already measured on the face: `face_lit_of` is the
//   fraction of the disc this face can see. Evaluating a lobe towards it is arithmetic, per pixel,
//   exactly as `lambert` above already is — no ray, no storage, nothing that scales with the
//   window except a multiply. That is the highlight that slides across a door as you move;
// - **everything else** — the sky, the portico, the room — which is a whole hemisphere and cannot
//   be arithmetic. That is the BINS, measured by the face's own gathering rays and read by the
//   composite along the one direction the eye is looking down. See `face_lobe_*` below.
//
// # What a face with no bins does, and why it is not a special case
//
// The bins are a payload allocated only where they are read (§4 of the plan: *a matte stone wall
// allocates no payload at all*), so most faces have none. A face with no bins is not a face with no
// lobe: its lobe is the whole hemisphere averaged into one direction, which is exactly the number
// the bounce already holds. So `bins = 0` is the continuous end of the same law rather than a
// branch — a fully rough surface's lobe IS its hemispherical mean, and storing sixteen copies of
// one number would buy nothing. That is what keeps this free of a roughness threshold: what the
// bin count does is decide how much DIRECTION survives, and nought is a legal answer.

const float kFacePi = 3.14159265358979;

// Roughness as the GGX width. Squared, which is the convention `pt_material.glsl` used and the one
// the clips were authored against, so `rough=64` means to a clip author what it always meant.
//
// Floored, because a width of nought is a division by nought in the distribution below and a clip
// may legitimately write `rough=0`. The floor is well under the angular size of one bin, so it can
// never be what decides how sharp a stored reflection is — that is the bin count's job, and a floor
// that could bind would be a roughness threshold wearing a safety guard's coat.
const float kFaceAlphaMin = 0.004;
float face_alpha_of(uint rough_byte) {
    const float r = float(rough_byte) * (1.0 / 255.0);
    return max(r * r, kFaceAlphaMin);
}

// What fraction of this surface reflects diffusely. A metal has no diffuse lobe at all; a
// dielectric has very nearly all of it.
//
// **Both readers of a face's light apply this**, which is the whole reason it lives in this file:
// if the composite darkened a bronze door's diffuse and the gathering ray went on reading it as
// full Lambertian, the room would be lit by one renderer and drawn by another — which is the
// sentence at the top of this file, and it was learned the expensive way (D420).
float face_diffuse_share(uint metal_byte) {
    return 1.0 - float(metal_byte) * (1.0 / 255.0);
}

// The specular colour at normal incidence. A dielectric reflects about four per cent of what falls
// on it, whatever colour it is; a metal reflects its own colour and nothing else. Mixed rather than
// chosen, so a clip writing `metal=110` gets the mixture it asked for.
const float kFaceDielectricF0 = 0.04;
vec3 face_f0_of(vec3 albedo, uint metal_byte) {
    return mix(vec3(kFaceDielectricF0), albedo, float(metal_byte) * (1.0 / 255.0));
}

// Schlick's Fresnel: how much more a surface reflects at a glancing angle than head on. It is what
// makes a polished floor a mirror at the far end of a hall and matte under your feet, and it costs
// four multiplies.
vec3 face_fresnel(vec3 f0, float cos_theta) {
    const float m = clamp(1.0 - cos_theta, 0.0, 1.0);
    const float m2 = m * m;
    return f0 + (1.0 - f0) * (m2 * m2 * m);
}

// The GGX microfacet distribution, which is the shape of the lobe.
float face_ggx_d(float n_dot_h, float alpha) {
    const float a2 = alpha * alpha;
    const float d = n_dot_h * n_dot_h * (a2 - 1.0) + 1.0;
    return a2 / max(kFacePi * d * d, 1e-9);
}

// Smith's height-correlated visibility, which is G / (4 (n.v)(n.l)) with the divide folded in.
float face_ggx_vis(float n_dot_v, float n_dot_l, float alpha) {
    const float a2 = alpha * alpha;
    const float lv = n_dot_l * sqrt(n_dot_v * n_dot_v * (1.0 - a2) + a2);
    const float ll = n_dot_v * sqrt(n_dot_l * n_dot_l * (1.0 - a2) + a2);
    return 0.5 / max(lv + ll, 1e-6);
}

// ---- the outgoing bins: which directions a face stores, and where each one points --------------
//
// A face's hemisphere, cut into `kLobeSide` by `kLobeSide` patches of very nearly equal solid angle
// by the hemi-octahedral map. The centre of the square is the face's own normal and its four
// corners are the four grazing directions, so the parameterisation has no pole and no seam
// anywhere a reflection is worth looking at.
//
// **Sixteen is a constant here and it is R4b's job to make it a function.** D186 says the bin count
// comes from roughness and from how many pixels the face covered, and neither is read here: what
// this landing fixes is that a face has a direction-dependent answer at all.
//
// **Sixty-four, and it was sixteen for one landing** (D591). Sixteen is a cone of 20.4 degrees
// half-angle and no image survives that; sixty-four is 10.2, which is finer than copper at 24
// degrees and lead at 25.4, about right for bronze at 10.7, and still four times too coarse for
// gilt at 3.6. **What decided the number is not memory, it is rays** — D592 measured 256 bins as
// two samples apiece out of a budget of five hundred, which is noise rather than resolution. It is
// affordable at sixty-four only because a face that holds a lobe now casts a ray OF ITS OWN aimed
// into the cone each bin gathers from, which is R4b's half of this stage.
const uint kLobeSide = 6u;
const uint kLobeBins = kLobeSide * kLobeSide;

// How many samples a bin wants before the face stops casting for it, and how many rays a visit it
// spends getting there.
//
// **The burst is SMALL, and that is the opposite of what D394 concluded for the ambient term.**
//
// D394 measured every attempt to meter the ambient burst as making the transient worse, because
// what that pass spends on an unconverged face is mostly the face and not the ray: ninety thousand
// faces at thirty-two rays and two hundred and forty thousand at two cost about the same. That
// argument does not carry here and the difference is the RAY, not the face. An ambient near ray is
// bounded at a metre; a lobe ray is unbounded, which is the expensive kind, and at thirty-two a
// visit it read **11.9 ms flying against 7.8** — 270,853 rays a frame, seventy-one per cent of
// every gathering ray in the picture.
//
// The other half of why it does not carry: D394's population is bounded and converges, so letting
// it measure hard empties it. A flying camera refreshes the lobe population continuously, so there
// is no state to get out of and the per-frame rate is the whole cost.
//
// What eight costs is that a reflection fades in over about nine seconds rather than two.
const uint kLobeSamples = 24u;             // a bin, so 864 rays over a face's life
const uint kLobeBurst = 8u;                // a visit, so 108 visits — about nine seconds — to
                                           // converge at the sun's stride
const uint kLobeConverged = kLobeBins * kLobeSamples;

// ...and how many a face with MORE bins takes, which is the same burst per bin rather than the same
// burst per face.
//
// **This is the fourth measurement of one constraint and the first change to the thing it names.**
// D592 raised the bin count to 256 and got two samples a bin; D594 went back to thirty-six and put a
// proper ray in; D599 built the sharp class on a roughness gate and reverted it because twenty-four
// samples of a radiance is twenty-four samples however many bins there are; R4b's coverage rule
// reached the same wall from the third side -- the sky's own colour landed in individual bins of the
// chrome and the picture was noise. Every one of those is the SAMPLE budget and not the rule.
//
// A face that holds four times the bins is asked for four times the rays a visit, so it converges in
// the same hundred-odd visits the cheap class does instead of in four times as long. What that costs
// is charged exactly where R4b says it is earned: the faces with many bins are the faces covering
// many pixels, so they are few, they are near, and they are the ones being looked at. A scene whose
// materials never ask for the sharp class pays nothing at all, because the multiplier is one.
uint face_lobe_burst_of(uint bins) { return max((kLobeBurst * bins) / kLobeBins, kLobeBurst); }

// A direction in the face's own frame (z along the normal, z >= 0) to a point on the unit square.
vec2 face_lobe_encode(vec3 d) {
    d /= max(abs(d.x) + abs(d.y) + abs(d.z), 1e-6);
    // The 45-degree rotation is what turns the octahedron's upper half into a full square rather
    // than into the diamond inscribed in one, which is the whole of why this map wastes no bins.
    return vec2(d.x + d.y, d.x - d.y) * 0.5 + 0.5;
}

// ...and back, which is what a bin's centre direction is.
vec3 face_lobe_decode(vec2 at) {
    const vec2 p = at * 2.0 - 1.0;
    vec3 d = vec3((p.x + p.y) * 0.5, (p.x - p.y) * 0.5, 0.0);
    d.z = 1.0 - abs(d.x) - abs(d.y);
    return normalize(d);
}

// The two axes across a face, which is the frame every bin is indexed in.
//
// **One function, and it is the reason there is one at all**: the pass that writes a bin and the
// pass that reads one derive this frame independently, and a frame that disagrees by a swap or a
// sign is a reflection that is mirrored or upside down and looks very nearly right — the hardest
// kind of wrong to see in a picture. It is `face_point`'s discipline for positions, applied to
// directions.
//
// The same expression `shade_faces.comp` has always used for its ambient rays, moved here rather
// than copied: `g_sky_side` and `g_sky_other` are this.
void face_lobe_frame(vec3 normal, out vec3 side, out vec3 other) {
    side = normalize(abs(normal.y) < 0.9 ? cross(normal, vec3(0.0, 1.0, 0.0))
                                         : cross(normal, vec3(1.0, 0.0, 0.0)));
    other = cross(normal, side);
}

// A world direction into the face's frame and out again. `side` and `other` come from
// `face_lobe_frame` above, handed in by the caller because the writer has them in registers
// already.
vec2 face_lobe_at(vec3 dir, vec3 normal, vec3 side, vec3 other) {
    return face_lobe_encode(vec3(dot(dir, side), dot(dir, other), max(dot(dir, normal), 0.0)));
}
vec3 face_lobe_direction(vec2 at, vec3 normal, vec3 side, vec3 other) {
    const vec3 local = face_lobe_decode(at);
    return normalize(side * local.x + other * local.y + normal * local.z);
}

// Where the centre of bin `index` points, on the square.
vec2 face_lobe_centre(uint index) {
    return (vec2(float(index % kLobeSide), float(index / kLobeSide)) + 0.5) *
           (1.0 / float(kLobeSide));
}

// The width the bins themselves impose, as a GGX alpha.
//
// A patch of 2*PI/kLobeBins steradians is a cone of half-angle acos(1 - 1/kLobeBins), and a GGX
// lobe's half-angle is very nearly its alpha for small alpha. So a lobe narrower than this cannot
// be represented however sharp the material is, and pretending otherwise is what would make the
// estimate noise rather than blur: every sample would land in the tail of every bin's kernel and
// the bins would be decided by the handful of rays that happened to fall near a centre.
//
// Widening the KERNEL to the bin instead spends the same rays on an answer that is smooth and one
// bin blurry, which is the honest reading of what sixteen bins can say. It is the reason this term
// has no firefly problem while the bounce beside it needed `kBounceBelieve`.
float face_lobe_bin_alpha() {
    return acos(clamp(1.0 - 1.0 / float(kLobeBins), -1.0, 1.0));
}

// ---- where a face's bins live, which is a cache and not an allocator ---------------------------
//
// The pool is laid out in `shaders/node.glsl` beside the buffer it is; what is here is the
// arithmetic, because the pass that writes a bin and the pass that reads one are different shaders
// and a second copy of an index is a second chance to be wrong about a stride (D332 and the whole
// reason kFaceLightWords is in this file).
//
// Sixty-five thousand five hundred and thirty-six blocks, eight ways, and **both numbers were
// measured rather than chosen**.
//
// The first sizing was 32,768 blocks four ways, against the census's 22,158 metal faces at the
// close camera. It was wrong for a reason the census could not have shown: lowering the worth floor
// to 0.038 admits the glass and the water, which is most of the point, and that takes the
// population asking to about **44,700**. The audit line said so in one run — 47% of askers turned
// away and 271 blocks changing hands a frame — and the consequence is not merely that some faces go
// without. **A block that keeps changing hands has its sample count zeroed with it**, so its holder
// bursts again, and a settled camera sat at 449 faces still bursting and a faces pass 2.1 ms over
// where it should have been. A pool that thrashes costs more than a pool that is simply full, and
// only the third counter on that line can tell the two apart.
//
// Eight ways rather than four for the other half of the same measurement: a four-way set with
// two-thirds occupancy turns away a face whose set happens to be full while the pool has room, and
// doubling the ways costs one more cache line on a probe the composite runs per metal pixel.
const uint kLobeBlocks = 131072u;
const uint kLobeWays = 8u;   // how many blocks of the pool one face may try
const uint kLobeBlockWords = kLobeBins * 2u;
const uint kNoLobe = 0xFFFFFFFFu;

// ---- R4b: a second size, for the faces whose material is sharper than thirty-six bins ----------
//
// **D186 says the bin count comes from roughness AND from how many pixels the face covered, and
// building it turned up that those two do not do what the plan expects of them here.** The
// distinction is worth writing down, because it is a fact about a VOXEL renderer that a triangle
// renderer would not meet:
//
// - **roughness decides how sharp the answer needs to be, and it is the whole of that.** A bin is a
//   cone; a reflection cannot be sharper than the cone it is averaged over, whatever else is true.
//   Gilt is a 3.6 degree lobe and thirty-six bins are 13.5, so gilt is drawn four times blurrier
//   than it is and reads as a wash. That is the reported *"way too subtle"*, and it is arithmetic;
// - **coverage decides nothing about sharpness at all**, which is the surprise. A face here is one
//   voxel. Standing two metres from the bronze door a face covers about eleven pixels and those
//   eleven pixels look at it over **0.86 degrees** — far inside one bin however many bins there
//   are. Neighbouring faces do not help either: they are 3 cm apart and see the same room 0.86
//   degrees apart. The reflected image is blurred to the BIN, and the number of faces or pixels
//   looking at it does not enter.
//
// So coverage is not the demand, it is the AFFORDABILITY: it says which faces are worth spending
// the expensive class on when there is not room for every face that wants one. That is the same job
// `kLobeWorthFloor` already does one level up, and it is why this is a priority and not a rule.
//
// A big block is **four consecutive small ones**, taken from the same set. Twelve by twelve is 144
// bins, 6.8 degrees, and 288 words — exactly four blocks of 72 — so it needs no second pool, no
// second buffer and no host change. The bins and the weights each become one run of 144 rather than
// two runs of 36, which is why the accessors below take the count.
const uint kLobeBigSide = 12u;
const uint kLobeBigBins = kLobeBigSide * kLobeBigSide;
const uint kLobeBigWays = 4u;   // how many consecutive blocks one big block is

// **How many pixels a face covers is NOT what decides the class, and this is where D186's second
// half was measured and not carried.**
//
// It was built: a face is `size` voxels across at `distance` from the eye and a pixel subtends
// `pixel_angle`, so the exact coverage is three lines of arithmetic and needs no counter at all.
// Gating the expensive class on it at sixteen pixels **speckled the great bronze door**. Coverage
// varies smoothly across one flat surface, so the class flipped face to face along the middle of it
// -- and a lobe cut into 144 directions and one cut into 36 share no bin for the cross-face blend
// to pair, so exactly the faces at the boundary got no blending and stood out as noise. D387's
// standing measurement is that **a hard per-face decision is a new discontinuity per face**, and
// this is that arriving in a size class.
//
// The deeper reason it buys nothing is in the note above `kLobeBigSide`: coverage does not drive
// SHARPNESS in a voxel renderer, because a face is one voxel and the pixels looking at it span less
// than a degree. It could only ever have been an affordability lever, and the pool is not short --
// 5,324 faces of 19,026 held the sharp class at the door with room to spare.
//
// So the class follows ROUGHNESS, which is a material property and therefore agrees between
// neighbours by construction. If the pool does run short the decline path already handles it, and
// the audit line already says so.

// And how many samples a bin of a big block asks for. FEWER than a small one's, and deliberately:
// the total rays a face spends over its life is then 144 x 8 against 36 x 24 — **the same 1,152**,
// so the expensive class costs the ray budget nothing at all and buys its sharpness out of the
// cross-face blend instead. R5 blends nine faces, which is what carries a bin at eight samples.
const uint kLobeBigSamples = 24u;

// How cold a block's holder has to be before another face may take it. The same six hundred frames
// the face store gives up a slot at, and for the same reason: a face the camera comes back to
// should find its own answer where it left it. A face on screen stamps its block every time the
// shading pass visits it, which is one frame in `face_stride`.
const uint kLobeCold = 600u;

// How much more a face has to be worth before it takes a WARM block off another face. A margin and
// not a comparison, because taking a block resets the count on it and sends its holder back to the
// start of its burst — so two faces of nearly equal worth sharing a set would trade one for ever
// and neither would ever converge. See the take-over in `node_face_lobe`.
const float kLobeTakeMargin = 1.5;

// And how much a face has to be worth before it asks at all. See `face_lobe_worth`.
//
// This admits every metal on the facility -- copper is the weakest at 0.375 -- **and the glass and
// the water at 0.040**, which are the only near-mirrors in the building and are the surfaces a
// reflection is most visible on. It leaves out marble at 0.036 and limestone at 0.023.
//
// It was 0.050 for one landing, which shut the glass and the water out by four thousandths, and
// that is most of why the first pictures had nothing in them anybody could see (D592). It is a
// CAPACITY dial and not a material one: it says how far down the pool's room reaches, which is the
// question `face_pressure` answers about slots. `--lobe-floor N` is the worth itself, so a figure
// from the census can be typed straight in; 1 is the arm where nobody asks.
const float kLobeWorthFloor = 0.038;

// Which four blocks a slot may hold. Set-aligned, so the four ways are adjacent and their headers
// are four adjacent pairs -- one cache line for the probe the composite runs per metal pixel.
uint face_lobe_set(uint slot) {
    uint h = slot * 0x9E3779B9u;
    h ^= h >> 16;
    h *= 0x7FEB352Du;
    h ^= h >> 15;
    return (h & (kLobeBlocks - 1u)) & ~(kLobeWays - 1u);
}

// The headers are the front of the buffer, two words a block: who holds it, then the frame that
// holder last touched it.
//
// The holder's WORTH rides in the same word as its slot, above the twenty-one bits a slot needs, so
// that one compare-and-swap decides a take-over on both at once. Split across two words it would be
// possible to lose a race and read the loser's worth beside the winner's slot -- which is the one
// state this arrangement must not be able to reach, because the next face to arrive would then
// compare itself against a number nobody holds.
//
// The frame is a whole word rather than sixteen bits beside the worth, and that is not thrift. A
// sixteen-bit frame wraps every eighteen minutes at sixty a second, and a block whose holder went
// away just before the wrap reads as touched a moment ago for ever -- a block that can never be
// taken, in a pool whose whole recovery mechanism is that a cold block can be.
// The slot is stored **one greater than it is**, so that a header of nought means "nobody holds
// this" and a pool that has merely been allocated is an empty one. A sentinel of all-ones would
// need the whole 512 KB of headers filled before the first frame -- a pass, a barrier and an
// ordering to get right -- to say what zero says for free, and every other card-owned array here
// already relies on the allocator's zero meaning exactly what it looks like.
//
// FOUR words a block and not two, since R4b: the third is how many rays this lobe has taken, which
// is what stops it casting, and the fourth is spare. A count derived from the bins instead would be
// sixty-four loads to answer a question asked on every visit of every lobe face — the same
// arithmetic `kFaceAmbientDone` exists in `photons` to avoid, one array along.
const uint kLobeSlotBits = 21u;             // 2,097,152, against a store of 1,081,344 slots
const uint kLobeSlotMask = (1u << kLobeSlotBits) - 1u;
const uint kLobeHeaderWords = 4u;
uint face_lobe_header(uint block) { return block * kLobeHeaderWords; }
uint face_lobe_count_at(uint block) { return block * kLobeHeaderWords + 2u; }
// ...and the fourth word, which says whether this lobe has been blended with its neighbours' yet.
// One bit, because the blend runs ONCE — see `face_lobe_denoise` in shade_faces.comp for why a
// filter that reads the array it writes may run once and must not run twice.
uint face_lobe_state_at(uint block) { return block * kLobeHeaderWords + 3u; }
const uint kLobeFiltered = 1u;
// ...and which SIZE this block is. `kLobeBig` marks the head of a run of four; `kLobeFollower`
// marks the other three, so a reader that lands on one of them knows to step back to the head
// rather than reading three quarters of a lobe as a whole one.
const uint kLobeBig = 2u;
const uint kLobeFollower = 4u;
// ...and R4c's image: whether this block's bins have been CENTRED on a direction yet, and which
// direction that is. See `face_lobe_cap_of` for what the cap is and why the bins stopped covering
// the whole hemisphere. Bit 3, and the packed axis in the twenty-four bits above bit 8 — so the
// three class bits below it, which `node_face_lobe` writes and this file must never clobber, keep
// the values they always had and a block claimed by that function reads back as "not centred".
const uint kLobeAxis = 8u;
const uint kLobeAxisShift = 8u;
const uint kLobeAxisBits = 12u;   // per component of the packed direction, so 24 in all
const uint kLobeAxisMask = (1u << kLobeAxisBits) - 1u;
uint face_lobe_axis_of(uint state) { return (state >> kLobeAxisShift) & 0xFFFFFFu; }
uint face_lobe_holder(uint slot, float worth) {
    return ((slot + 1u) & kLobeSlotMask) |
           (uint(clamp(worth, 0.0, 1.0) * 255.0 + 0.5) << kLobeSlotBits);
}
bool face_lobe_is_held(uint held) { return (held & kLobeSlotMask) != 0u; }
uint face_lobe_holder_slot(uint held) { return (held & kLobeSlotMask) - 1u; }
float face_lobe_holder_worth(uint held) {
    return float((held >> kLobeSlotBits) & 0xFFu) * (1.0 / 255.0);
}

// ...and the bins are behind every header, kLobeBlockWords a block: the packed means as one run,
// then the weight sums behind them, so four bilinear taps are four words inside 64 bytes.
// The bins, and the count is passed because a big block's are one run of 144 where a small block's
// are one run of 36. A block whose bins were indexed with the wrong count reads its own weights as
// colours, which is the shape of fault kFaceLightWords is declared in this file to prevent.
uint face_lobe_bin_at(uint block, uint bin) {
    return kLobeBlocks * kLobeHeaderWords + block * kLobeBlockWords + bin;
}
uint face_lobe_weight_at(uint block, uint bin, uint bins) {
    return kLobeBlocks * kLobeHeaderWords + block * kLobeBlockWords + bins + bin;
}

// How many bins a block holds and how wide across it is, from the class recorded in its head.
uint face_lobe_bins_of(uint state) {
    return (state & kLobeBig) != 0u ? kLobeBigBins : kLobeBins;
}
uint face_lobe_side_of(uint state) {
    return (state & kLobeBig) != 0u ? kLobeBigSide : kLobeSide;
}

// Where the centre of bin `index` points, on the square, at a given grid width.
vec2 face_lobe_centre_of(uint index, uint side) {
    return (vec2(float(index % side), float(index / side)) + 0.5) * (1.0 / float(side));
}

// The width the bins themselves impose at a given grid, as a GGX alpha. See face_lobe_bin_alpha.
float face_lobe_alpha_of(uint bins) {
    return acos(clamp(1.0 - 1.0 / float(bins), -1.0, 1.0));
}

// How many bins this material would actually use, from its roughness alone.
//
// A GGX lobe of width alpha covers about pi*alpha^2 steradians of the 2*pi a hemisphere has, so it
// takes about 2/alpha^2 bins to hold it without blurring it. Gilt at 0.063 wants five hundred and
// gets what the pool can give; limestone at 0.646 wants five and is nowhere near the floor to ask.
float face_lobe_bins_wanted(uint rough_byte) {
    const float alpha = face_alpha_of(rough_byte);
    return 2.0 / max(alpha * alpha, 1e-6);
}

// ---- R4c's image: the bins cover a CAP about the eye, not the whole hemisphere -----------------
//
// **This is what was owed. D591 put the storage in, D592 said plainly that there was no reflection
// anybody could recognise, and D594 put the RAY in — and the picture still has no image in it,
// because thirty-six bins over a hemisphere is 13.5 degrees a bin and a hundred and forty-four is
// 6.8. Nothing recognisable survives either. The bins hold light; they did not hold a picture.**
//
// The obvious answer is more bins, and it is the answer D592 already priced and rejected twice: 256
// bins are two samples apiece out of a face's whole ray budget, and an image at a degree and a half
// over a hemisphere is about fourteen thousand bins, which is 28,800 words a face. The pool is
// 38.9 MB for seventy-two.
//
// # What makes it affordable, and it is a fact about a VOXEL renderer
//
// **The composite reads exactly ONE direction out of a face.** D597 made that a rule rather than an
// accident: the lobe is evaluated from the face's own CENTRE to the eye, once, because a voxel face
// has to be one colour and a per-pixel direction drew a gradient across every 3 cm face. And a face
// is one voxel — every pixel looking at it looks from within 0.86 degrees of every other, which is
// D186's own measurement from the other end.
//
// So a hemisphere of bins spends almost all of its resolution on directions nothing will ever read.
// A face does not need to know what it reflects in every direction at once; it needs to know what it
// reflects along the handful of degrees the eye is actually in. Point the same thirty-six bins at a
// cap of six degrees and they are 2.0 degrees apart instead of 13.5 — which is where an image
// starts — for the same thirty-six bins, the same 38.9 MB, the same 864 rays and the same pass.
//
// # How wide the cap is, and it is roughness and nothing else
//
// `cap = alpha * side / 2`, clamped. That is not a tuning: it is the statement that a bin should be
// about as wide as the material's own lobe, said as an equation. Bronze at `rough=110` is a 10.7
// degree lobe and comes out with 10.7 degree bins over a 32 degree cap — the hemisphere is not worth
// covering for it either, and nothing about it changes. Chrome at `rough=8` asks for 0.2 degree bins
// and is stopped by the floor, which is the only constant here.
//
// **The floor is about camera motion and not about sharpness.** A cap is a claim about where the eye
// is, and the eye moves: at three metres, walking one centimetre sideways turns the direction to a
// face by 0.19 degrees. Six degrees is about a third of a metre of standing still, and a cap that
// tracked the material all the way down would be re-centred — and so re-measured — on every frame a
// player breathed. The ceiling is the other end of the same statement: past about a radian the
// orthographic map that puts a cap on a square is distorting more than the extra reach is worth, and
// a material that rough has a lobe wider than its whole cap anyway.
const float kLobeCapMin = 0.105;      // six degrees
const float kLobeCapMax = 1.000;      // fifty-seven
// How far out of its cap the eye may walk before the face re-centres it. A fraction rather than an
// angle, so it scales with the cap: at 0.6 the read is still well inside the bins that have samples
// in them when the move happens, which is what makes a re-centre a fade rather than a hole.
const float kLobeCapRecentre = 0.6;

float face_lobe_cap_of(uint rough_byte, uint side) {
    return clamp(0.5 * face_alpha_of(rough_byte) * float(side), kLobeCapMin, kLobeCapMax);
}

// The kernel a cap's samples are drawn from and splatted through.
//
// **This is the second half of the change and without it the first half buys nothing.** R4c widens
// the kernel to the BIN on purpose (see `face_lobe_bin_alpha`): a lobe narrower than a bin makes
// every sample land in a tail, which is noise rather than blur. That argument is about the bin being
// the coarser of the two, and under a cap it usually is not — so the kernel follows the MATERIAL,
// floored at half the spacing between bin centres so that neighbouring bins' kernels still overlap
// and the bilinear read has something continuous to interpolate.
//
// A mirror therefore stores a near-point sample of the world in each bin, blurred to one degree
// rather than to thirteen, and that is the difference between an average and a picture.
float face_lobe_cap_kernel(uint rough_byte, float cap, uint side) {
    return max(face_alpha_of(rough_byte), cap / float(side));
}

// The cap's two axes across, from the face's own frame rather than from the axis alone.
//
// It has to be a function of things BOTH passes have and of nothing else, for exactly the reason
// `face_lobe_frame` above exists: the pass that writes a bin and the pass that reads one derive this
// independently, and a frame that disagrees by a swap or a sign is a reflection that is mirrored and
// looks very nearly right. Deriving it from the face's fixed frame also makes it CONTINUOUS in the
// axis, which `face_lobe_frame(axis, ...)` would not be — its own branch at |y| = 0.9 would rotate
// two neighbouring faces' bins against each other and R5's cross-face blend would then average bin j
// of one against a completely different direction of the other.
void face_lobe_cap_frame(vec3 axis, vec3 side, vec3 other, out vec3 u, out vec3 v) {
    vec3 t = side - axis * dot(side, axis);
    // Only when the eye is very nearly along `side`, which is a grazing view of the face.
    if (dot(t, t) < 1e-4) t = other - axis * dot(other, axis);
    u = normalize(t);
    v = cross(axis, u);
}

// A direction to a point on the cap's square, and back. Orthographic — the direction is projected
// onto the plane the axis is normal to and divided by the cap's sine — which is exact, invertible
// and has no pole, and whose only cost is that it compresses towards the rim.
//
// `inside` is a separate answer rather than a clamp for trap 7's reason: a direction outside the cap
// is one this block has measured NOTHING about, and reading the rim bin for it would draw the
// nearest thing the face happens to reflect as though it were what the eye is looking at. The
// composite falls back to the hemispherical mean instead, which is what a face with no block reads
// and is the picture as it was before this stage.
vec2 face_lobe_cap_at(vec3 dir, vec3 axis, vec3 u, vec3 v, float sin_cap, out bool inside) {
    const vec2 p = vec2(dot(dir, u), dot(dir, v)) / max(sin_cap, 1e-4);
    inside = dot(dir, axis) > 0.0 && max(abs(p.x), abs(p.y)) <= 1.0;
    return p * 0.5 + 0.5;
}

vec3 face_lobe_cap_direction(vec2 at, vec3 axis, vec3 u, vec3 v, float sin_cap) {
    vec2 p = (at * 2.0 - 1.0) * sin_cap;
    // The square's corners are further out than the cap's rim, and past a unit vector's length there
    // is no direction to be had at all. Pulled back to the rim rather than dropped, so the corner
    // bins hold the grazing edge of the cap instead of holding nothing.
    const float r2 = dot(p, p);
    if (r2 > 0.9801) p *= sqrt(0.9801 / r2);
    return normalize(u * p.x + v * p.y + axis * sqrt(max(1.0 - dot(p, p), 0.0)));
}

// The axis itself, packed into the twenty-four bits of the state word above the class bits.
//
// Through the same hemi-octahedral square the bins used to be indexed on, at twelve bits a
// component — about a thirtieth of a degree, which is two orders finer than the narrowest cap. It
// is stored rather than recomputed because the READER must use the axis the samples were actually
// taken about: recomputing it from this frame's camera would put the bins one frame of camera
// motion away from where they were filled, which at a two degree bin is visible.
uint face_lobe_axis_pack(vec3 dir, vec3 normal, vec3 side, vec3 other) {
    const vec2 at = clamp(face_lobe_at(dir, normal, side, other), vec2(0.0), vec2(1.0));
    const uvec2 q = uvec2(at * float(kLobeAxisMask) + 0.5);
    return (q.x & kLobeAxisMask) | ((q.y & kLobeAxisMask) << kLobeAxisBits);
}

vec3 face_lobe_axis_unpack(uint bits, vec3 normal, vec3 side, vec3 other) {
    const vec2 at = vec2(float(bits & kLobeAxisMask), float((bits >> kLobeAxisBits) & kLobeAxisMask)) *
                    (1.0 / float(kLobeAxisMask));
    return face_lobe_direction(at, normal, side, other);
}

// ---- R4d: the hero wavelength, which is how dispersion is paid for -----------------------------
//
// **A march per wavelength is the cost nobody here has paid, and the plan's own note says so: it is
// a hero wavelength per face SAMPLE.** A face takes hundreds of samples where a pixel takes one, so
// a face can carry a spectral estimator that a pixel cannot — draw one wavelength per sample, bend
// that sample by the index the medium has AT that wavelength, and weight what comes back by the
// wavelength's share of the sensor. The mean over samples is the dispersed answer, and it costs
// exactly the marches the sample was already going to make.
//
// This lives in the file with no bindings in it because both passes that could ever want it are the
// two that include this file, and the alternative — pt_media.glsl — reads the tracer's own push
// block and cannot be included by the face pass at all.

// Where in the visible band this sample looks, in [0, 1] from the red end to the blue.
//
// STRATIFIED by the sample index rather than drawn at random, for the same reason the bins are
// served round-robin: a random wavelength per sample leaves whole stretches of the band unvisited
// for hundreds of frames, and a bin whose samples happened to be mostly red is a bin with a colour
// cast that never goes away. The golden ratio is what turns a running index into a sequence that is
// even at every prefix rather than only at the end.
float face_hero_at(uint index) {
    return fract(float(index) * 0.61803398875 + 0.5);
}

// ...and what that wavelength is worth to each channel, normalised so that the MEAN over the band is
// one in every channel. That normalisation is the whole of what makes this unbiased: a material with
// no dispersion at all must come back bit for bit what it was, and it does, because every sample is
// then the same radiance weighted by three numbers whose mean is (1, 1, 1).
//
// The three lobes are the sensor this renderer already is — it works in linear RGB and has no
// spectral basis — so they are the plainest thing that is true of any RGB sensor: three bands, the
// red one at the long end, the blue at the short, the green between, overlapping at the edges. What
// they are NOT is a CIE fit, and pretending otherwise would be inventing a colour space this
// renderer does not have.
vec3 face_hero_weight(float hero) {
    // Centres at five sixths, a half and a sixth of the band, and a width that makes the three sum
    // to very nearly a constant across it.
    const vec3 centre = vec3(0.8333, 0.5, 0.1667);
    const vec3 d = (vec3(hero) - centre) * (1.0 / 0.30);
    const vec3 lobe = exp(-d * d);
    // Divided by ITS OWN mean over the band, one number a channel, and the three are not the same
    // number. The band is finite, so the two end lobes are cut off on one side and carry a fifth
    // less area than the middle one: the integral of exp(-((x-c)/0.3)^2) over [0, 1] is 0.5219 at
    // c = 0.5 and 0.4168 at c = 1/6. One divisor for all three made the red and the blue come back
    // at 0.825 of what went in, which is a pane of ordinary glass tinting what is behind it green,
    // and it is the test at the bottom of `tests/test_refraction.cpp` that named it.
    return lobe * vec3(2.3992, 1.9160, 2.3992);
}

// The index of refraction at that wavelength.
//
// Cauchy, written as the Abbe number it is usually published as: n(F) - n(C) = (n - 1) / V, and the
// band is walked linearly between them because the curvature of Cauchy's own term across the visible
// is far below the byte `ior` is quantised into.
//
// **`V` is a constant here and it should be a byte on the material, which is said plainly because it
// is the one thing this cannot get right.** `VisualRecord` carries `ior` and nothing about
// dispersion, so every medium in this renderer disperses like the same glass. Twenty-five is dense
// flint — the most dispersive glass anybody puts in a window — which is the top of the honest range
// and therefore the setting that gives this the best chance of being seen; a crown at sixty-four
// would be under half of it. What the field would be called is `abbe`, one byte, 0 meaning "the
// default below".
const float kMediumAbbe = 25.0;

float face_medium_index(float ior, float hero) {
    // hero 0 is the red end and 1 the blue, and blue bends more, so the spread is centred on the
    // authored figure: a material read at the middle of the band is exactly what the clip wrote.
    return ior + (ior - 1.0) * (hero - 0.5) * (1.0 / kMediumAbbe);
}

// ---- R4b: a ray aimed into the cone one bin gathers from ---------------------------------------
//
// The half vector of a GGX lobe of width `alpha` about the normal, from two uniform numbers. The
// standard importance sample, and the reason it is here rather than in the pass that casts is that
// the pass that READS a bin has to agree about what shape a bin is: the kernel this draws from and
// the kernel `face_lobe_accumulate` splats through are one width, and a bin whose samples come from
// a narrower cone than the one it is read with is a bin biased towards its own centre.
vec3 face_lobe_half_vector(float alpha, vec2 at, vec3 normal, vec3 side, vec3 other) {
    const float phi = at.x * 6.28318530718;
    const float a2 = alpha * alpha;
    // cos(theta) of the GGX normal distribution, inverted analytically.
    const float cos_theta = sqrt(clamp((1.0 - at.y) / (1.0 + (a2 - 1.0) * at.y), 0.0, 1.0));
    const float sin_theta = sqrt(max(1.0 - cos_theta * cos_theta, 0.0));
    return normalize(side * (sin_theta * cos(phi)) + other * (sin_theta * sin(phi)) +
                     normal * cos_theta);
}

// ...and the same thing done properly, which is the DISTRIBUTION OF VISIBLE NORMALS.
//
// **The plain sample above starves exactly the bins a reflection is read from, and that is why it
// had to be replaced.** It draws a half vector about the NORMAL, and the ray is then the bin's own
// direction mirrored about that half vector. For a bin near the rim — a grazing direction, which is
// where every reflection worth looking at is read — the bin's direction and a half vector near the
// normal are almost ninety degrees apart, so the mirrored ray comes back **below the surface** and
// is thrown away. The bins that are hardest to fill were being sampled by the one scheme that
// cannot fill them.
//
// Heitz's method samples the half vectors that are actually VISIBLE from the direction in question,
// so a grazing bin gets grazing half vectors and its rays land above the surface by construction.
// It is the standard fix for the standard fault and it is fifteen lines.
//
// `towards` is in the face's own frame, z along the normal. What comes back is too.
vec3 face_lobe_visible_normal(vec3 towards, float alpha, vec2 at) {
    // Stretch into the space where the lobe is a hemisphere.
    const vec3 stretched = normalize(vec3(alpha * towards.x, alpha * towards.y, towards.z));
    const float flat_sq = stretched.x * stretched.x + stretched.y * stretched.y;
    const vec3 t1 = flat_sq > 0.0 ? vec3(-stretched.y, stretched.x, 0.0) * inversesqrt(flat_sq)
                                  : vec3(1.0, 0.0, 0.0);
    const vec3 t2 = cross(stretched, t1);
    const float radius = sqrt(clamp(at.x, 0.0, 1.0));
    const float phi = at.y * 6.28318530718;
    const float u = radius * cos(phi);
    float v = radius * sin(phi);
    // The half of the disc behind the horizon is folded rather than clipped, which is what makes
    // this a sample of the VISIBLE half and not of the whole distribution.
    const float lean = 0.5 * (1.0 + stretched.z);
    v = (1.0 - lean) * sqrt(max(1.0 - u * u, 0.0)) + lean * v;
    const vec3 lifted =
        u * t1 + v * t2 + sqrt(max(1.0 - u * u - v * v, 0.0)) * stretched;
    // ...and unstretch.
    return normalize(vec3(alpha * lifted.x, alpha * lifted.y, max(lifted.z, 1e-6)));
}

// How many words the whole pool is, which is what the host allocates.
uint face_lobe_pool_words() { return kLobeBlocks * (kLobeHeaderWords + kLobeBlockWords); }

// How much this face's lobe is worth storing bins for, in [0, 1].
//
// It decides RESIDENCY and nothing about the picture: the pool of bin blocks is a fraction of the
// store, so something has to say which faces get one, and this is the same shape of question the
// face store's own pressure answers about slots. Two continuous factors, both out of the two bytes
// R4a put on the face:
//
//   - how much of what leaves the surface is specular at all, which is metalness. A dielectric
//     reflects four per cent and a metal all of it;
//   - how much DIRECTION a lobe of this width has left after the bins have blurred it. A fully
//     rough surface's lobe is the hemisphere, its bins would all hold the same number, and the
//     number is one the face already stores.
//
// Deliberately NOT a function of the albedo, which the light pass does not have for its own face —
// it reads the marcher's folded colour for a surface a ray LANDS on and has never needed one for
// itself. Metalness alone is the upper bound on the true worth, which is the right direction for a
// residency test: it can admit a face whose lobe turns out dim, and it cannot turn one away.
float face_lobe_worth(uint rough_byte, uint metal_byte) {
    const float alpha = face_alpha_of(rough_byte);
    const float strength = mix(kFaceDielectricF0, 1.0, float(metal_byte) * (1.0 / 255.0));
    return strength * clamp(1.0 - alpha * alpha, 0.0, 1.0);
}

// ---- rgb9e5, which is what a bin is stored in --------------------------------------------------
//
// One word for three channels of radiance with a shared exponent, which is the format §4 of the
// plan names for exactly this. A bin holds an unbounded quantity — the sky is thousands of times a
// wall — so a fixed point would either clip the sky or quantise the wall, and three halves would be
// two words a bin for a precision nothing here can use.
const int kFaceRgb9e5Bias = 15;
const int kFaceRgb9e5Mantissa = 9;
const float kFaceRgb9e5Max = 65408.0;   // (511/512) * 2^16, the largest the format holds

uint face_pack_rgb9e5(vec3 c) {
    c = clamp(c, vec3(0.0), vec3(kFaceRgb9e5Max));
    const float largest = max(max(c.r, c.g), c.b);
    // The exponent that puts the largest channel just under one, and the floor is what keeps a
    // black bin from taking the logarithm of nought.
    int e = max(-kFaceRgb9e5Bias - 1, int(floor(log2(max(largest, 1e-30))))) + 1 + kFaceRgb9e5Bias;
    float unit = exp2(float(e - kFaceRgb9e5Bias - kFaceRgb9e5Mantissa));
    // Rounding the largest channel can carry it past the mantissa's range, which is one step of
    // exponent rather than an overflow. Checked rather than clamped, because clamping it would make
    // the brightest bin in the frame darker than the one beside it.
    if (int(floor(largest / unit + 0.5)) > 511) {
        e += 1;
        unit *= 2.0;
    }
    const uvec3 m = uvec3(clamp(floor(c / unit + 0.5), vec3(0.0), vec3(511.0)));
    return (uint(clamp(e, 0, 31)) << 27) | (m.b << 18) | (m.g << 9) | m.r;
}

vec3 face_unpack_rgb9e5(uint packed) {
    const float unit = exp2(float(int(packed >> 27) - kFaceRgb9e5Bias - kFaceRgb9e5Mantissa));
    return vec3(float(packed & 0x1FFu), float((packed >> 9) & 0x1FFu),
                float((packed >> 18) & 0x1FFu)) * unit;
}

// ---- what the lobe is worth to a pixel, once the environment along it is known -----------------
//
// The environment half. `reflected` is the radiance arriving along the mirror direction — a bin if
// the face has them and its hemispherical mean if it has not — and what comes back is what the eye
// sees of it.
//
// The Fresnel is taken at the VIEW angle rather than at the half vector, which is the split-sum
// approximation every real-time renderer uses for a prefiltered environment and is exact for a
// mirror. What it costs is a little too much grazing reflection on a rough surface, which is far
// below what sixteen bins are already doing to the same picture.
vec3 face_lobe_environment(vec3 f0, float n_dot_v, vec3 reflected) {
    return face_fresnel(f0, n_dot_v) * reflected;
}

// The sun half, which needs no storage at all: the face has already measured what fraction of the
// disc it can see, and the rest is the same arithmetic the diffuse `lambert` above is.
//
// This is the term a player reads as metal before any of the others — a highlight that slides
// across a door as they walk past it — and it is the sharpest thing in the frame, because it is not
// quantised by the bins.
//
// **The clamp is the engine's own sun and not a taste constant.** `pt_sky.glsl` draws the disc at
// `sun_radiance()` — see the comment there, which says in as many words that it exists "so a mirror
// reflects something rather than a flat gradient" — so a perfect mirror must show the sun at exactly
// that and no brighter. A GGX lobe narrower than the sun's own disc integrates to more than one
// without it, and what that draws is a single blinding pixel that the light meter then exposes the
// whole building down to avoid.
vec3 face_lobe_sun(vec3 f0, vec3 normal, vec3 view, vec3 to_sun, float alpha, float visible) {
    const float n_dot_l = dot(normal, to_sun);
    const float n_dot_v = dot(normal, view);
    if (n_dot_l <= 0.0 || n_dot_v <= 0.0 || visible <= 0.0) return vec3(0.0);
    const vec3 half_vector = normalize(view + to_sun);
    const float n_dot_h = max(dot(normal, half_vector), 0.0);
    const float v_dot_h = max(dot(view, half_vector), 0.0);
    const float reflectance = min(face_ggx_d(n_dot_h, alpha) *
                                      face_ggx_vis(n_dot_v, n_dot_l, alpha) * n_dot_l,
                                  1.0);
    return face_fresnel(f0, v_dot_h) * (reflectance * visible);
}
#endif  // WS_FACE_TERMS_GLSL
