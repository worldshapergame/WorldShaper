// The sun's own shadow, sharp, and the contact scale underneath it.
//
// # What was wrong
//
// The sun term in this viewer was one number out of the light grid: a trilinear fetch from a
// lattice of points 0.4 m apart. Forty centimetres is a soft blob, and the facility is built to
// show the opposite. `portico.clip` is a hard sun shadow across six fluted shafts whose fillets
// are 0.030 m. `crypt.clip` puts three gratings of 0.030 m iron bars over a floor and calls the
// stencil of hard-edged stripes they throw "the highest-contrast small feature in the building".
// `theatre.clip` aims twenty-four footlights up a proscenium so that every rustication throws a
// long shadow upwards.
//
// **A 0.4 m lattice cannot represent a 3 cm bar's shadow at all.** Not softly, not badly — the
// bar is a thirteenth of one cell and there is no value in the volume that knows it is there. So
// the sharp end of the sun has to come from somewhere else, and that is what this file is.
//
// # What it is
//
//   a shadow map      orthographic down the sun, over the clip's own bounds, rasterised from the
//                     SAME instanced quads the surface pass draws, once, at load. The clip does
//                     not move and neither does the sun, so it is never rendered again.
//   a near cascade    a second map over a 16 m box round the eye, for clips too big for one map
//                     to carry a centimetre-scale feature. Re-rendered only when the eye has
//                     walked out of the box it was rendered for. Off entirely for a clip that
//                     fits one map, which is most of them -- see `kOneMapSpan`.
//   contact shadows   a short screen-space trace along the sun ray, 0.30 m, against a depth
//                     pre-pass. This is the scale BELOW the map's texel: the join where a bench
//                     meets a floor, the underside of a moulding, a baluster against its rail.
//
// # The texel, measured, because it is the whole question
//
// The map's texel is the light-space span of what it covers divided by its resolution. Measured
// on the bakes in `web/data` with the sun at `kSunDir`:
//
//   clip                light-space span      2048         4096      a 0.030 m bar at 2048
//   sampler                  12.8 m         0.62 cm      0.31 cm      4.8 texels
//   facility/portico         16.5 m         0.81 cm      0.40 cm      3.7 texels
//   facility/crypt           27.5 m         1.34 cm      0.67 cm      2.2 texels
//   facility (whole)         45.8 m         2.23 cm      1.12 cm      1.3 texels
//
// So: **one 2048 map resolves a 3 cm bar on every fragment and does NOT resolve it on the whole
// building.** 1.3 texels is under Nyquist — the bar would land on one texel here and none there,
// and seven bars in a row would come out as five stripes and a smear, which is precisely the
// failure `crypt.clip` is written to catch. 4096 over the whole building is 2.7 texels, which
// resolves but aliases badly along a bar that is not axis-aligned to the map, and it costs 64 MB
// of depth on a phone. Hence the near cascade: 16 m at 2048 is 0.78 cm, and a bar is 3.8 texels.
//
// # The split with the light grid, which is not a sum and not a product
//
// Both terms answer the same question -- what fraction of the sun's disc reaches this point -- so
// adding them doubles the shadow and multiplying them squares it. They are split by SCALE and
// crossfaded, and the number that decides is the distance from the blocker.
//
// The sun is a disc about half a degree across, so a shadow's penumbra is 0.0093 of the throw. At
// a 0.3 m throw that is 3 mm and the shadow is hard; at 43 m it is 40 cm, which is exactly the
// light grid's own cell. So:
//
//   throw under 5 m     the map, with its PCF kernel opened to the real penumbra width
//   throw over 20 m     the light grid, which IS a 0.4 m blur of the same visibility
//   between             crossfaded
//
// That is the whole reconciliation, and it means each term is used where it is the better
// estimate rather than where it is convenient. Outside the map's box the grid is all there is,
// and the crossfade to it is the same one.
//
// # Which artefacts were traded for which
//
// The grid leaks. A lattice point buried in stone has no light of its own and is filled from its
// brightest neighbour at HALF strength, twice (three quarters put a pale band across every soffit
// in the halls). Half is dark enough not to be seen in the ambient and it is still a leak: a
// surface within 0.4 m of a lit wall reads a sun term it cannot see.
//
// A shadow map has the opposite pair of faults and they are the ones now on screen:
//
//   acne          a lit face self-shadowing, because the depth it stores IS its own depth.
//                 Traded away with a NORMAL offset of 1.6 texels rather than a depth bias --
//                 normal offset moves the sample off the surface instead of down the light, so
//                 it costs nothing extra at a grazing angle where a depth bias costs the most.
//   peter-panning what the normal offset buys is a shadow that starts 1.6 texels late, so it
//                 detaches from the foot of what casts it. At 0.81 cm a texel on the portico that
//                 is 1.3 cm, which is a third of a voxel at metre 32. THIS IS WHAT THE CONTACT
//                 TRACE IS FOR: 0.30 m of screen-space ray closes exactly that gap, and it is
//                 why the two are in one file.
//
// The leak is gone from the sun term wherever the map covers -- the map has no concept of a
// buried point. It is untouched in the SKY term, which is still the grid's byte and still half.
//
// # What it costs
//
// The map is rendered once per clip and the near cascade only when the eye leaves its box, so
// neither is a per-frame cost. Per frame there is one depth-only pass over the opaque mesh for
// the contact trace, and in the surface shader ten fetches of the map (one for the blocker, nine
// for the kernel) plus up to eight of the depth buffer -- and the contact trace is skipped
// outright wherever the map already says the point is in shadow, which in a portico is most of
// it. Measured numbers are in documentation/24-clip-viewer.md.

