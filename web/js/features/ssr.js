// Reflections at run time: screen-space first, a baked probe behind it.
//
// Nothing in this viewer reflected anything. A `mirror` (rough 6, metal 252) came out the colour
// of the sky and so did still water, because the only thing the surface shader had to put in a
// specular lobe was `sky_colour(R)` -- a gradient with a sun in it and no room, no floor and no
// posts. `clips/mirror_test.clip` exists to show that: a polished floor, four coloured posts and
// a chrome ball, and in the picture before this change the floor is a flat white sheet with the
// posts standing on it and nothing underneath them.
//
// # Two halves, and the second one is the important half
//
// **SSR** marches the depth buffer along the reflection vector and reads back the colour the
// scene was already drawn in. It is exact for anything on screen and it cannot see anything that
// is not -- and *a mirror facing you shows mostly what is behind you*, which never is. So every
// ray that leaves the screen, points back at the camera, or finds nothing, falls through to
//
// **the probe**: `ws_probe_radiance(world, direction, roughness)`, a pre-filtered octahedral
// reflection probe the parallel RPRB bake fills in. Until that lands the function in this file
// returns the sky, which is exactly what the viewer reflected before -- so a miss looks like the
// picture always did, and the day the probes arrive that one function is the only thing to
// replace.
//
// # How the scene gets into a texture
//
// The viewer drew straight to the default framebuffer, which has no readable depth and no
// readable colour. So this adds a **scene capture**: one offscreen target, at HALF the canvas's
// pixels, holding the sky and the opaque surfaces, with a depth texture beside it. It is drawn by
// `Renderer.captureScene`, which is the viewer's own sky pass and its own opaque pass pointed
// somewhere else -- not a second description of the scene, which is the failure mode D204 names.
//
// Half resolution is the phone budget: a quarter of the pixels, and a reflection is read through
// a mip chain anyway. It is also why the march is affordable at all -- the depth texture a ray
// walks is a quarter the size.
//
// **The capture is display-space, and a reflection has to be added in radiance.** It is written
// by the same fragment shader that writes the screen, so it comes out tone mapped and gamma
// encoded. `ws_capture_radiance` puts it back: the ACES fit in `tonemap` is a ratio of two
// quadratics and inverts in closed form. The alternative -- an RGBA16F target -- needs
// EXT_color_buffer_float, which a phone may not have, and buys precision only in the highlights
// that clipped to white anyway. Those come back as 7.24, which is bright enough to read as a
// highlight and is the one place this is approximate.
//
// # What the other passes may want from it
//
// The capture is shared plumbing on purpose. `Ssr.colour` is an RGBA8 texture of the opaque scene
// with a full mip chain; `Ssr.depth` is a DEPTH_COMPONENT24 texture of the same; both are the
// current camera at `Ssr.width` x `Ssr.height`. Refraction wants exactly these. A post pass wants
// something different -- the FINAL image, WITH the glass in it -- and should make its own target
// with `makeTarget` rather than borrow this one, because this one has no glass in it.

// The units come from the register, `UNIT` in web/js/gl.js, and not from here: 2, 3, 4 and 5 are
// the irradiance volume, the occlusion atlas, the light list and the material volume.
export const SSR_COLOUR_UNIT = 13;  // UNIT.captureColour -- refraction shares this exact target
export const SSR_DEPTH_UNIT = 14;   // UNIT.captureDepth

// THE PROBE INTERFACE, when web/js/features/probes.js is not there to provide it.
//
// It declares the two functions RPRB's PROBE_GLSL declares, with the same signatures, and
// `probeReflection` returns **coverage zero** -- which is the real "no probe here" answer and
// sends every caller down the same analytic-sky path it will use in the finished build. That is
// the point of writing the fallback this way rather than having it return the sky itself: the
// miss path is exercised, not simulated, so nothing new gets tested for the first time on the day
// the bake lands.
//
// TO WIRE THE REAL ONE: in gl.js, import { PROBE_GLSL, Probes } from './features/probes.js' and
// point WS_PROBE_GLSL at PROBE_GLSL instead of at this. Nothing in this file changes.
export const PROBE_FALLBACK_GLSL = `
vec4 probeReflection(vec3 world, vec3 normal, vec3 reflectDir, float roughness) {
    return vec4(0.0);
}
vec3 probeFresnel(vec3 f0, float ndv, float roughness) {
    return f0 + (max(vec3(1.0 - roughness), f0) - f0) * pow(1.0 - ndv, 5.0);
}
`;

