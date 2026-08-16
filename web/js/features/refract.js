// Glass, refraction, translucency and coloured volumes.
//
// The clips have declared four things about transparent matter since the format was written and
// the viewer drew none of them: `ior` (glass 1.5, water 1.33, crystal 1.62), `absorb`
// (Beer-Lambert per metre), `translucent` (alabaster 210, onyx 160, wax 150) and `opacity`. What
// was on screen was a blended tint, which is a coloured SURFACE — and `clips/facility/_contract.clip`
// says in as many words that the whole point of `absorb` is that a stained window is a coloured
// VOLUME rather than a coloured surface: a 12 mm pane is a tint and the same glass 300 mm deep is
// a colour.
//
// # What this is, and what it is not
//
// **The refraction is screen-space and it is an approximation.** There are no rays here. The
// surface takes the picture of the scene WITHOUT any glass in it and samples that at an offset,
// and the offset is the refracted vector carried across the material's own thickness and projected
// back to the screen. That is right for a phone and it is wrong in two ways that are worth saying
// out loud rather than discovering:
//
//   - it can only show what is ON SCREEN. A pane at the edge of the view refracts what is beside
//     it in the frame, not what is beside it in the world, and the sample is clamped rather than
//     wrapped so the edge smears instead of tiling.
//   - the picture it samples has no transparent surface in it, so glass behind glass shows the
//     stone behind both rather than the near pane's own tint.
//
// The third fault a screen-space sample usually has — pulling in something that stands IN FRONT of
// the refractor — is gone when the capture carries depth, which the shared scene target does: the
// sample is refused where what it lands on is nearer than the glass, and the pixel straight behind
// is used instead. Without depth the offset clamp bounds it and nothing removes it.
//
// **The absorption is not an approximation.** exp(-absorb * path) over a real path length is what
// `shaders/node.glsl:node_medium_absorb` does in the game, in the same units — the byte is
// sixteenths per metre — and the path is the thickness crossed at the refracted angle rather than
// the thickness itself, so a slanted look through a pane is deeper in colour than a square one.
// Everything therefore turns on knowing the thickness, which is why `refract_thickness` below is
// the one dependency this file has.
//
// # Where the picture behind the glass comes from, and why the encoding matters
//
// **It does not own a target.** `capture` takes whatever scene capture the renderer already has —
// `features/ssr.js`'s `Ssr`, which draws sky and opaque into one offscreen target at half the
// canvas and hands back `colour`, `depth`, `captured` — and uses that. Sky and opaque with no glass
// and no cut cap is exactly what belongs behind a pane, and taking it makes refraction cost no
// pass of its own at all. Only when there is no such capture does it fall back to copying the
// framebuffer itself, and that fallback is a `copyTexSubImage2D` and still not a second target.
//
// **That capture is display-space — tonemapped and gamma-encoded — and Beer-Lambert does not
// multiply in display space.** Transmittance attenuates RADIANCE, so the encoding has to come off
// before the tint goes on and back on afterwards, or a stained window over a sunlit wall comes out
// far too saturated: ACES has already compressed that wall towards white, and multiplying the
// compressed value by exp(-k d) takes the colour out of a number the curve has already flattened.
// Measured on a white wall at radiance 3 behind ruby glass, the two routes differ by about a fifth
// of the green and blue channels. `refract_scene_radiance` therefore inverts both in closed form —
// the same closed form as `ws_capture_radiance` in ssr.js, and when these two files meet, one of
// them should be deleted in favour of the other.
//
// **A float target is not needed for this.** The inverse is exact and bounded (white decodes to
// about 7 in radiance), and its only real cost is that 8 bits of an ACES-compressed highlight
// carry fewer stops than 8 bits of a midtone, so a very bright background seen through strongly
// absorbing glass can band. Nothing in the facility does that. `RGBA16F` would remove it and needs
// `EXT_color_buffer_float`, which is exactly the extension a phone may not have.
//
// # Cost
//
// With the shared capture: no extra pass, and per glass pixel one `refract`, one matrix multiply
// and two texture fetches (colour and depth). Without it: one full-screen `copyTexSubImage2D` per
// frame, and only for a clip that has a material with both an `ior` and an `opacity` — `wantedFor`
// decides that once, at load. There is no loop over layers anywhere in here, which is the thing a
// phone cannot afford.

