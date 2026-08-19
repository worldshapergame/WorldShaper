#ifndef WS_REFLECT_GLSL
#define WS_REFLECT_GLSL
// R4f: a specular reflection is a CONTINUATION of the primary ray, not a lookup into a stored
// distribution.
//
// **This reverses the rule R4c was built under.** That rule was *everything is per voxel face
// based -- even reflections*, and what it produced is D591, D592, D594, D597, D599, D694 and D703:
// six attempts to get a picture out of a per-face distribution, each of which measured the same
// wall. The user reversed it after playing the build -- *"reflections should not be voxel face
// based, it should be more like ior bending light, and still be super performant"* -- and this file
// is the other half of D652 arriving beside it. D652 put the TRANSMITTED half of what happens at an
// interface back on the ray: it bends by the material's own index of refraction, crosses the
// medium, bends again and marches on. What was missing is the REFLECTED half, which leaves along
// the mirror direction and marches on in exactly the same sense.
//
// Three of the standing complaints dissolve into that rather than being fixed:
//
//   - *"make reflections support infinite reflections between them"*. A reflection is another
//     continuation of the same march, so two facing mirrors recurse until the CONTRIBUTION budget
//     below stops them. Depth is a budget and not a stored structure and not a count.
//   - *"when im moving reflective surfaces like mirrors get stripes of non reflective zones"*.
//     That stripe is D703's cap re-centring -- a face whose stored cap the eye has walked out of
//     reads nothing and falls back to the hemispherical mean for the second its burst takes. With
//     no cap in the answer there is nothing to walk out of.
//   - *"a reflection you cannot recognise"* (D592's own words). A bin is a cone and a picture needs
//     more cones than there are rays to fill them; a ray is a direction and needs none.
//
// # What is NOT ripped out, and why
//
// The lobe pool stays, and it stays as the ROUGH surface's answer. A rough surface's lobe is far
// wider than a pixel, one ray into it is noise, and the face has already averaged hundreds of them
// -- so the converged average is both the cheap answer and the correct one. What changes is that it
// is no longer *the* specular path for a sharp surface, which is the job it could never do.
//
// # The rule that decides between them, and it is continuous with no threshold anywhere
//
// R4's standing rule is that nothing in this stage may branch on a material's identity or compare a
// roughness against a cutoff. So:
//
//   > How much of the answer comes from THIS RAY versus from the face's ALREADY-ACCUMULATED
//   > AVERAGE is the lobe's angular width measured against the angular resolution the stored
//   > answer can carry -- which is the face's own angular size, floored at the pixel's.
//
// A mirror's lobe is a tenth of a degree and a voxel face at three metres subtends half a degree,
// so the pixel's own ray is nearly the whole answer: sharp, correct and recursive. A brushed
// metal's lobe is twenty degrees, forty times the face, so the average is nearly the whole answer:
// cheap, already measured, no noise. Nothing branches. It is the same comparison the marcher
// already makes between a node's size and a pixel's footprint, applied to DIRECTION instead of to
// position -- which is why the floor is the pixel: below the pixel there is nothing to resolve.
//
// # And the pixel is what makes it affordable
//
// `reflect_share` is a smooth function that reaches nought on anything rough, and a ray whose whole
// contribution -- Fresnel times share times whatever throughput it already carries -- is under
// `kReflectBudget` is not cast at all. On a scene with no polished material in it that test fails
// at every pixel and the stage costs one comparison. That is the sense in which R4's *"within 15%
// on a scene with no reflective material"* is met by construction rather than by measurement.