// Everything the surface fragment shader gains, as one string, so that gl.js's own shader is
// edited in one place and this file holds the whole feature.
//
// It goes in AFTER `sky_colour` and `tonemap` (the stub reflects the sky, and the capture has to
// be inverted through the same curve the scene was written with) and BEFORE `main`.
export const SSR_FRAGMENT_GLSL = `
// u_viewProj is NOT declared here. The sun-shadow chunk above already declares it, and one
// shader may declare a uniform once -- the merge found this out as
// "ERROR: 0:519: 'u_viewProj' : redefinition", which is the good failure: the page said so.
uniform sampler2D u_captureColour;
uniform sampler2D u_captureDepth;
uniform float u_ssr;            // 0 off, 1 on
uniform float u_ssrNear;
uniform float u_ssrFar;
uniform float u_ssrLodMax;      // the top mip of the capture, for the roughness blur
uniform float u_ssrReach;       // how far a ray may travel, in metres: the clip's own size

const int WS_SSR_STEPS = 24;
const int WS_SSR_REFINE = 4;
// Above this there is nothing left for a screen-space ray to say that a pre-filtered probe does
// not say better and cheaper, and skipping it is what keeps a building of rough stone free.
const float WS_SSR_ROUGH_MAX = 0.62;
// ...and below this much Fresnel there is nothing to see either. A stone wall head-on reflects
// four per cent, and marching twenty-four steps to fetch a colour that is then multiplied by 0.04
// is the whole facility paying for the salon's two mirrors. Grazing angles still march, because
// grazing is where a floor stops being paint.
const float WS_SSR_MIN_FRESNEL = 0.055;

// The analytic sky, which is what this viewer reflected before there was anything else. It is no
// longer the fallback for everything -- it is the fallback for where the probes do not reach, and
// probeReflection says where that is.
vec3 ws_sky_radiance(vec3 direction, float roughness) {
    vec3 flat_sky = mix(u_skyDown, u_skyUp, clamp(direction.y * 0.5 + 0.5, 0.0, 1.0)) * 0.5;
    return mix(sky_colour(direction), flat_sky, roughness * roughness);
}

// Window depth -> metres along the view axis. The projection in gl.js is the standard one, so
// this is its inverse and nothing else.
float ws_capture_view_depth(float window_z) {
    float ndc = window_z * 2.0 - 1.0;
    return (2.0 * u_ssrNear * u_ssrFar) /
           (u_ssrFar + u_ssrNear - ndc * (u_ssrFar - u_ssrNear));
}

// The capture back into radiance. See the note at the top of ssr.js for why it is stored the way
// it is; this is the closed-form inverse of \`tonemap\` followed by the inverse of the gamma.
vec3 ws_capture_radiance(vec2 uv, float lod) {
    vec3 y = pow(textureLod(u_captureColour, uv, lod).rgb, vec3(2.2));
    vec3 disc = max(vec3(0.0), y * (1.3702 - 1.0127 * y) + 0.0009);
    vec3 x = (vec3(0.03) - 0.59 * y - sqrt(disc)) / (2.0 * (2.43 * y - 2.51));
    return max(x, vec3(0.0)) / max(u_exposure, 1e-4);
}

float ws_hash(vec2 p) {
    return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453);
}

// March the depth texture along R. Returns the radiance found and, through \`weight\`, how much of
// it to believe -- 0 everywhere the march cannot answer, which is where the probe takes over.
//
// A STEP IS A LERP AND NOT A MATRIX MULTIPLY, and that is worth the two lines it takes to set up.
// The projection is affine in world space, so viewProj * (origin + R * t) is exactly
// viewProj * origin + t * viewProj * R -- one mat4 multiply for each end and then four multiplies
// and four adds per step. Written the obvious way, projecting every sample point, twenty-eight
// steps and five refinements are thirty-three matrix multiplies a fragment; measured on
// mirror_test that way, the frame went from 197 ms to 1156 ms in SwiftShader, and this brings the
// same picture back for a third of it.
vec3 ws_screen_reflection(vec3 origin, vec3 R, float rough, out float weight) {
    weight = 0.0;
    vec4 head = u_viewProj * vec4(origin, 1.0);
    vec4 step_clip = u_viewProj * vec4(R, 0.0);         // one metre along the ray, in clip space

    // Steps that grow: fine where the ray leaves the surface and coarse where it is far away,
    // which is the same distribution the perspective divide has already put on the depth buffer.
    float stride = max(u_ssrReach * 0.004, 0.01);
    // A per-pixel offset into the first step, or every stride lands on the same plane across the
    // whole surface and the reflection comes out in bands.
    float travel = stride * (0.6 + ws_hash(gl_FragCoord.xy) * 0.8);

    bool hit = false;
    float before = 0.0;
    float after = 0.0;
    vec2 uv = vec2(0.0);
    for (int i = 0; i < WS_SSR_STEPS; ++i) {
        vec4 at = head + step_clip * travel;
        if (at.w <= 0.0) break;                                     // gone behind the eye
        vec2 screen = at.xy / at.w * 0.5 + 0.5;
        if (any(lessThan(screen, vec2(-0.05))) ||
            any(greaterThan(screen, vec2(1.05)))) break;            // gone off the screen
        float scene = ws_capture_view_depth(texture(u_captureDepth, clamp(screen, 0.0, 1.0)).r);
        float behind = at.w - scene;
        // Behind what is drawn, but not so far behind that the ray has passed clean through a
        // post and out the other side. The tolerance is the step, because a coarse step is
        // allowed to overshoot by as much as it is long and a fine one is not.
        if (behind > 0.0 && behind < stride * 2.0 + u_ssrReach * 0.008) {
            hit = true;
            after = travel;
            uv = screen;
            break;
        }
        before = travel;
        travel += stride;
        stride *= 1.13;
    }
    if (!hit) return vec3(0.0);

    // Halve the interval that straddles the surface. Four of these turn a step that may be a
    // metre long into a hit good to a few centimetres, which is the difference between a
    // reflection that slides as you move and one that stays put.
    for (int i = 0; i < WS_SSR_REFINE; ++i) {
        float middle = (before + after) * 0.5;
        vec4 at = head + step_clip * middle;
        vec2 screen = at.xy / at.w * 0.5 + 0.5;
        float scene = ws_capture_view_depth(texture(u_captureDepth, clamp(screen, 0.0, 1.0)).r);
        if (at.w - scene > 0.0) { after = middle; uv = screen; } else { before = middle; }
    }

    uv = clamp(uv, 0.0, 1.0);
    // Fade at the edge of the screen, where the reflection of a thing runs out at the same moment
    // the thing itself does and a hard border is the giveaway.
    vec2 edge = smoothstep(vec2(0.0), vec2(0.10), uv) *
                smoothstep(vec2(0.0), vec2(0.10), 1.0 - uv);
    weight = edge.x * edge.y;
    // And fade where the ray points back at the camera: that asks for pixels behind the eye,
    // which are in no buffer. It is exactly the mirror-facing-you case, and it is the probe's.
    weight *= 1.0 - smoothstep(0.15, 0.65, dot(R, normalize(u_eye - origin)));

    // Rougher surfaces read a blurrier mip rather than firing more rays. No jitter beyond the
    // one on the first step, because there is no temporal filter here to resolve noise into an
    // image -- a blurred fetch is stable and a stochastic one would crawl.
    float lod = clamp(rough * 2.4 * u_ssrLodMax, 0.0, u_ssrLodMax);
    return ws_capture_radiance(uv, lod);
}

// What the specular lobe reflects: the room if the screen has it, the probe if it does not, and
// the analytic sky where there are no probes either.
//
// \`f0\` and \`ndv\` are here to decide whether to march at all, not to shade with -- the Fresnel
// itself is applied by the caller with gl.js's own Schlick, where it always was. probeFresnel is
// deliberately NOT called: two Fresnels would multiply.
//
// \`rough\` is the GGX-clamped roughness this file marches and blurs with; \`rawRough\` is the
// material's own byte, which is what the probe wants for its pre-filtered level.
// The coverage comes back out because the caller needs it and cannot cheaply ask again: the
// sky-visibility term that stands in for a probe has to be faded out exactly as far as a probe
// actually covers the point, or an interior reflection is darkened twice and a mirror in a hall
// goes black. That is the probe branch's finding and it survives here.
vec3 ws_reflected_radiance(vec3 world, vec3 N, float rough, float rawRough, vec3 f0, float ndv,
                           out float coverage) {
    vec3 R = reflect(normalize(world - u_eye), N);
    // R goes in UNCORRECTED. probeReflection applies its own parallax correction, and correcting
    // twice bends the reflection off the wall it belongs to.
    vec4 probe = probeReflection(world, N, R, rawRough);
    coverage = clamp(probe.a, 0.0, 1.0);
    // .a is coverage -- how much of this point's probe neighbourhood actually had probes baked.
    // Zero is not "black", it is "nobody looked here", and the sky is the honest answer. Mixed
    // rather than branched, so the edge of a probe volume is a fade and not a seam.
    vec3 fallback = mix(ws_sky_radiance(R, rough), probe.rgb, clamp(probe.a, 0.0, 1.0));
    if (u_ssr < 0.5 || rough > WS_SSR_ROUGH_MAX) return fallback;
    vec3 f = fresnel(f0, ndv);
    if (max(f.r, max(f.g, f.b)) < WS_SSR_MIN_FRESNEL) return fallback;
    float weight = 0.0;
    // Started a little off the surface, or the first step reads the surface's own depth and every
    // polished floor reflects itself.
    vec3 found = ws_screen_reflection(world + N * max(u_ssrReach * 0.002, 0.01), R, rough, weight);
    weight *= 1.0 - smoothstep(WS_SSR_ROUGH_MAX * 0.55, WS_SSR_ROUGH_MAX, rough);
    return mix(fallback, found, clamp(weight, 0.0, 1.0));
}
`;

