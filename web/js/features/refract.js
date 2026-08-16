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
// surface takes the picture that was already on the framebuffer — everything opaque, drawn — and
// samples it at an offset, and the offset is the refracted vector carried across the material's
// own thickness and projected back to the screen. That is right for a phone and it is wrong in
// three ways that are worth saying out loud rather than discovering:
//
//   - it can only show what is ON SCREEN. A pane at the edge of the view refracts what is beside
//     it in the frame, not what is beside it in the world, and the sample is clamped rather than
//     wrapped so the edge smears instead of tiling.
//   - it has no depth test against the picture it samples, so a post standing IN FRONT of a thick
//     refractor can be pulled a few pixels into it. The offset is clamped for exactly this reason
//     (see `maxOffset`), which bounds the artefact rather than removing it. One line removes it if
//     an offscreen target with a depth texture lands — see `capture` below.
//   - the picture it samples was taken before ANY transparent surface was drawn, so glass behind
//     glass shows the stone behind both rather than the near pane's own tint.
//
// **The absorption is not an approximation.** exp(-absorb * path) over a real path length is what
// `shaders/node.glsl:node_medium_absorb` does in the game, in the same units — the byte is
// sixteenths per metre — and the path is the thickness crossed at the refracted angle rather than
// the thickness itself, so a slanted look through a pane is deeper in colour than a square one.
// Everything therefore turns on knowing the thickness, which is why `refract_thickness` below is
// the one dependency this file has.
//
// # Cost
//
// One full-screen copy of the framebuffer per frame, and only for a clip that has a material with
// both an `ior` and an `opacity` — `wantedFor` decides that once, at load, so the facility's own
// glass pays for it and a clip with no glass in it pays nothing at all. Per glass pixel it is one
// `refract`, one matrix multiply and one texture fetch. There is no loop over layers anywhere in
// here, which is the thing a phone cannot afford.

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
uniform sampler2D u_behind;    // the opaque picture, tonemapped and gamma-encoded, as it was drawn
uniform vec2 u_behindScale;    // viewport / texture size, because the texture is rounded up
uniform vec2 u_viewportInv;    // 1 / viewport in pixels, to turn gl_FragCoord into a uv
uniform float u_refract;       // 0 is the control arm: no offset, no fetch, no copy
uniform float u_thickness;     // metres of matter behind a surface, until THCK bakes the field
uniform float u_maxOffset;     // how far a refracted sample may reach, in screen fractions
uniform mat4 u_viewProj;       // the same matrix the vertex stage uses, to project the exit point

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