// The texture units, taken from the register in web/js/gl.js rather than picked here: 2, 3 and 4
// are the colour irradiance volume, the occlusion atlas and the emissive light list.
export const SHADOW_FAR_UNIT = 8;    // UNIT.shadowFar
export const SHADOW_NEAR_UNIT = 9;   // UNIT.shadowNear
export const SCENE_DEPTH_UNIT = 10;  // UNIT.sceneDepth

// The sun is half a degree across. Everything about how wide a penumbra should be comes from
// this one number and none of it is a look.
export const SUN_TANGENT = 0.0093;

// A clip whose light-space span is under this is carried by ONE map at a centimetre a texel, and
// the near cascade is never built for it. 20 m at 2048 is 0.98 cm, which resolves a 3 cm bar at
// nearly four texels.
const kOneMapSpan = 20.0;
// The near cascade's box, when there is one. 16 m at 2048 is 0.78 cm a texel.
const kNearSpan = 16.0;
// How far the eye may walk before the near cascade is rendered again. A quarter of the box, so
// the eye is never within a quarter of the box of its edge.
const kNearSlack = 4.0;

// --- the depth-only program, over the same instanced quads -------------------------------------
//
// The attribute locations are the surface program's, so this draws through the renderer's own VAO
// and its own per-face loop. a_ao is not read and a_material is not read: a shadow is a shape.

const DEPTH_VERTEX = `#version 300 es
precision highp float;

layout(location = 0) in vec3 a_cell;
layout(location = 1) in vec2 a_size;

uniform mat4 u_viewProj;
uniform vec3 u_origin;
uniform float u_scale;
uniform vec3 u_offset;
uniform vec3 u_eu;
uniform vec3 u_ev;
uniform float u_flip;

out vec3 v_world;

void main() {
    vec2 corner = vec2(float(gl_VertexID & 1), float((gl_VertexID >> 1) & 1));
    vec2 uv = (u_flip > 0.5) ? corner.yx : corner;
    vec3 cell = a_cell + u_offset + u_eu * (uv.x * a_size.x) + u_ev * (uv.y * a_size.y);
    v_world = u_origin + cell * u_scale;
    gl_Position = u_viewProj * vec4(v_world, 1.0);
}`;