// How thick a transparent or translucent surface is, in metres, until the thickness field lands.
//
// 0.12 is the facility's own glazing and `clips/refraction_small.clip` says so in its own comment
// ("Twelve centimetres, which is the facility's own glazing, so the offset here is the offset
// there"). It is a stand-in and it is wrong for the water in `glass_test`, which is three times it.
export const STUB_THICKNESS = 0.12;

// How far, as a fraction of the screen, a refracted sample may reach from the pixel that asks for
// it. This is the whole of what bounds the artefact described above: an offset of a few percent of
// the screen is a few pixels of smear at worst and a visible bend at best, and one of a third of
// the screen is a pane with somebody else's furniture in it.
export const MAX_OFFSET = 0.06;

// The GLSL. Three blocks, inserted into the surface fragment shader at three marked points in
// web/js/gl.js, so that nothing here has to know how that shader is assembled.

// 1. Uniforms, the thickness stub, and the composite itself.
export const GLSL_DECLARATIONS = `
uniform sampler2D u_behind;      // the scene with no glass in it, tonemapped and gamma-encoded
uniform sampler2D u_behindDepth; // its depth, when the capture has one
uniform float u_behindHasDepth;  // 1 when it does, and then the sample can be refused
uniform vec2 u_behindScale;      // viewport / texture size, when the capture is not the whole one
uniform vec2 u_viewportInv;      // 1 / viewport in pixels, to turn gl_FragCoord into a uv
uniform float u_refract;         // 0 is the control arm: no offset, no fetch, no copy
uniform float u_thickness;       // metres of matter behind a surface, until THCK bakes the field
uniform float u_maxOffset;       // how far a refracted sample may reach, in screen fractions
uniform mat4 u_viewProj;         // the same matrix the vertex stage uses, to project the exit point

// Defined further down the shader this block is spliced into. Declared rather than moved, so that
// this file can go in anywhere and does not care what order the other features arrive in.
vec3 tonemap(vec3 x);

// HOW THICK IS THE MATTER BEHIND THIS SURFACE?
//
// Without an answer \`absorb\` cannot mean anything at all — Beer-Lambert is exp(-k * distance) and
// a rasteriser has no distance — which is why this is the one thing in this file that comes from
// somewhere else. A parallel pass (fourcc THCK, tools/bake/matvol.hpp and
// web/js/features/matvol.js) is baking a thickness field and will expose a GLSL sampler for it.
//
// THE SIGNATURE ASSUMED HERE:
//
//     float matvol_thickness(vec3 world, vec3 direction);
//
// metres of matter from \`world\` along \`direction\` before it leaves the material. World space,
// metres out, direction normalised, no side effects.
//
// Until it lands this returns a constant, and it is ONE LINE to replace.
float refract_thickness(vec3 world, vec3 direction) {
    return u_thickness;   // <-- becomes: return matvol_thickness(world, direction);
}

// The absorption bytes as a coefficient per metre, in the game's own units. \`absorb=6,1,4\` on the
// green glass is six units of red taken out per metre and a unit is a sixteenth, which is exactly
// what shaders/node.glsl:node_medium_absorb reads. The bytes arrive here normalised by the texture
// fetch, so the 255 puts them back.
vec3 refract_absorption(vec3 absorbByte) {
    return absorbByte * (255.0 / 16.0);
}

// The capture, back to radiance. The scene target holds a tonemapped, gamma-encoded picture —
// see the note at the top of this file for why a float one is not wanted — and transmittance
// multiplies radiance, so both have to come off before the tint goes on.
//
// This is the closed-form inverse of \`tonemap\`: the ACES fit is a ratio of two quadratics, so
// solving y = f(x) for x is one square root. It is the same function as \`ws_capture_radiance\` in
// features/ssr.js and the two should become one when those files meet.
vec3 refract_scene_radiance(vec2 uv) {
    vec3 y = pow(texture(u_behind, uv).rgb, vec3(2.2));
    vec3 disc = max(vec3(0.0), y * (1.3702 - 1.0127 * y) + 0.0009);
    vec3 x = (vec3(0.03) - 0.59 * y - sqrt(disc)) / (2.0 * (2.43 * y - 2.51));
    return max(x, vec3(0.0)) / max(u_exposure, 1e-4);
}

// What a transparent surface finally is: what is behind it, bent and absorbed, with its own lit
// surface over the top in the proportion its opacity and its angle ask for.
//
// \`surface\` arrives already tonemapped and encoded, because it is the same colour the opaque pass
// would have written. What comes back through the glass makes the round trip instead — decoded to
// radiance, attenuated, tonemapped again — for the reason at the top of this file.
vec4 refract_composite(vec3 surface, float opacity, vec3 world, vec3 N, vec3 V,
                       float iorByte, vec3 absorbByte, float ndv) {
    // Glancing angles of a pane are more reflective and less see-through, which is the one thing
    // about glass everybody notices when it is missing.
    float alpha = clamp(opacity + (1.0 - opacity) * pow(1.0 - ndv, 4.0), 0.0, 1.0);
    vec3 k = refract_absorption(absorbByte);

    // Nothing to bend with, or the control arm: the old blend, and what is behind is whatever the
    // blender has already put in the framebuffer. The tint is a real Beer-Lambert over the
    // thickness rather than a fraction of the byte, and it goes on in light like the other one.
    if (u_refract < 0.5 || iorByte < 0.5) {
        vec3 tint = exp(-k * refract_thickness(world, -N));
        return vec4(pow(pow(surface, vec3(2.2)) * tint, vec3(1.0 / 2.2)), alpha);
    }

    float ior = 1.0 + iorByte / 128.0;      // as VisualRecord stores it: 0 is vacuum, 64 is 1.5
    vec3 into = refract(-V, N, 1.0 / ior);
    // Total internal reflection at the entry face cannot happen coming out of air, but a normal
    // that has been flipped at a grazing angle can produce it; carry straight on rather than drop
    // the pixel.
    if (dot(into, into) < 1e-6) into = -V;

    float thickness = refract_thickness(world, into);
    // A slab crossed at an angle is a longer path than the slab is thick, which is the whole of
    // why a stained light reads deeper at its edges. Clamped, because at true grazing the path
    // goes to infinity and the pane goes black.
    float path = thickness / max(dot(into, -N), 0.35);

    // Where the ray leaves the matter, projected back to the screen. This is the approximation:
    // the offset is the exit POINT rather than where the exiting ray finally lands, which is what
    // a rasteriser can afford and is what everything on a phone does. A material that does not
    // bend gives an exit point further along the eye ray, which projects to the same pixel, so
    // the offset is purely the bend and nothing else.
    vec2 here = gl_FragCoord.xy * u_viewportInv;
    vec4 exitClip = u_viewProj * vec4(world + into * path, 1.0);
    vec2 there = (exitClip.xy / max(abs(exitClip.w), 1e-4) * sign(exitClip.w)) * 0.5 + 0.5;
    vec2 delta = there - here;
    float reach = length(delta);
    if (reach > u_maxOffset) delta *= u_maxOffset / reach;
    vec2 uv = clamp(here + delta, vec2(0.0), vec2(1.0)) * u_behindScale;

    // THE ONE THING A SCREEN-SPACE SAMPLE CANNOT ARGUE WITH, when the capture carries depth: the
    // offset may land on something standing IN FRONT of the glass, and drawing that inside the
    // pane is the artefact everybody recognises. Both depths are window space in the same
    // projection -- the capture is drawn with the same camera and the same clip plane -- so it is
    // one compare, and the answer when it fails is the pixel straight behind.
    if (u_behindHasDepth > 0.5 && texture(u_behindDepth, uv).r < gl_FragCoord.z) {
        uv = here * u_behindScale;
    }

    vec3 behind = refract_scene_radiance(uv);   // out of the encoding, into light
    behind *= exp(-k * path);                   // Beer-Lambert, over the path actually crossed
    vec3 encoded = pow(tonemap(behind * u_exposure), vec3(1.0 / 2.2));
    return vec4(mix(encoded, surface, alpha), 1.0);
}
`;