// Round a pixel count so that a canvas drifting under the adaptive-resolution loop does not
// reallocate two textures and a mip chain every frame.
function quantise(value) {
    return Math.max(8, Math.round(value / 16) * 16);
}

// One offscreen target: an RGBA8 colour texture with a mip chain, and a depth texture. Public so
// that a pass wanting a different one (a post chain wants the FINAL image at FULL size) makes its
// own rather than borrowing this one.
export function makeTarget(gl, width, height, wantMips) {
    const colour = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, colour);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA8, width, height, 0, gl.RGBA, gl.UNSIGNED_BYTE, null);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER,
                     wantMips ? gl.LINEAR_MIPMAP_LINEAR : gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    if (wantMips) gl.generateMipmap(gl.TEXTURE_2D);

    const depth = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, depth);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.DEPTH_COMPONENT24, width, height, 0, gl.DEPTH_COMPONENT,
                  gl.UNSIGNED_INT, null);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_COMPARE_MODE, gl.NONE);

    const frame = gl.createFramebuffer();
    gl.bindFramebuffer(gl.FRAMEBUFFER, frame);
    gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, colour, 0);
    gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.DEPTH_ATTACHMENT, gl.TEXTURE_2D, depth, 0);
    const status = gl.checkFramebufferStatus(gl.FRAMEBUFFER);
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    gl.bindTexture(gl.TEXTURE_2D, null);
    return { colour, depth, frame, width, height, complete: status === gl.FRAMEBUFFER_COMPLETE };
}