// The slice is honoured here as well as in the surface pass. A building cut in half must not go
// on casting the shadow of the half that was cut away, and the depth pre-pass must agree with
// what is actually on screen or the contact trace shadows against geometry nobody can see.
const DEPTH_FRAGMENT = `#version 300 es
precision highp float;
in vec3 v_world;
uniform vec4 u_clip;
void main() {
    if (dot(v_world, u_clip.xyz) + u_clip.w > 0.0) discard;
}`;

// --- what goes into the surface fragment shader -------------------------------------------------
//
// One block of uniforms and functions, and one call. gl.js splices both in between markers.

export const SHADOW_GLSL = `
// The sun's own shadow. See web/js/features/shadow.js for why a 0.4 m lattice cannot do this and
// what the split between the two terms is.
uniform highp sampler2D u_sunFarMap;
uniform highp sampler2D u_sunNearMap;
uniform mat4 u_sunFarMatrix;      // world -> (u, v, depth), all three in 0..1
uniform mat4 u_sunNearMatrix;
uniform vec4 u_sunFar;            // 1/resolution, metres per texel, metres per unit depth, spare
uniform vec4 u_sunNear;
uniform float u_sunNearOn;
uniform float u_sunMapOn;
uniform highp sampler2D u_sceneDepth;
uniform vec4 u_sceneCamera;       // near, far, 1/width, 1/height
uniform float u_contactReach;     // metres of screen-space trace; 0 turns it off
uniform mat4 u_viewProj;

const float WS_SUN_TANGENT = ${SUN_TANGENT};

// One map. The centre fetch is the blocker, and the blocker is what says how wide the kernel
// should be -- the sun is a disc, so a shadow thrown 3 m has a 2.8 cm penumbra and a shadow
// thrown 0.2 m has a 2 mm one. A fixed kernel gets one of those two right.
float ws_sun_map(highp sampler2D map, vec3 at, vec4 shape, out float throwMetres) {
    float texel = shape.x;
    float centre = texture(map, at.xy).r;
    throwMetres = max(0.0, at.z - centre) * shape.z;
    float radius = clamp(throwMetres * WS_SUN_TANGENT / shape.y, 0.6, 3.0);
    float lit = 0.0;
    float total = 0.0;
    for (int j = -1; j <= 1; ++j) {
        for (int i = -1; i <= 1; ++i) {
            vec2 tap = at.xy + vec2(float(i), float(j)) * texel * radius;
            float d = texture(map, tap).r;
            // A tent rather than a box: nine equal taps at one radius is a ring, and a ring
            // reads as a second edge outside the first one.
            float w = (i == 0 ? 1.0 : 0.55) * (j == 0 ? 1.0 : 0.55);
            lit += (at.z <= d) ? w : 0.0;
            total += w;
        }
    }
    return lit / total;
}

// How far inside its own box a point is, in units of the fade band, so a cascade ends in a
// gradient rather than in a line.
//
// ACROSS the map only. The third axis is the depth and it must NOT fade: a floor is the farthest
// thing from the sun in the whole clip, its depth sits hard against 1, and fading on that would
// take the shadow off exactly the surface every shadow in this building lands on. It did.
float ws_sun_inside(vec3 at, float fade) {
    if (at.z < -0.02 || at.z > 1.02) return 0.0;
    vec2 d = min(at.xy, vec2(1.0) - at.xy);
    return clamp(min(d.x, d.y) / fade, 0.0, 1.0);
}

vec3 ws_sun_project(mat4 m, vec3 world, vec3 N, vec4 shape, float ndl) {
    // The offset is along the NORMAL, not down the light. It is what buys off acne, and the
    // slope term is because a face the sun grazes has its own depth spread over more texels.
    float slope = clamp(1.0 - ndl, 0.0, 1.0);
    vec4 c = m * vec4(world + N * shape.y * (1.6 + 2.4 * slope), 1.0);
    return c.xyz / c.w;
}

// The sun's visibility, as the map and the grid between them know it.
float ws_sun_visibility(vec3 world, vec3 N, vec3 L, float lattice, float ndl) {
    if (u_sunMapOn < 0.5 || ndl <= 0.0) return lattice;

    float thrown = 0.0;
    float map = 1.0;
    float covered = 0.0;

    if (u_sunNearOn > 0.5) {
        vec3 at = ws_sun_project(u_sunNearMatrix, world, N, u_sunNear, ndl);
        float inside = ws_sun_inside(at, 0.08);
        if (inside > 0.0) {
            map = ws_sun_map(u_sunNearMap, at, u_sunNear, thrown);
            covered = inside;
        }
    }
    if (covered < 1.0) {
        float farThrown = 0.0;
        vec3 at = ws_sun_project(u_sunFarMatrix, world, N, u_sunFar, ndl);
        float inside = ws_sun_inside(at, 0.008);
        float value = 1.0;
        if (inside > 0.0) value = ws_sun_map(u_sunFarMap, at, u_sunFar, farThrown);
        map = mix(value, map, covered);
        thrown = mix(farThrown, thrown, covered);
        covered = max(covered, inside);
    }

    // Outside every map the grid is all there is.
    float sun = mix(lattice, map, covered);

    // ...and past the throw where the sun's own disc is wider than the kernel can open, the grid
    // is the better estimate and not merely the fallback: it IS a 0.4 m blur of this same
    // visibility, and 0.4 m is the true penumbra of a shadow thrown 43 m. Crossfaded, never
    // added -- both terms answer the same question and a sum would shadow twice.
    sun = mix(sun, lattice, smoothstep(5.0, 20.0, thrown) * covered);
    return sun;
}

float ws_view_depth(float ndcZ) {
    float n = u_sceneCamera.x;
    float f = u_sceneCamera.y;
    return (2.0 * n * f) / (f + n - ndcZ * (f - n));
}

// The scale below the map's texel. Eight steps of a ray up the sun, projected into the depth
// pre-pass -- this is the join where a bench meets a floor and the underside of a moulding, and
// it is also what closes the gap the normal offset opens at the foot of a wall.
float ws_contact_shadow(vec3 world, vec3 N, vec3 L) {
    if (u_contactReach <= 0.0) return 1.0;
    const int STEPS = 8;
    // Interleaved gradient noise: eight steps without it are eight stripes, and with it they are
    // a grain finer than a pixel of the shadow they are drawing.
    float jitter = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
    vec3 from = world + N * 0.010;
    float shade = 0.0;
    for (int i = 0; i < STEPS; ++i) {
        float t = (float(i) + jitter) / float(STEPS);
        vec4 c = u_viewProj * vec4(from + L * (t * u_contactReach), 1.0);
        if (c.w <= 0.0) break;
        vec3 ndc = c.xyz / c.w;
        if (abs(ndc.x) > 1.0 || abs(ndc.y) > 1.0) break;
        float scene = texture(u_sceneDepth, ndc.xy * 0.5 + 0.5).r * 2.0 - 1.0;
        float sceneZ = ws_view_depth(scene);
        float rayZ = ws_view_depth(ndc.z);
        float into = rayZ - sceneZ;
        // Behind something, and not behind something it has no business being behind: a wall
        // thirty metres further on is not what is touching this bench. The near limit follows
        // the distance because the depth buffer's own resolution does.
        if (into > max(0.015, sceneZ * 0.004) && into < 0.35) {
            // Fading with the length of the trace, so the far end of the reach is not a ring.
            shade = 1.0 - smoothstep(0.55, 1.0, t);
            break;
        }
    }
    return 1.0 - shade * 0.9;
}

// The two are combined with a MINIMUM and not a product. They occlude different scales of the
// same sun: whatever the map found, the trace can only ever find more, and where the map already
// says nothing reaches this point a product would darken it a second time.
float ws_sun(vec3 world, vec3 N, vec3 L, float lattice, float ndl) {
    float sun = ws_sun_visibility(world, N, L, lattice, ndl);
    if (sun > 0.02 && ndl > 0.0) sun = min(sun, ws_contact_shadow(world, N, L));
    return sun;
}
`;