// ---- the budget, which is what bounds the recursion --------------------------------------------
//
// A reflection terminates on what it is WORTH and never on a depth count, and that is the whole of
// why two facing mirrors recurse. Most dielectrics turn away four per cent of what falls on them
// head on, so a second bounce off ordinary stone is already under a thousandth and costs nothing;
// two chrome surfaces facing each other keep three quarters of it per bounce and go on for a dozen.
//
// The number is a share of the pixel, and it bounds the DIFFERENCE the ray would have made rather
// than the ray's own radiance: what a refused ray costs is `F * share * (ray - stored average)`,
// because the stored average is what is drawn in its place.
//
// A fiftieth. Above it the polished floor's four per cent still casts (four per cent of a share
// near one); below it a rough panel at a grazing angle does not (forty per cent of a share near a
// fiftieth). Both of those are measured cases and neither is a roughness test.
const float kReflectBudgetDefault = 0.02;

// How many bounces the loop is WRITTEN for, which is not the same as how many it takes.
//
// GLSL wants a bound, and the report has to say which of the two actually stopped a given series.
// For every dielectric in this repository it is the budget, and long before sixteen: polished stone
// at four per cent head on is under a fiftieth after ONE bounce, so a second is never cast.
//
// **For two CHROME walls it is this number, and that is measured rather than assumed.**
// `clips/mirror_hall.clip` keeps 0.83 of the pixel per bounce -- a Fresnel of 0.918 sixty degrees
// off the normal times a share of 0.904 -- so a budget of a fiftieth would reach twenty-one. Swept
// against the picture, the hall stops changing between `--reflect-budget 0.05` and 0.02 (2.28 of
// 255 against a same-arm floor of 2.27) and its cost stops rising with it (12.77 ms against 12.86),
// which is exactly where the arithmetic puts the sixteenth bounce. At 0.30 it stops at six and the
// BUDGET is what stops it: the picture moves 3.84 between 0.30 and 0.12 and 7.49 between 0.12 and
// 0.05.
//
// So: sixteen images of a hall, and a number that could be raised if anybody ever wants more.
const uint kReflectBounces = 16u;

// ---- and what a reflected escape pays for the cloud deck ----------------------------------------
//
// The deck a reflection sees has to be MARCHED -- see `reflect_escaped` below for why there is no
// cache to read it out of -- and marching it at the primary ray's quality was measured at 2.60 ms
// of visibility becoming 18.06 on `clips/mirror_test.clip`. That is the deck costing six times the
// whole rest of the frame in order to appear in a mirror, which is not a trade anybody would take.
//
// Both numbers are about the SHADING inside a cloud rather than about where the cloud is, and a
// reflection is the case where that shading cannot be seen: it arrives through a Fresnel term under
// one, blurred by the surface's own lobe, at a resolution the face bounds. Sixteen steps still
// resolve the density field, which is the shape a player recognises; nought metres of light march
// takes the sun and sky optical depths from the local density, which is the estimate `cloud_march`
// already falls back to past fifteen kilometres for the same reason -- the detail is being thrown
// away by the haze anyway.
const int kReflectCloudSteps = 16;
const float kReflectCloudLightMetres = 0.0;

// ---- Fresnel from the material's own index of refraction ---------------------------------------
//
// `face_f0_of` mixes a fixed 0.04 towards the albedo by metalness, and 0.04 is exactly what
// ((n-1)/(n+1))^2 gives at n = 1.5 -- so every dielectric in this renderer has been assumed to be
// window glass. `VisualRecord` has carried an `ior` byte since the record existed and D652 was the
// first ray to read it. This is the second: where a material says what its index is, its Fresnel
// comes from that index.
//
// The byte at NOUGHT is "the record did not say" and not "n = 1", which would be a surface that
// reflects nothing at all -- trap 7 in the shape it always takes here. Every material that has
// never mentioned an index therefore comes back at exactly `kFaceDielectricF0` and this changes
// nothing about it.
float reflect_dielectric_f0(uint type_id) {
    const uint type_at = min(type_id, uint(types.items.length()) - 1u);
    const uint visual_at = min(types.items[type_at].x, uint(visuals.items.length()) - 1u);
    const uint byte = (visuals.items[visual_at].y >> 16u) & 0xFFu;
    if (byte == 0u) return kFaceDielectricF0;
    const float n = 1.0 + float(byte) * (1.0 / 128.0);
    const float r = (n - 1.0) / (n + 1.0);
    return r * r;
}