export class Ssr {
    constructor(gl) {
        this.gl = gl;
        // Off with `?ssr=0`, which is the control arm and does not need a rebuild to take.
        const asked = new URLSearchParams(location.search).get('ssr');
        this.enabled = asked !== '0' && asked !== 'off';
        // FULL RESOLUTION, and this was settled at the merge rather than here.
        //
        // Half the canvas was right for a reflection and only for a reflection: a reflection is
        // read through a mip chain and does not care. Refraction takes this same target -- one
        // target, one pass, no second capture -- and a look straight through nearly-clear glass
        // is a DIRECT view, not a blurred one. On `glass_test` the wall seen through a clear pane
        // came out with staircased edges the same wall beside the pane did not have. Half
        // resolution saves a quarter of the pixels on one pass and costs a visible artefact on
        // every pane in the building.
        //
        // SSR reads a mip of this and is unaffected; refraction reads level 0. `?ssr=half` is the
        // arm that puts the halving back, for measuring what it was worth.
        this.scale = asked === 'half' ? 0.5 : 1.0;
        // `?ssr=capture` captures the scene and then does not march it, which is the only way to
        // say how much of the frame is the extra pass and how much is the rays.
        this.march = asked !== 'capture';
        this.target = null;
        this.width = 0;
        this.height = 0;
        this.lodMax = 0;
        this.captured = false;
        this.broken = false;
    }