// 2. Translucency: stone you can see INTO. Replaces the one-line wrap that was there, which read
//    the light in FRONT of the surface and was therefore nought in exactly the arrangement it
//    existed for.
export const GLSL_TRANSLUCENCY = `
    // Light that goes in one side of a solid and comes out of the other. Two terms and no rays.
    //
    // THE BACK-LIT TERM is the one that matters and the one that was missing. A taper behind an
    // alabaster pilaster, the sun behind a marble panel: what decides whether it shows is how much
    // light is on the FAR side and how much of it survives the thickness. The far side is read out
    // of the same baked light volume every other surface here reads, one fetch, at a point behind
    // the surface rather than in front of it. What was here sampled sunVisible in front, which is
    // nought when the sun is behind — so the term died exactly when it was wanted.
    //
    // How deep light gets is the game's own number: kTranslucentVoxels is 6 voxels at 32 to the
    // metre, squared in the byte, so marble at 110 reaches under a voxel and stays stone while
    // alabaster at 210 reaches four and a thin panel of it lights up.
    if (translucency > 0.0) {
        float reach = max(0.1875 * translucency * translucency, 1e-3);
        float thick = refract_thickness(v_world, -N);
        float carried = exp(-thick / reach);

        vec3 behindAt = (v_world - N * max(thick, u_lightBias) - u_lightOrigin) * u_lightScale;
        behindAt = clamp(behindAt + u_lightTexel, vec3(0.0), vec3(1.0));
        vec2 beyond = texture(u_light, behindAt).rg;

        vec3 through = u_sunColour * max(dot(-N, L), 0.0) * beyond.r + ambient * beyond.g * 0.5;
        diffuse += albedo * translucency * carried * through;

        // ...and the wrap. A surface that scatters light inside itself is still lit a little way
        // past its own terminator, which is what stops thin stone having the hard shadow line of a
        // painted one. Only the EXCESS over the Lambert term already added, so nothing is counted
        // twice.
        float soft = translucency * 0.5;
        float wrapped = clamp((dot(N, L) + soft) / (1.0 + soft), 0.0, 1.0);
        diffuse += albedo * translucency * 0.35 * max(wrapped - ndl, 0.0) * u_sunColour * sunVisible;
    }
`;