// The specular colour at normal incidence, with the dielectric half taken from the index above.
vec3 reflect_f0_of(vec3 albedo, uint metal_byte, uint type_id) {
    return mix(vec3(reflect_dielectric_f0(type_id)), albedo,
               float(metal_byte) * (1.0 / 255.0));
}

// ---- the continuous share ----------------------------------------------------------------------
//
// The angular width of the reflected lobe, in radians, from the roughness byte.
//
// `face_alpha_of` is NOT used here and that is deliberate. Its floor -- `kFaceAlphaMin`, 0.004 --
// exists so the GGX distribution does not divide by nought, which is a statement about an
// expression and not about how wide a mirror's lobe is. Read as a width it puts half a degree of
// blur under every polished surface in the world, which is four times what `rough=8` chrome asks
// for and is enough on its own to decide the share. So the width is the roughness the clip author
// actually wrote, and the floor stays where it belongs.
//
// Twice alpha, because alpha is the spread of the HALF vector and a reflection is about the
// outgoing direction, which a microfacet turns by twice as much.
float reflect_lobe_width(uint rough_byte) {
    const float r = float(rough_byte) * (1.0 / 255.0);
    return 2.0 * r * r;
}

// How much of the answer this pixel's own ray is entitled to be.
//
// `carrier` is the angular resolution the thing that would answer INSTEAD can carry: the face's own
// angular size, because the stored average is one number for the whole face read along one
// direction (D703's measurement, arriving from the other end), floored at the pixel because below
// the pixel there is nothing to resolve either way.
//
// One over one plus the ratio. Smooth, one at zero width, nought in the limit, and with no constant
// in it that could be read as a threshold.
float reflect_share(float lobe_width, float carrier) {
    return carrier / max(carrier + lobe_width, 1e-9);
}

// ---- what a reflected ray that ESCAPES is worth --------------------------------------------------
//
// **Reported by the user while this was being built**: *"the reflections i see dont reflect the
// actual sky colors and neither clouds."* Both halves are here and they have different causes.
//
// # The colours
//
// `sky_radiance` is the composite's own `sky_colour`, the same function out of the same file, so a
// reflection and the eye see one sky rather than two plausible ones. What made the OLD reflection
// disagree was not the sky model: it was that a bin is an average over thirteen degrees, so the
// gradient the eye sees across a sky arrived in a lobe as one colour. A ray has a direction and
// needs no such averaging.
//
// # The clouds, and this is the part that does not come free
//
// The composite draws the deck out of `in_cloud`, which is a full-resolution history the cloud pass
// marches ONE RAY PER PIXEL along -- so it is indexed by pixel, and a pixel names the direction the
// PRIMARY ray went. A reflected direction is a different direction and that image has no answer for
// it, whatever pass reads it. There is no cache to look in and no shim to fix: the deck for a
// direction nobody has marched has to be marched.
//
// So it is, with `cloud_march` -- the same function, the same decks, the same weather field and the
// same wind as the pass that draws the sky the eye sees, which is what makes this one answer rather
// than a second plausible one. It runs at most ONCE per pixel, on the segment that escapes, and
// only where the reflection was worth casting at all: a scene with nothing polished in it marches
// no clouds, and a scene with an overcast switched off (`push.sky_cloud.x`) marches none either.
//
// # And the sun's DISC is deliberately taken back out
//
// `sky_evaluate` draws the disc so that "a mirror reflects something rather than a flat gradient" --
// its own comment. But R4c's split already puts the sun back through the lobe analytically and
// clamped (`face_lobe_sun`, and the clamp there is the engine's own sun), so returning the disc
// here as well is the same light counted twice, and on a mirror it is the brightest thing in the
// frame -- straight into the light meter and every firefly counter in the store. The environment
// half must therefore be the sky WITHOUT the disc, which is what the split means.
//
// The expression is the one `sky_evaluate` adds, spelled again to subtract it. That is a second
// copy and it should be a `sky_evaluate` that never adds it; the report hands back that patch.
vec3 reflect_escaped(vec3 origin, vec3 dir) {
    vec3 radiance = sky_radiance(dir);
    const float to_sun = dot(dir, trace.sun.xyz);
    if (to_sun > trace.sun.w) {
        radiance -= sun_radiance() * smoothstep(trace.sun.w, mix(trace.sun.w, 1.0, 0.15), to_sun);
    }
    radiance = max(radiance, vec3(0.0));
    if (push.sky_cloud.x > 0.0 && push.r4.w > 0.0) {
        const vec3 from = world_position(origin);
        float through = 1.0;
        vec3 scattered = cloud_march_at(from, dir, world_height_metres(from), kCloudFarMetres,
                                        kReflectCloudSteps, kReflectCloudLightMetres, through);
        // A mean that has had a NaN in it is a NaN for ever, which is what clouds.comp says here.
        if (any(isnan(scattered)) || any(isinf(scattered))) scattered = vec3(0.0);
        if (isnan(through) || isinf(through)) through = 1.0;
        // The composite's own compositing, exactly: `cloud.rgb + cloud.a * colour`.
        radiance = scattered + through * radiance;
    }
    return radiance;
}