    get colour() { return this.target ? this.target.colour : null; }
    get depth() { return this.target ? this.target.depth : null; }

    // Size the target to the canvas and bind it. Returns false when there is nothing to capture
    // into, and then everything downstream falls back to the probe on its own.
    begin(canvasWidth, canvasHeight) {
        const gl = this.gl;
        this.captured = false;
        if (!this.enabled || this.broken) return false;
        const width = quantise(canvasWidth * this.scale);
        const height = quantise(canvasHeight * this.scale);
        if (!this.target || this.width !== width || this.height !== height) {
            this.dispose();
            this.target = makeTarget(gl, width, height, true);
            if (!this.target.complete) {
                // A card that cannot give a depth texture is a card that gets the sky, and says so
                // once rather than every frame.
                console.warn('ssr: no offscreen depth target, reflections fall back to the probe');
                this.broken = true;
                this.dispose();
                return false;
            }
            this.width = width;
            this.height = height;
            this.lodMax = Math.floor(Math.log2(Math.max(width, height)));
        }
        // The capture is about to be written, so nothing may be sampling it.
        gl.activeTexture(gl.TEXTURE0 + SSR_COLOUR_UNIT);
        gl.bindTexture(gl.TEXTURE_2D, null);
        gl.activeTexture(gl.TEXTURE0 + SSR_DEPTH_UNIT);
        gl.bindTexture(gl.TEXTURE_2D, null);
        gl.bindFramebuffer(gl.FRAMEBUFFER, this.target.frame);
        gl.viewport(0, 0, width, height);
        return true;
    }

    // Back to the screen, and build the mip chain the roughness blur reads.
    end(canvasWidth, canvasHeight) {
        const gl = this.gl;
        gl.bindFramebuffer(gl.FRAMEBUFFER, null);
        gl.viewport(0, 0, canvasWidth, canvasHeight);
        gl.activeTexture(gl.TEXTURE0 + SSR_COLOUR_UNIT);
        gl.bindTexture(gl.TEXTURE_2D, this.target.colour);
        gl.generateMipmap(gl.TEXTURE_2D);
        this.captured = true;
    }

    // Everything the surface shader needs to march. Called with `captured` false in the capture
    // pass itself, which is what keeps a mirror inside a mirror to one bounce.
    bind(uniforms, camera, clip) {
        const gl = this.gl;
        const on = this.captured && this.enabled && !this.broken && this.march;
        gl.uniform1f(uniforms.u_ssr, on ? 1 : 0);
        if (!on) return;
        gl.activeTexture(gl.TEXTURE0 + SSR_COLOUR_UNIT);
        gl.bindTexture(gl.TEXTURE_2D, this.target.colour);
        gl.uniform1i(uniforms.u_captureColour, SSR_COLOUR_UNIT);
        gl.activeTexture(gl.TEXTURE0 + SSR_DEPTH_UNIT);
        gl.bindTexture(gl.TEXTURE_2D, this.target.depth);
        gl.uniform1i(uniforms.u_captureDepth, SSR_DEPTH_UNIT);
        gl.uniform1f(uniforms.u_ssrNear, camera.near);
        gl.uniform1f(uniforms.u_ssrFar, camera.far);
        gl.uniform1f(uniforms.u_ssrLodMax, this.lodMax);
        // The clip's own size, so a ray in a 12 m room and a ray in a 290 m tower both step in
        // proportion to what they are crossing rather than to a constant somebody picked once.
        gl.uniform1f(uniforms.u_ssrReach, Math.min(clip && clip.reach ? clip.reach : 20, 120));
    }

    dispose() {
        const gl = this.gl;
        if (!this.target) return;
        gl.deleteFramebuffer(this.target.frame);
        gl.deleteTexture(this.target.colour);
        gl.deleteTexture(this.target.depth);
        this.target = null;
        this.width = 0;
        this.height = 0;
        this.captured = false;
    }
}