// --- a very small amount of matrix arithmetic, kept here so gl.js is not edited for it ---------

function normalise(v) {
    const length = Math.hypot(v[0], v[1], v[2]) || 1;
    return [v[0] / length, v[1] / length, v[2] / length];
}

function cross(a, b) {
    return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]];
}

function compile(gl, type, source, name) {
    const shader = gl.createShader(type);
    gl.shaderSource(shader, source);
    gl.compileShader(shader);
    if (!gl.getShaderParameter(shader, gl.COMPILE_STATUS)) {
        throw new Error(name + ': ' + gl.getShaderInfoLog(shader));
    }
    return shader;
}

function link(gl, vertexSource, fragmentSource, name) {
    const program = gl.createProgram();
    gl.attachShader(program, compile(gl, gl.VERTEX_SHADER, vertexSource, name + ' vertex'));
    gl.attachShader(program, compile(gl, gl.FRAGMENT_SHADER, fragmentSource, name + ' fragment'));
    gl.linkProgram(program);
    if (!gl.getProgramParameter(program, gl.LINK_STATUS)) {
        throw new Error(name + ': ' + gl.getProgramInfoLog(program));
    }
    const uniforms = {};
    const count = gl.getProgramParameter(program, gl.ACTIVE_UNIFORMS);
    for (let i = 0; i < count; ++i) {
        const info = gl.getActiveUniform(program, i);
        uniforms[info.name] = gl.getUniformLocation(program, info.name);
    }
    return { program, uniforms };
}