// ---- what a surface the ray landed on is giving off ---------------------------------------------
//
// This is `bounce_face_light` and `bounce_radiance` from shaders/shade_faces.comp, and it is a
// SECOND COPY of them, which this file is not allowed to be and says so plainly. Both belong in
// `shaders/face_terms.glsl` -- the file that exists for exactly this, because "the light this frame
// is drawn with and the light the next bounce is computed from cannot drift apart" is its opening
// sentence. They are here because that file was being edited in another worktree on the day this
// was written; the report hands back the patch that moves them.
//
// Until then the rule is: this file may not change what those two functions compute. Any difference
// between them is a bug in this file.
bool reflect_face_ready(uint slot) {
    if (slot == kNoFace || slot == kFaceTombstone) return false;
    if (slot >= faces.items.length()) return false;
    if (face_samples_of(faces.items[slot].counters) < kFaceSettled) return false;
    return slot * kFaceLightWords + kFaceLightWords - 1u < face_light.words.length();
}

// The DIFFUSE half of what one face gives off, read for a ray that landed on `h`.
//
// Diffuse only, and the specular half is the caller's: this function is called once per bounce and
// the caller is the thing that decides whether the specular half continues as another march or is
// taken out of the face's stored average. Splitting them here is what makes the recursion one loop
// instead of two.
vec3 reflect_face_diffuse(uint slot, NodeHit h, uint material) {
    const uint counters = faces.items[slot].counters;
    const uint samples = face_samples_of(counters);
    const uint at = slot * kFaceLightWords;

    const vec3 albedo = vec3(float(h.colour & 0xFFu), float((h.colour >> 8) & 0xFFu),
                             float((h.colour >> 16) & 0xFFu)) * (1.0 / 255.0);
    const vec3 normal = vec3(h.normal);
    const float visible = face_sun_believed(float(face_lit_of(counters)) / float(max(samples, 1u)),
                                            samples, face_light.words[at + kFaceSunStandIn]);
    const float lambert = max(dot(normal, node_push.sun.xyz), 0.0) * visible;

    const uint counts = face_light.words[at];
    const uint near_n = counts & 0xFFFFu;
    const uint far_n = (counts >> 16) & 0xFFFFu;
    float open_sky = 0.0;
    float unoccluded = 1.0;
    if (far_n > 0u) open_sky = float(face_light.words[at + 4u]) / float(far_n);
    if (near_n >= kFaceSettled) {
        const float mean = float(face_light.words[at + 1u]) / (float(near_n) * 255.0);
        unoccluded = 1.0 - clamp(mean, 0.0, 1.0);
    }

    vec3 lamp = vec3(0.0);
    const uint lamp_n = face_light.words[at + 8u] & 0xFFFFu;
    if (lamp_n > 0u) {
        lamp = vec3(uintBitsToFloat(face_light.words[at + 5u]),
                    uintBitsToFloat(face_light.words[at + 6u]),
                    uintBitsToFloat(face_light.words[at + 7u])) / float(lamp_n);
    }

    vec3 bounce = vec3(0.0);
    if (far_n > 0u) {
        bounce = vec3(uintBitsToFloat(face_light.words[at + 9u]),
                      uintBitsToFloat(face_light.words[at + 10u]),
                      uintBitsToFloat(face_light.words[at + 11u]));
    }

    const vec3 direct = face_direct_irradiance(sun_radiance(), lambert, sky_radiance(normal),
                                               face_sky_visibility(open_sky, unoccluded), lamp);
    const vec3 diffuse = (material & kFaceMaterialKnown) != 0u
                             ? albedo * face_diffuse_share(face_material_metal(material))
                             : albedo;
    return face_outgoing_radiance(diffuse, direct, bounce, kFacePi);
}

