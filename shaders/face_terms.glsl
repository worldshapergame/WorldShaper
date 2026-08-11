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
const uint kFaceLightWords = 12u;

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
#endif  // WS_FACE_TERMS_GLSL