// --- one shadow map ------------------------------------------------------------------------------

class Cascade {
    constructor(gl, resolution) {
        this.gl = gl;
        this.resolution = resolution;
        this.texture = gl.createTexture();
        gl.bindTexture(gl.TEXTURE_2D, this.texture);
        gl.texStorage2D(gl.TEXTURE_2D, 1, gl.DEPTH_COMPONENT24, resolution, resolution);
        // NEAREST and no comparison: the kernel below reads the DEPTH, because the distance to
        // the blocker is what opens the kernel to the width of the sun. A sampler2DShadow would
        // give hardware 2x2 filtering and would not give that number back.
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
        // Clamped, and the border therefore repeats the edge. Nothing reads outside the box: the
        // coverage test above fades to the light grid before it gets there.
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);

        this.frame = gl.createFramebuffer();
        gl.bindFramebuffer(gl.FRAMEBUFFER, this.frame);
        gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.DEPTH_ATTACHMENT, gl.TEXTURE_2D,
                                this.texture, 0);
        // No colour at all. A shadow map that writes one is writing a megabyte a frame it never
        // reads.
        gl.drawBuffers([gl.NONE]);
        gl.readBuffer(gl.NONE);
        const status = gl.checkFramebufferStatus(gl.FRAMEBUFFER);
        gl.bindFramebuffer(gl.FRAMEBUFFER, null);
        if (status !== gl.FRAMEBUFFER_COMPLETE) {
            throw new Error('shadow map framebuffer is ' + status.toString(16));
        }

        // world -> (u, v, depth), all three 0..1, and its clip-space twin for rendering.
        this.matrix = new Float32Array(16);
        this.render = new Float32Array(16);
        this.metresPerTexel = 1;
        this.metresPerDepth = 1;
        this.centre = [0, 0, 0];
        this.ready = false;
    }

    // The box, in the sun's own frame. `spanU`/`spanV` may be given to cover less than the whole
    // clip -- that is what makes the near cascade near -- but the DEPTH range is always the whole
    // clip's, because a blocker outside the box still casts into it.
    aim(basis, low, high, centre, span) {
        const { right, up, forward } = basis;
        let uMin = 1e9, uMax = -1e9, vMin = 1e9, vMax = -1e9, dMin = 1e9, dMax = -1e9;
        for (let corner = 0; corner < 8; ++corner) {
            const p = [(corner & 1) ? high[0] : low[0], (corner & 2) ? high[1] : low[1],
                       (corner & 4) ? high[2] : low[2]];
            const u = right[0] * p[0] + right[1] * p[1] + right[2] * p[2];
            const v = up[0] * p[0] + up[1] * p[1] + up[2] * p[2];
            const d = forward[0] * p[0] + forward[1] * p[1] + forward[2] * p[2];
            uMin = Math.min(uMin, u); uMax = Math.max(uMax, u);
            vMin = Math.min(vMin, v); vMax = Math.max(vMax, v);
            dMin = Math.min(dMin, d); dMax = Math.max(dMax, d);
        }
        if (span > 0) {
            const cu = right[0] * centre[0] + right[1] * centre[1] + right[2] * centre[2];
            const cv = up[0] * centre[0] + up[1] * centre[1] + up[2] * centre[2];
            // Snapped to a texel, so that the map a step later is the same map shifted by whole
            // texels and the edge of a shadow does not crawl when it is rebuilt.
            const texel = span / this.resolution;
            const su = Math.round(cu / texel) * texel;
            const sv = Math.round(cv / texel) * texel;
            uMin = Math.max(uMin, su - span * 0.5); uMax = Math.min(uMax, su + span * 0.5);
            vMin = Math.max(vMin, sv - span * 0.5); vMax = Math.min(vMax, sv + span * 0.5);
        }
        // Half a metre of margin, which is more than the fade band the shader keeps at the edge
        // of the box -- so nothing that is actually inside the clip is ever in the fade.
        const pad = 0.5;
        uMin -= pad; uMax += pad; vMin -= pad; vMax += pad; dMin -= pad; dMax += pad;

        const spanU = Math.max(1e-3, uMax - uMin);
        const spanV = Math.max(1e-3, vMax - vMin);
        const spanD = Math.max(1e-3, dMax - dMin);
        this.metresPerTexel = Math.max(spanU, spanV) / this.resolution;
        this.metresPerDepth = spanD;
        this.centre = centre.slice();
        this.span = span;

        // Rows of the clip-space matrix: u and v to -1..1, and depth INVERTED, so that 0 is at
        // the sun and a blocker is always the smaller number.
        const rows = [
            [right[0] * 2 / spanU, right[1] * 2 / spanU, right[2] * 2 / spanU,
             -(uMax + uMin) / spanU],
            [up[0] * 2 / spanV, up[1] * 2 / spanV, up[2] * 2 / spanV, -(vMax + vMin) / spanV],
            [-forward[0] * 2 / spanD, -forward[1] * 2 / spanD, -forward[2] * 2 / spanD,
             (dMax + dMin) / spanD],
            [0, 0, 0, 1],
        ];
        for (let c = 0; c < 4; ++c) {
            for (let r = 0; r < 4; ++r) {
                this.render[c * 4 + r] = rows[r][c];
                // ...and the same thing with -1..1 folded into 0..1, which is what a texture
                // fetch and a depth comparison both want.
                this.matrix[c * 4 + r] = (r < 3)
                    ? rows[r][c] * 0.5 + (c === 3 ? 0.5 : 0)
                    : rows[r][c];
            }
        }
    }
}