// The hemispherical mean this face reflects, which is what stands in for the ray when the ray is
// not entitled to the answer. It is the face's own gathered bounce -- the same number R4c calls a
// lobe with one bin in it -- and it is READ rather than measured, so the rough half of the split
// costs nothing at all.
vec3 reflect_face_average(uint slot) {
    const uint at = slot * kFaceLightWords;
    const uint far_n = (face_light.words[at] >> 16) & 0xFFFFu;
    if (far_n == 0u) return vec3(0.0);
    return vec3(uintBitsToFloat(face_light.words[at + 9u]),
                uintBitsToFloat(face_light.words[at + 10u]),
                uintBitsToFloat(face_light.words[at + 11u]));
}

// Which record answers for this hit: the surface's own face, then the coarse faces standing over
// it, then nothing. R9f's search, and the same one `bounce_radiance` runs.
uint reflect_answering_slot(NodeHit h, bool claim, out uint own_slot) {
    own_slot = node_face_lookup(h.face_node, h.face_level, h.face_dir);
    // This ray is integrating that face, so the face is worth measuring. R9b's stamp, before the
    // readiness test rather than after it, because the case it exists for is the one that fails it.
    node_face_gathered(own_slot);
    if (reflect_face_ready(own_slot)) return own_slot;
    // ---- R9a: and if there is no face there at all, ASK for one ------------------------------
    //
    // **This is the difference between a mirror with a picture in it and a mirror with a hole in
    // it, and the hole is what the first build of this stage drew.** A face is claimed where a
    // PIXEL lands, so the store holds exactly what the camera can see -- and the middle band of a
    // sphere seen head on reflects what is BEHIND the camera, which is precisely the set that has
    // no faces. It came back black with a speckle of the few surfaces that happened to be claimed,
    // which is §8 R9's own table saying *a mirror facing a wall behind the camera reflects
    // nothing* with a picture attached.
    //
    // R9a is the mechanism and it was built for this: the ray names the ONE face it landed on down
    // the same feedback channel, tagged secondary, and the host claims it. It is deliberately not
    // "report everything the ray crossed" -- that is the unbounded streaming this rewrite exists to
    // stop -- and the two are told apart by the same distinction R9a draws: a ray names what it
    // LANDED on.
    //
    // Throttled on the caller's side because there is no slot to stamp -- the whole point is that
    // this face is not in the store yet. `claim` is the primary pass's own face lattice, one pixel
    // in `stride` each way, and it is true only for the FIRST reflected segment: a deeper bounce is
    // dimmer by its own throughput and asking for its faces too would multiply the entry count by
    // the depth. D431 is what bounds this: 1,538,219 reports against a capacity of 131,072 took the
    // streaming reports down with them.
    if (claim && own_slot == kNoFace && h.face_level != kNoFaceLevel) {
        node_face_request(h.face_node, h.face_level, h.face_dir);
    }
    for (int step = 1; step <= kFaceAncestorStep; ++step) {
        const uint anc = node_face_lookup(h.face_node >> step, h.face_level + uint(step), h.face_dir);
        if (reflect_face_ready(anc)) return anc;
    }
    return kNoFace;
}

