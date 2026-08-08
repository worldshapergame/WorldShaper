// Everything between a radiance value and a pixel on the screen.
//
// Its own module because it is a different subject from light transport: exposure, tone mapping,
// glare, the lens. Transport answers "how much light arrives"; this answers "what should that
// look like", and the two are argued about on completely different grounds.
// What the primary ray picked up, and lost, before it reached the surface it shades.
//
// A ray that passes through glass is shaded at whatever is *behind* the glass, and everything
// below this point in the shader is written as though it were shading what the eye sees
// directly. Rather than thread a tint through every one of the dozen places that writes a
// pixel, the glass leaves its mark here: what it added on the way (its own reflection) and what
// fraction of the light behind it survives (transmission, times absorption over the distance
// travelled inside). write_pixel applies both, once, for all of them.
vec3 g_prefix = vec3(0.0);
vec3 g_throughput = vec3(1.0);

void write_pixel(ivec2 pixel, uint sample_index, vec3 radiance_in, float primary_t, float aspect) {
    vec3 radiance = g_prefix + g_throughput * radiance_in;
    vec4 total = (sample_index == 0u) ? vec4(0.0) : imageLoad(accum, pixel);
    if (total.w >= kAccumWindow) {
        total *= (kAccumWindow - 1.0) / kAccumWindow;
    }
    // Geometry arriving or leaving resets this outright, on the CPU side, by zeroing the
    // sample index. Halving it instead was tried, and it is the better-looking option Ã¢â‚¬â€ the
    // room scene's speckle went from 7.3 to 4.7, because during streaming chunks arrive on
    // almost every frame and a reset means the average never gets past one sample.
    //
    // It is not the correct option. Halving leaves a residue that nothing clears once
    // streaming stops, and a sealed box with no lights in it settled at a mean of 2.9 instead
    // of zero. A room with no way into it has to be black, so the reset stays and the noise
    // during streaming is the price.
    total.rgb += radiance;
    total.w += 1.0;
    imageStore(accum, pixel, total);

    vec3 mean = total.rgb / max(total.w, 1.0);

    // Reinhard with a shoulder, and gamma. Deliberately plain: the point is to see what the
    // material does, not what a grading curve does to it. The physical post stack is Stage 9.
    vec3 mapped = pow(clamp(mean / (mean + vec3(1.0)), 0.0, 1.0), vec3(1.0 / 2.2));

    // The tools, over the top, exactly as the real-time pipeline draws them. After tone mapping
    // and outside the accumulation, because a preview is interface rather than light: it must
    // not be averaged in, must not converge, and must not be dimmed by an exposure curve.
    // Unjittered, so an outline is a line rather than a smeared band.
    vec2 uv = (vec2(pixel) + 0.5) / vec2(push.resolution) * 2.0 - 1.0;
    vec3 straight = normalize(push.forward.xyz + push.right.xyz * uv.x * push.lens.x * aspect -
                              push.up.xyz * uv.y * push.lens.x);
    mapped = draw_preview(mapped, push.origin.xyz, straight, primary_t);

    imageStore(out_colour, pixel, vec4(mapped, 1.0));