// --- the whole of it ------------------------------------------------------------------------------

export class SunShadow {
    constructor(gl) {
        this.gl = gl;
        this.depth = link(gl, DEPTH_VERTEX, DEPTH_FRAGMENT, 'shadow depth');

        // 2048 unless the card cannot, which no WebGL2 implementation reports. A 2048 depth map
        // is 16 MB; 4096 is 64 MB and is not a phone's memory to spend, and the near cascade is
        // what buys the same texel size for a quarter of it.
        const most = gl.getParameter(gl.MAX_TEXTURE_SIZE);
        this.resolution = Math.min(2048, most);

        this.far = new Cascade(gl, this.resolution);
        this.near = new Cascade(gl, this.resolution);
        this.nearOn = false;
        this.nearAt = null;

        this.sceneDepth = null;
        this.sceneFrame = gl.createFramebuffer();
        this.sceneSize = [0, 0];
        this.contactReach = 0.30;

        this.basis = null;
        this.clip = null;
        this.draw = null;
        this.enabled = true;
        this.stats = { rebuilds: 0, texelCm: 0, nearTexelCm: 0 };
    }

    // The sun's frame. `sun` points AT the sun, so `forward` grows towards it and a blocker is
    // always the larger number along it -- which the matrix above then inverts.
    setSun(sun) {
        const forward = normalise(sun);
        const up = Math.abs(forward[1]) > 0.99 ? [0, 0, 1] : [0, 1, 0];
        const right = normalise(cross(up, forward));
        this.basis = { forward, right, up: cross(forward, right) };
    }