// What a transparent surface finally is: what is behind it, bent and absorbed, with its own lit
// surface over the top in the proportion its opacity and its angle ask for.
//
// The gamma here is the file's own convention -- squared in, square-rooted out -- because the
// picture being sampled has already been tonemapped and encoded and must not be tonemapped twice.
// Transmittance multiplies in LIGHT and not in the encoding, so the decode is not optional.
vec4 refract_composite(vec3 surface, float opacity, vec3 world, vec3 N, vec3 V,
                       float iorByte, vec3 absorbByte, float ndv) {
    // Glancing angles of a pane are more reflective and less see-through, which is the one thing
    // about glass everybody notices when it is missing.
    float alpha = clamp(opacity + (1.0 - opacity) * pow(1.0 - ndv, 4.0), 0.0, 1.0);
    vec3 k = refract_absorption(absorbByte);

    // Nothing to bend with, or the control arm: the old blend, except that the tint is now a real
    // Beer-Lambert over the thickness rather than a fraction of the byte.
    if (u_refract < 0.5 || iorByte < 0.5) {
        vec3 tint = exp(-k * refract_thickness(world, -N));
        return vec4(surface * tint, alpha);
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
    // a rasteriser can afford and is what everything on a phone does.
    vec2 here = gl_FragCoord.xy * u_viewportInv;
    vec4 exitClip = u_viewProj * vec4(world + into * path, 1.0);
    vec2 there = (exitClip.xy / max(abs(exitClip.w), 1e-4) * sign(exitClip.w)) * 0.5 + 0.5;
    vec2 delta = there - here;
    float reach = length(delta);
    if (reach > u_maxOffset) delta *= u_maxOffset / reach;
    vec2 uv = clamp(here + delta, vec2(0.0), vec2(1.0)) * u_behindScale;

    vec3 behind = texture(u_behind, uv).rgb;
    behind = behind * behind;                 // to light, where transmittance multiplies
    behind *= exp(-k * path);                 // Beer-Lambert, over the path actually crossed
    return vec4(mix(sqrt(behind), surface, alpha), 1.0);
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
        this.unit = 2;    // 0 is the materials, 1 is the light volume
        this.allocate(1, 1);
    }

    allocate(width, height) {
        const gl = this.gl;
        gl.activeTexture(gl.TEXTURE0 + this.unit);
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

    // Take the picture that is on the framebuffer NOW.
    //
    // IT COPIES FROM WHATEVER FRAMEBUFFER IS BOUND, which is the whole of how this composes with
    // an offscreen target instead of competing with one. Bound to the canvas it copies the canvas
    // — with its multisampling resolved on the way, which is why this does not need an offscreen
    // target of its own and does not cost the picture its antialiasing. Bound to somebody else's
    // colour attachment it copies that, unchanged and with no coordination beyond the binding.
    //
    // What it would want FROM such a target, if one lands: a depth texture. With one, the sample
    // at the offset can be refused when what it lands on is nearer than the glass, which is the
    // one artefact the clamp above only bounds. That is a condition in `refract_composite` and
    // nothing else.
    //
    // The texture is rounded up to a multiple of 64 and the viewport is copied into the corner of
    // it, so the adaptive resolution in app.js — which moves the canvas by a percent at a time —
    // reallocates rarely instead of every frame. `u_behindScale` is what carries the difference.
    capture(width, height) {
        const gl = this.gl;
        if (!this.enabled || !this.wanted) return false;
        const w = Math.max(1, width);
        const h = Math.max(1, height);
        const tw = roundUp(w, 64);
        const th = roundUp(h, 64);
        if (tw !== this.width || th !== this.height) this.allocate(tw, th);
        gl.activeTexture(gl.TEXTURE0 + this.unit);
        gl.bindTexture(gl.TEXTURE_2D, this.texture);
        gl.copyTexSubImage2D(gl.TEXTURE_2D, 0, 0, 0, 0, 0, w, h);
        gl.activeTexture(gl.TEXTURE0);
        this.viewWidth = w;
        this.viewHeight = h;
        return true;
    }

    // Bind the texture and every uniform the block above wants. Called for the opaque pass as well
    // as the blended one, because a sampler in a linked program with nothing bound to its unit is
    // undefined and not merely unused.
    apply(uniforms, live) {
        const gl = this.gl;
        gl.activeTexture(gl.TEXTURE0 + this.unit);
        gl.bindTexture(gl.TEXTURE_2D, this.texture);
        gl.activeTexture(gl.TEXTURE0);
        if (uniforms.u_behind !== undefined) gl.uniform1i(uniforms.u_behind, this.unit);
        if (uniforms.u_behindScale !== undefined) {
            gl.uniform2f(uniforms.u_behindScale, this.viewWidth / this.width,
                         this.viewHeight / this.height);
        }
        if (uniforms.u_viewportInv !== undefined) {
            gl.uniform2f(uniforms.u_viewportInv, 1 / this.viewWidth, 1 / this.viewHeight);
        }
        if (uniforms.u_refract !== undefined) {
            gl.uniform1f(uniforms.u_refract, (live && this.enabled && this.wanted) ? 1 : 0);
        }
        if (uniforms.u_thickness !== undefined) gl.uniform1f(uniforms.u_thickness, this.thickness);
        if (uniforms.u_maxOffset !== undefined) gl.uniform1f(uniforms.u_maxOffset, this.maxOffset);
    }
}