// 3. The composite call, where the old flat tint and the old alpha were.
export const GLSL_COMPOSITE = `
    if (alpha < 1.0) {
        o_colour = refract_composite(encoded, alpha, v_world, N, V, iorByte, depth.rgb, ndv);
        return;
    }
`;

function roundUp(value, step) {
    return Math.max(step, Math.ceil(value / step) * step);
}

// Does this clip have anything that refracts? A material with an index of refraction AND something
// less than fully opaque. Asked once at load, because the answer decides whether a frame pays for
// the copy at all.
export function wantedFor(clip) {
    for (let m = 0; m < clip.materialCount; ++m) {
        const at = m * 16;
        if (clip.materials[at + 6] > 0 && clip.materials[at + 3] < 255) return true;
    }
    return false;
}

export class Refraction {
    constructor(gl) {
        this.gl = gl;
        this.texture = gl.createTexture();
        this.width = 0;
        this.height = 0;
        this.viewWidth = 1;
        this.viewHeight = 1;
        this.wanted = false;
        // The control arm, and it is a real one: `?refract=0` takes the copy, the offset and the
        // fetch out and leaves everything else standing, which is what a frame-cost figure has to
        // be measured against.
        this.enabled = !/[?&]refract=0/.test(
            (typeof location !== 'undefined' && location.search) || '');
        this.thickness = STUB_THICKNESS;
        this.maxOffset = MAX_OFFSET;
        // 0 is the materials and 1 is the light volume; features/ssr.js has claimed 2 and 3 for
        // the scene capture it owns. These two are this file's, and they hold either that capture
        // or the fallback copy — whichever `capture` found.
        this.colourUnit = 4;
        this.depthUnit = 5;
        this.scene = null;      // the shared capture, when there is one
        this.allocate(1, 1);
    }