    // Called once per clip. `draw(uniforms, matrix, plane)` renders the opaque mesh; it is kept
    // so the near cascade can be rebuilt later without gl.js handing it over again.
    setClip(clip, sun, draw) {
        this.clip = clip;
        this.draw = draw;
        this.setSun(sun);
        this.nearAt = null;
        this.nearOn = false;
        if (!clip || clip.opaqueQuads === 0) { this.far.ready = false; return; }

        const low = clip.low;
        const high = clip.high;
        this.far.aim(this.basis, low, high, [0, 0, 0], 0);
        this.renderCascade(this.far, [0, 0, 0, -1]);
        this.far.ready = true;
        this.stats.texelCm = this.far.metresPerTexel * 100;

        // Is one map enough? The span that matters is the light-space one, which for a building
        // seen from the south-east is bigger than any of its three sides.
        const span = this.far.metresPerTexel * this.resolution;
        this.nearOn = span > kOneMapSpan;
        this.stats.nearTexelCm = this.nearOn ? (kNearSpan / this.resolution) * 100 : 0;
    }

    renderCascade(cascade, plane) {
        const gl = this.gl;
        gl.bindFramebuffer(gl.FRAMEBUFFER, cascade.frame);
        gl.viewport(0, 0, cascade.resolution, cascade.resolution);
        gl.disable(gl.BLEND);
        gl.disable(gl.STENCIL_TEST);
        // No culling. A clip that has been sliced is an open shell and half of what casts a
        // shadow in it is seen from behind; culling would drop it out of its own shadow.
        gl.disable(gl.CULL_FACE);
        gl.enable(gl.DEPTH_TEST);
        gl.depthFunc(gl.LESS);
        gl.depthMask(true);
        gl.clearDepth(1);
        gl.clear(gl.DEPTH_BUFFER_BIT);
        gl.useProgram(this.depth.program);
        this.draw(this.depth.uniforms, cascade.render, plane);
        gl.bindFramebuffer(gl.FRAMEBUFFER, null);
        this.stats.rebuilds += 1;
    }