// ---- the continuation itself --------------------------------------------------------------------
//
// What comes back is the environment radiance along the mirror direction at the surface the pixel
// landed on, and the share of the answer it is entitled to be. The composite blends:
//
//     environment = mix(the face's stored average, this, share)
//
// and then puts it through the Fresnel it was already putting the stored average through, so the
// energy split is not touched by any of this and `--no-ior-reflection` is the same frame as before.
//
// # The series
//
// At each surface the radiance leaving it back down the ray is
//
//     L = diffuse + F * ( (1 - share) * stored + share * L_next )
//
// which unrolls to one loop carrying a throughput: `T` starts at one, each bounce adds
// `T * (diffuse + F * (1 - share) * stored)` and then multiplies `T` by `F * share`. A ray that
// escapes adds `T * sky`. That is the whole of the recursion, and the fact that it is a series with
// a factor under one per term is why "infinite" is affordable: the terms die.
struct ReflectAnswer {
    vec3 radiance;
    float share;    // the share the FIRST surface's own ray is entitled to. 0 means "nothing cast"
    uint bounces;   // how many marches were actually run, for the debug view
};

//
// # Two accumulators and not one, and the difference is the composite's own Fresnel
//
// The surface the PIXEL landed on has its Fresnel and its share applied by `resolve.comp`, which
// was already applying them to the stored average. So the first surface's `F * share` belongs in
// the test of whether the march is worth running -- `gate` -- and must NOT reach the radiance this
// returns, or every reflection is squared. `carry` is the same product with that first factor left
// out. They differ by exactly one term and by nothing else.
ReflectAnswer reflect_trace(vec3 eye, vec3 dir, NodeHit first, float pixel_angle, float dither,
                            float budget, bool claim) {
    ReflectAnswer answer;
    answer.radiance = vec3(0.0);
    answer.share = 0.0;
    answer.bounces = 0u;
    if (!(budget > 0.0) || !first.hit || first.face_level == kNoFaceLevel) return answer;

    // The material of the surface the pixel landed on. Its own slot, not whatever record answers
    // for its light -- R9f's division: a stand-in lends a measurement of a place and never an idea
    // of what is there.
    const uint first_slot = node_face_lookup(first.face_node, first.face_level, first.face_dir);
    if (first_slot == kNoFace || first_slot == kFaceTombstone ||
        first_slot >= face_material.words.length()) {
        return answer;
    }
    const uint first_material = face_material.words[first_slot];
    if ((first_material & kFaceMaterialKnown) == 0u) return answer;

    vec3 point = eye + dir * first.t;
    vec3 into = dir;
    vec3 normal = normalize(vec3(first.normal));
    vec3 albedo = vec3(float(first.colour & 0xFFu), float((first.colour >> 8) & 0xFFu),
                       float((first.colour >> 16) & 0xFFu)) * (1.0 / 255.0);
    uint material = first_material;
    uint type_id = first.type_id;
    uint face_level = first.face_level;
    float travelled = first.t;
    // The cone this segment resolves at. It starts as the pixel's and WIDENS by the lobe at every
    // surface, which is the whole of what makes a rough reflection cheap: the marcher's detail
    // clock is a footprint, so a wider cone stops descending sooner, walks fewer nodes and comes
    // back with a folded average instead of a point sample. The blur a rough surface needs is done
    // by the level of detail rather than by a filter or by a second ray.
    float cone = pixel_angle;
    vec3 gate = vec3(1.0);
    vec3 carry = vec3(1.0);
    // What the surface reached by the previous march has already measured for itself, which is the
    // half of its specular the ray is NOT entitled to. Read at the hit and spent at the top of the
    // next turn, where its Fresnel and its share are worked out.
    vec3 stored = vec3(0.0);

    for (uint bounce = 0u; bounce < kReflectBounces; ++bounce) {
        const float lobe = reflect_lobe_width(face_material_rough(material));
        // The angular resolution the stored answer can carry, and so what the ray is measured
        // against. A face is `1 << face_level` voxels across; at `travelled` voxels away that is
        // this many radians. Floored at the cone, which at the first surface is the pixel.
        const float carrier = max(float(1u << face_level) / max(travelled, 1.0), cone);
        const float share = reflect_share(lobe, carrier);
        const vec3 f0 = reflect_f0_of(albedo, face_material_metal(material), type_id);
        const vec3 fresnel = face_fresnel(f0, max(dot(normal, -into), 1e-4));

        if (bounce > 0u) {
            // This surface's specular, the part of it the ray is not entitled to: its own
            // hemispherical mean, weighted by one minus the share. The two halves carry the same
            // Fresnel and complementary shares, so nothing is counted twice and nothing is lost.
            answer.radiance += carry * fresnel * (1.0 - share) * stored;
            carry *= fresnel * share;
        }
        gate *= fresnel * share;

        // What carrying on is worth in the final pixel. The budget is on the CONTRIBUTION and on
        // nothing else -- not on the depth, not on the roughness, not on whether somebody decided
        // the material is a mirror.
        if (max(max(gate.r, gate.g), gate.b) < budget) break;
        // Said only once the first march is going to happen, so that a pixel which casts nothing
        // reports a share of nought -- the composite would otherwise blend its stored average
        // towards a radiance nobody measured, which is black.
        if (bounce == 0u) answer.share = share;

        const vec3 away = reflect(into, normal);
        // D156's nudge, which has to scale with distance: a 32-bit float's own step at 96,000
        // voxels is 0.0156, so a fixed 1e-3 rounds straight back onto the surface it left.
        const vec3 from = point + normal * max(1e-3, travelled * 1e-5);
        cone += lobe;
        const NodeHit h = node_march(from, away, cone, dither, false, false, false, true, false,
                                     kThroughStop, kNodeUnbounded, 0.0);
        answer.bounces += 1u;

        if (!h.hit) {
            answer.radiance += carry * reflect_escaped(from, away);
            break;
        }

        travelled += h.t;
        point = from + away * h.t;
        into = away;
        normal = normalize(vec3(h.normal));
        carry *= h.through;

        uint own = kNoFace;
        const uint slot = (h.face_level == kNoFaceLevel)
                              ? kNoFace
                              : reflect_answering_slot(h, claim && bounce == 0u, own);
        uint next_material = 0u;
        if (own != kNoFace && own != kFaceTombstone && own < face_material.words.length()) {
            next_material = face_material.words[own];
        }
        stored = vec3(0.0);
        if (slot != kNoFace) {
            answer.radiance += carry * reflect_face_diffuse(slot, h, next_material);
            stored = reflect_face_average(slot);
        }

        albedo = vec3(float(h.colour & 0xFFu), float((h.colour >> 8) & 0xFFu),
                      float((h.colour >> 16) & 0xFFu)) * (1.0 / 255.0);
        material = next_material;
        type_id = h.type_id;
        face_level = h.face_level == kNoFaceLevel ? 0u : h.face_level;
        // Nothing is known about what this surface is made of -- a coarse face standing over 512
        // voxels that need not agree, or one that has not looked yet. It cannot continue and it
        // must not be guessed at, so the series ends here with what it has. That errs dark, which
        // is the direction this renderer is required to err in.
        if ((material & kFaceMaterialKnown) == 0u) break;
    }
    return answer;
}

#endif  // WS_REFLECT_GLSL
