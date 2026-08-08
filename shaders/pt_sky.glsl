// The sky, and the sun's disc in it.
//
// What a ray sees when it hits nothing, and the only light in most scenes. Its own module
// because a real sky model - turbidity, a sun that sets, a night, cloud - is a large piece of
// work that has nothing to do with anything else in the renderer.

// Light and materials

vec3 sky_radiance(vec3 dir) {
    float up = clamp(dir.y * 0.5 + 0.5, 0.0, 1.0);
    vec3 horizon = vec3(0.62, 0.68, 0.78);
    vec3 zenith = vec3(0.18, 0.32, 0.62);
    vec3 ground = vec3(0.16, 0.15, 0.14);
    // Scaled well below the sun. Those colours were written to be looked at directly, and
    // used unchanged as *radiance* they light the whole hemisphere at nearly the brightness of
    // the sun itself ÃƒÆ’Ã†â€™Ãƒâ€šÃ‚Â¢ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â€šÂ¬Ã…Â¡Ãƒâ€šÃ‚Â¬ÃƒÆ’Ã‚Â¢ÃƒÂ¢Ã¢â‚¬Å¡Ã‚Â¬Ãƒâ€šÃ‚Â every surface gets the same flat fill from every direction, shadows
    // stop reading, and the result is the grey wash a first attempt at this always produces.
    // Real daylight has the sun an order of magnitude above the sky it shares the frame with.
    vec3 base = ((dir.y < 0.0) ? mix(ground, horizon, up * 2.0)
                               : mix(horizon, zenith, up * 2.0 - 1.0)) * 0.25;
    // The sun's own disc, so a mirror reflects something rather than a flat gradient.
    if (dot(dir, trace.sun.xyz) > trace.sun.w) return base + trace.sun_colour.rgb;
    return base;
}