    // The depth of what is on screen, for the contact trace. It is its own pass because a
    // framebuffer cannot be read while it is being written to, and it is full resolution because
    // the whole point of the trace is the scale a half-resolution buffer throws away.
    ensureSceneDepth(width, height) {
        const gl = this.gl;
        if (this.sceneDepth && this.sceneSize[0] === width && this.sceneSize[1] === height) return;
        if (this.sceneDepth) gl.deleteTexture(this.sceneDepth);
        this.sceneDepth = gl.createTexture();
        gl.bindTexture(gl.TEXTURE_2D, this.sceneDepth);
        gl.texStorage2D(gl.TEXTURE_2D, 1, gl.DEPTH_COMPONENT24, width, height);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
        gl.bindFramebuffer(gl.FRAMEBUFFER, this.sceneFrame);
        gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.DEPTH_ATTACHMENT, gl.TEXTURE_2D,
                                this.sceneDepth, 0);
        gl.drawBuffers([gl.NONE]);
        gl.readBuffer(gl.NONE);
        gl.bindFramebuffer(gl.FRAMEBUFFER, null);
        this.sceneSize = [width, height];
    }

    // Everything that has to happen before the surface pass: the near cascade if the eye has
    // walked out of it, and the depth of the frame about to be drawn.
    before(view) {
        if (!this.enabled || !this.far.ready || !this.draw) return;
        const gl = this.gl;

        if (this.nearOn) {
            const eye = view.eye;
            const ahead = normalise([view.at[0] - eye[0], view.at[1] - eye[1], view.at[2] - eye[2]]);
            // Pushed a little the way the eye is looking, so more of the box is in front of it
            // than behind.
            const centre = [eye[0] + ahead[0] * kNearSpan * 0.25,
                            eye[1] + ahead[1] * kNearSpan * 0.25,
                            eye[2] + ahead[2] * kNearSpan * 0.25];
            const moved = !this.nearAt || Math.hypot(centre[0] - this.nearAt[0],
                                                     centre[1] - this.nearAt[1],
                                                     centre[2] - this.nearAt[2]) > kNearSlack;
            if (moved || this.nearPlane !== JSON.stringify(view.plane)) {
                this.near.aim(this.basis, this.clip.low, this.clip.high, centre, kNearSpan);
                this.renderCascade(this.near, view.plane);
                this.nearAt = centre;
                this.nearPlane = JSON.stringify(view.plane);
            }
        }

        if (this.contactReach > 0) {
            this.ensureSceneDepth(view.width, view.height);
            gl.bindFramebuffer(gl.FRAMEBUFFER, this.sceneFrame);
            gl.viewport(0, 0, view.width, view.height);
            gl.disable(gl.BLEND);
            gl.disable(gl.STENCIL_TEST);
            gl.disable(gl.CULL_FACE);
            gl.enable(gl.DEPTH_TEST);
            gl.depthFunc(gl.LESS);
            gl.depthMask(true);
            gl.clearDepth(1);
            gl.clear(gl.DEPTH_BUFFER_BIT);
            gl.useProgram(this.depth.program);
            this.draw(this.depth.uniforms, view.viewProj, view.plane);
            gl.bindFramebuffer(gl.FRAMEBUFFER, null);
            gl.viewport(0, 0, view.width, view.height);
        }
    }

    // Units 2, 3 and 4. Units 0 and 1 are the materials and the light grid.
    bind(uniforms, camera) {
        const gl = this.gl;
        const on = (this.enabled && this.far.ready) ? 1 : 0;
        gl.uniform1f(uniforms.u_sunMapOn, on);
        gl.uniform1f(uniforms.u_sunNearOn, (on && this.nearOn && this.nearAt) ? 1 : 0);
        gl.uniformMatrix4fv(uniforms.u_sunFarMatrix, false, this.far.matrix);
        gl.uniformMatrix4fv(uniforms.u_sunNearMatrix, false, this.near.matrix);
        gl.uniform4fv(uniforms.u_sunFar, [1 / this.far.resolution, this.far.metresPerTexel,
                                          this.far.metresPerDepth, 0]);
        gl.uniform4fv(uniforms.u_sunNear, [1 / this.near.resolution, this.near.metresPerTexel,
                                           this.near.metresPerDepth, 0]);
        gl.uniform1f(uniforms.u_contactReach,
                     (on && this.sceneDepth && this.contactReach > 0) ? this.contactReach : 0);
        gl.uniform4fv(uniforms.u_sceneCamera, [camera.near, camera.far,
                                               1 / Math.max(1, this.sceneSize[0]),
                                               1 / Math.max(1, this.sceneSize[1])]);
        gl.activeTexture(gl.TEXTURE0 + SHADOW_FAR_UNIT);
        gl.bindTexture(gl.TEXTURE_2D, this.far.texture);
        gl.uniform1i(uniforms.u_sunFarMap, SHADOW_FAR_UNIT);
        gl.activeTexture(gl.TEXTURE0 + SHADOW_NEAR_UNIT);
        gl.bindTexture(gl.TEXTURE_2D, this.near.texture);
        gl.uniform1i(uniforms.u_sunNearMap, SHADOW_NEAR_UNIT);
        gl.activeTexture(gl.TEXTURE0 + SCENE_DEPTH_UNIT);
        gl.bindTexture(gl.TEXTURE_2D, this.sceneDepth || this.far.texture);
        gl.uniform1i(uniforms.u_sceneDepth, SCENE_DEPTH_UNIT);
        gl.activeTexture(gl.TEXTURE0);
    }
}