    allocate(width, height) {
        const gl = this.gl;
        gl.activeTexture(gl.TEXTURE0 + this.colourUnit);
        gl.bindTexture(gl.TEXTURE_2D, this.texture);
        // RGB8 AND NOT RGBA8, and it is not a preference. A copy out of a framebuffer may only
        // ask for components the framebuffer has, and the canvas is made with `alpha: false` — so
        // an RGBA destination is "invalid copy texture format combination" on every frame and the
        // texture stays black. RGB is also a subset of any offscreen target somebody else adds,
        // which is the other reason to want it.
        gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGB8, width, height, 0, gl.RGB, gl.UNSIGNED_BYTE, null);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
        this.width = width;
        this.height = height;
        gl.activeTexture(gl.TEXTURE0);
    }

    setClip(clip) {
        this.wanted = clip ? wantedFor(clip) : false;
    }

    // Find the picture of the scene with no glass in it.
    //
    // FIRST CHOICE IS SOMEBODY ELSE'S, and that is the whole design. `scene` is the renderer's
    // shared capture -- features/ssr.js's `Ssr`, or anything with the same four properties:
    // `captured`, `colour`, `depth`, and a size. It holds sky and opaque drawn with this frame's
    // camera and this frame's clip plane and nothing transparent, which is exactly what belongs
    // behind a pane, and taking it means refraction costs no pass of its own. It also carries
    // depth, which is what lets a sample be refused when it lands on something in front.
    //
    // FALLBACK, when there is no such capture: copy the framebuffer. `copyTexSubImage2D` reads
    // whatever framebuffer is bound, so on the canvas it takes the canvas with its multisampling
    // resolved on the way and costs the picture no antialiasing. There is no depth to be had that
    // way, so the guard turns itself off and the offset clamp is all that bounds the artefact.
    //
    // The fallback texture is rounded up to a multiple of 64 and the viewport is copied into the
    // corner of it, so the adaptive resolution in app.js -- which moves the canvas by a percent at
    // a time -- reallocates rarely instead of every frame. `u_behindScale` carries the difference.
    capture(width, height, scene) {
        const gl = this.gl;
        this.scene = null;
        if (!this.enabled || !this.wanted) return false;
        if (scene && scene.captured && scene.colour) {
            this.scene = scene;
            return true;
        }
        const w = Math.max(1, width);
        const h = Math.max(1, height);
        const tw = roundUp(w, 64);
        const th = roundUp(h, 64);
        if (tw !== this.width || th !== this.height) this.allocate(tw, th);
        gl.activeTexture(gl.TEXTURE0 + this.colourUnit);
        gl.bindTexture(gl.TEXTURE_2D, this.texture);
        gl.copyTexSubImage2D(gl.TEXTURE_2D, 0, 0, 0, 0, 0, w, h);
        gl.activeTexture(gl.TEXTURE0);
        this.viewWidth = w;
        this.viewHeight = h;
        return true;
    }

    // Bind whatever `capture` found, and every uniform the block above wants. Called for the
    // opaque pass as well as the blended one, because a sampler in a linked program with nothing
    // bound to its unit is undefined and not merely unused.
    apply(uniforms, live, viewWidth, viewHeight) {
        const gl = this.gl;
        const scene = live ? this.scene : null;
        const hasDepth = !!(scene && scene.depth);
        const fallback = scene ? scene.colour : this.texture;

        gl.activeTexture(gl.TEXTURE0 + this.colourUnit);
        gl.bindTexture(gl.TEXTURE_2D, fallback);
        gl.activeTexture(gl.TEXTURE0 + this.depthUnit);
        // Never nothing: the depth sampler is read under a uniform branch, and a unit with no
        // texture on it is undefined rather than unused. The colour texture stands in when there
        // is no depth, and `u_behindHasDepth` is what stops it being believed.
        gl.bindTexture(gl.TEXTURE_2D, hasDepth ? scene.depth : fallback);
        gl.activeTexture(gl.TEXTURE0);

        const set1i = (name, value) => {
            if (uniforms[name] !== undefined) gl.uniform1i(uniforms[name], value);
        };
        const set1f = (name, value) => {
            if (uniforms[name] !== undefined) gl.uniform1f(uniforms[name], value);
        };
        set1i('u_behind', this.colourUnit);
        set1i('u_behindDepth', this.depthUnit);
        set1f('u_behindHasDepth', hasDepth ? 1 : 0);
        // The shared capture covers the whole screen however many pixels it has, so its uv IS the
        // screen's uv. The fallback is a corner of a rounded-up texture and is not.
        if (uniforms.u_behindScale !== undefined) {
            if (scene) gl.uniform2f(uniforms.u_behindScale, 1, 1);
            else {
                gl.uniform2f(uniforms.u_behindScale, this.viewWidth / this.width,
                             this.viewHeight / this.height);
            }
        }
        if (uniforms.u_viewportInv !== undefined) {
            const w = Math.max(1, viewWidth || this.viewWidth);
            const h = Math.max(1, viewHeight || this.viewHeight);
            gl.uniform2f(uniforms.u_viewportInv, 1 / w, 1 / h);
        }
        set1f('u_refract', (live && this.enabled && this.wanted) ? 1 : 0);
        set1f('u_thickness', this.thickness);
        set1f('u_maxOffset', this.maxOffset);
    }
}
