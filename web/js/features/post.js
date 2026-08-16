// The end of the frame.
//
// Everything before this file draws matter. This one takes what came out, adds the light that
// spills off a flame, turns radiance into a picture exactly once, and hands the browser eight bits
// a channel. It is where the sky, the stone, the cut face and the shapes view stop being four
// separate opinions about what "bright" means.
//
// # One tone map, not four
//
// Every pass used to end with the same three lines — `pow(tonemap(colour * exposure), 1/2.2)` —
// written out separately in the surface shader, the sky shader, the cap shader and the shapes
// shader. Four copies of a curve is four chances for one of them to drift, and the seam shows
// exactly where two of them meet: the horizon, and the edge of a cut. So when this file is running,
// none of those passes tone map at all. They write LINEAR RADIANCE into a floating-point target and
// the composite below is the only place in the viewer where a curve is applied. The four copies are
// still there for the browser that cannot give us a float target, and they are still identical.
//
// # Bloom is thresholded on the EMISSIVE TERM, not on luminance
//
// The usual bright-pass takes the final image and keeps what is over 1.0. In a scene lit by a sun
// at three times its sky, that is a sunlit limestone wall — so the building glows and the candle,
// which is three voxels across and loses most of its pixels to the neighbouring stone, does not.
//
// So the surface shader writes the emissive contribution to a SECOND RENDER TARGET, and the bloom
// chain reads that. A white wall has no emission and cannot bloom no matter how bright the sun is;
// a taper has nothing but emission and blooms at whatever size it is on screen. That is one extra
// attachment — R11F_G11F_B10F where the browser has it, four bytes a pixel — and it is the whole
// reason a lamp reads as a light rather than as a pale square.
//
// # Fog is not here
//
// Aerial perspective wants the distance to the surface and the direction of the ray, and a
// rasteriser has both in the fragment shader already. Doing it here instead would mean a depth
// texture, a reconstruction of the world position from it, and a second read of the whole target —
// all to arrive at a number the surface shader was holding. So the fog term lives in gl.js beside
// the shading it modifies, and this file never sees it. See the note there for the atmosphere's
// own numbers, which are src/app/main.cpp's and not a taste.
//
// # Precision
//
// `mediump` on everything that is a colour and `highp` on everything that is a coordinate. That is
// not a stylistic choice: mediump carries about ten bits of mantissa, so a texture coordinate near
// 1.0 in a 2048-wide target lands two texels from where it was meant to and the bloom chain
// smears diagonally. Colours in [0, 64] survive mediump with room to spare, and on a phone the
// half-rate ALU is free performance.

const FULLSCREEN_VERTEX = `#version 300 es
precision highp float;
out highp vec2 v_uv;
void main() {
    // One triangle rather than two, so there is no diagonal seam for the rasteriser to run twice.
    vec2 corner = vec2(float((gl_VertexID & 1) * 4 - 1), float((gl_VertexID >> 1) * 4 - 1));
    v_uv = corner * 0.5 + 0.5;
    gl_Position = vec4(corner, 0.0, 1.0);
}`;

// The bright pass. It reads the emissive target, so there is nothing to separate out — what is in
// there is already only the light the materials emit. The knee is here to stop a barely-emissive
// material from putting a wide soft halo round itself: below `u_knee` nothing spills, above it
// everything does, and the quadratic between the two is what keeps the transition from being a
// visible ring as a lamp brightens.
const PREFILTER_FRAGMENT = `#version 300 es
precision mediump float;
in highp vec2 v_uv;
uniform mediump sampler2D u_source;
uniform mediump float u_knee;
out mediump vec4 o_colour;
void main() {
    vec3 c = texture(u_source, v_uv).rgb;
    float brightest = max(c.r, max(c.g, c.b));
    float soft = clamp((brightest - u_knee * 0.5) / max(u_knee, 1e-4), 0.0, 1.0);
    o_colour = vec4(c * soft * soft, 1.0);
}`;

// Four bilinear taps at half a source texel, so each tap is already a 2x2 average and the four
// together are a 4x4 box for the price of four fetches. A 13-tap Jimenez downsample is better and
// costs three times as much; on a phone, at this size, nobody can tell them apart.
const DOWNSAMPLE_FRAGMENT = `#version 300 es
precision mediump float;
in highp vec2 v_uv;
uniform mediump sampler2D u_source;
uniform highp vec2 u_texel;      // one texel of the SOURCE
out mediump vec4 o_colour;
void main() {
    highp vec2 o = u_texel;
    vec3 sum = texture(u_source, v_uv + vec2(-o.x, -o.y)).rgb
             + texture(u_source, v_uv + vec2( o.x, -o.y)).rgb
             + texture(u_source, v_uv + vec2(-o.x,  o.y)).rgb
             + texture(u_source, v_uv + vec2( o.x,  o.y)).rgb;
    o_colour = vec4(sum * 0.25, 1.0);
}`;

// A 3x3 tent on the way back up, added onto the mip above with the blender rather than with a
// second read. Tent rather than box because the whole point of walking back up the chain is that
// the wide, soft levels arrive without the square edges the box put in them.
const UPSAMPLE_FRAGMENT = `#version 300 es
precision mediump float;
in highp vec2 v_uv;
uniform mediump sampler2D u_source;
uniform highp vec2 u_texel;
out mediump vec4 o_colour;
void main() {
    highp vec2 o = u_texel;
    vec3 sum = texture(u_source, v_uv + vec2(-o.x,  o.y)).rgb
             + texture(u_source, v_uv + vec2( 0.0,  o.y)).rgb * 2.0
             + texture(u_source, v_uv + vec2( o.x,  o.y)).rgb
             + texture(u_source, v_uv + vec2(-o.x,  0.0)).rgb * 2.0
             + texture(u_source, v_uv).rgb * 4.0
             + texture(u_source, v_uv + vec2( o.x,  0.0)).rgb * 2.0
             + texture(u_source, v_uv + vec2(-o.x, -o.y)).rgb
             + texture(u_source, v_uv + vec2( 0.0, -o.y)).rgb * 2.0
             + texture(u_source, v_uv + vec2( o.x, -o.y)).rgb;
    o_colour = vec4(sum * (1.0 / 16.0), 1.0);
}`;

// The one place a curve is applied. Exposure, then ACES in the fitted form the shaders were
// already using — the same five constants, so a scene looks the same as it did with four copies of
// it — then the transfer function.
const COMPOSITE_FRAGMENT = `#version 300 es
precision mediump float;
in highp vec2 v_uv;
uniform mediump sampler2D u_scene;
uniform mediump sampler2D u_bloom;
uniform mediump float u_exposure;
uniform mediump float u_bloomStrength;
uniform bool u_hasBloom;
out mediump vec4 o_colour;

vec3 tonemap(vec3 x) {
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

void main() {
    vec3 colour = texture(u_scene, v_uv).rgb;
    if (u_hasBloom) colour += texture(u_bloom, v_uv).rgb * u_bloomStrength;
    o_colour = vec4(pow(tonemap(colour * u_exposure), vec3(1.0 / 2.2)), 1.0);
}`;

// FXAA, the console flavour of 3.11: a luma cross, a direction across the edge, and four taps
// along it. It runs on the tone-mapped image on purpose — luma of a linear radiance is dominated
// by whatever is brightest in the frame and finds edges that are not there once the curve has been
// applied.
//
// The mesh here is voxel faces, so every silhouette is a staircase of axis-aligned steps, which is
// the one thing FXAA is unambiguously good at. MSAA would do it better and costs a multisampled
// depth buffer, which on a phone is the most expensive thing in the frame.
const FXAA_FRAGMENT = `#version 300 es
precision mediump float;
in highp vec2 v_uv;
uniform mediump sampler2D u_source;
uniform highp vec2 u_texel;
out mediump vec4 o_colour;

const vec3 LUMA = vec3(0.299, 0.587, 0.114);

void main() {
    highp vec2 t = u_texel;
    vec3 rgbM  = texture(u_source, v_uv).rgb;
    float lM  = dot(rgbM, LUMA);
    float lNW = dot(texture(u_source, v_uv + vec2(-t.x, -t.y)).rgb, LUMA);
    float lNE = dot(texture(u_source, v_uv + vec2( t.x, -t.y)).rgb, LUMA);
    float lSW = dot(texture(u_source, v_uv + vec2(-t.x,  t.y)).rgb, LUMA);
    float lSE = dot(texture(u_source, v_uv + vec2( t.x,  t.y)).rgb, LUMA);

    float lMin = min(lM, min(min(lNW, lNE), min(lSW, lSE)));
    float lMax = max(lM, max(max(lNW, lNE), max(lSW, lSE)));
    // Flat enough that there is no edge to soften. Most of a wall takes this branch, which is what
    // makes the pass affordable.
    if (lMax - lMin < max(0.0833, lMax * 0.166)) {
        o_colour = vec4(rgbM, 1.0);
        return;
    }

    vec2 dir = vec2(-((lNW + lNE) - (lSW + lSE)), ((lNW + lSW) - (lNE + lSE)));
    float reduce = max((lNW + lNE + lSW + lSE) * 0.03125, 0.0078125);
    float rcp = 1.0 / (min(abs(dir.x), abs(dir.y)) + reduce);
    dir = clamp(dir * rcp, vec2(-8.0), vec2(8.0)) * t;

    vec3 rgbA = 0.5 * (texture(u_source, v_uv + dir * (1.0 / 3.0 - 0.5)).rgb +
                       texture(u_source, v_uv + dir * (2.0 / 3.0 - 0.5)).rgb);
    vec3 rgbB = rgbA * 0.5 + 0.25 * (texture(u_source, v_uv - dir * 0.5).rgb +
                                     texture(u_source, v_uv + dir * 0.5).rgb);
    float lB = dot(rgbB, LUMA);
    o_colour = vec4((lB < lMin || lB > lMax) ? rgbA : rgbB, 1.0);
}`;

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

// Can this browser render into a floating-point target at all? Asked on a throwaway 1x1 context,
// BEFORE the viewer's own context exists, because the answer decides whether that context should
// ask for multisampling — and a canvas keeps the first context it is given, attributes and all, so
// there is no asking again afterwards.
export function probeFloatTargets() {
    try {
        const canvas = document.createElement('canvas');
        canvas.width = 1;
        canvas.height = 1;
        const gl = canvas.getContext('webgl2', { alpha: false, depth: false, antialias: false });
        if (!gl) return { full: false, half: false };
        const full = !!gl.getExtension('EXT_color_buffer_float');
        const half = full || !!gl.getExtension('EXT_color_buffer_half_float');
        const lose = gl.getExtension('WEBGL_lose_context');
        if (lose) lose.loseContext();
        return { full, half };
    } catch (error) {
        return { full: false, half: false };
    }
}

export class Post {
    constructor(gl, probe) {
        this.gl = gl;
        this.budget = null;              // gl.js hands its instrument over, so passes are timed
        const support = probe || probeFloatTargets();
        // EXT_color_buffer_float is what makes RGBA16F renderable; the half-float extension covers
        // the same format on the browsers that only have that one. Without either there is no HDR
        // buffer to write into and the four in-shader tone maps stay in charge — the viewer draws
        // exactly what it drew before this file existed.
        this.available = false;
        if (gl && (support.full || support.half)) {
            if (support.full) gl.getExtension('EXT_color_buffer_float');
            else gl.getExtension('EXT_color_buffer_half_float');
            // Bilinear on a float target is what the whole bloom chain is made of.
            gl.getExtension('OES_texture_float_linear');
            this.available = true;
        }
        // Eleven bits and ten is plenty for a term that only ever holds emission and is about to
        // be blurred to nothing, and it is half the bandwidth of RGBA16F on the one attachment
        // that is written by every opaque fragment in the frame.
        this.bloomFormat = support.full ? 0x8C3A /* R11F_G11F_B10F */ : 0x881A /* RGBA16F */;

        this.width = 0;
        this.height = 0;
        this.sceneFbo = null;
        this.sceneColour = null;
        this.sceneEmissive = null;
        this.sceneDepth = null;
        this.ldrFbo = null;
        this.ldrColour = null;
        this.chain = [];                 // [{ texture, fbo, width, height }]
        this.chainWanted = -1;
        this.active = false;             // is the scene currently being drawn into our target

        this.exposure = 1.0;
        this.bloomStrength = 0.62;
        this.bloomKnee = 0.55;
        this.flags = { bloom: true, bloomMips: 6, bloomBase: 2, fxaa: true };

        if (this.available) {
            this.prefilter = link(gl, FULLSCREEN_VERTEX, PREFILTER_FRAGMENT, 'bloom prefilter');
            this.down = link(gl, FULLSCREEN_VERTEX, DOWNSAMPLE_FRAGMENT, 'bloom down');
            this.up = link(gl, FULLSCREEN_VERTEX, UPSAMPLE_FRAGMENT, 'bloom up');
            this.composite = link(gl, FULLSCREEN_VERTEX, COMPOSITE_FRAGMENT, 'composite');
            this.fxaa = link(gl, FULLSCREEN_VERTEX, FXAA_FRAGMENT, 'fxaa');
            this.empty = gl.createVertexArray();
        }
    }

    setQuality(flags) {
        const wasBloom = this.flags.bloom;
        this.flags = Object.assign({}, this.flags, flags);
        // The emissive attachment is only allocated when something is going to read it. A phone on
        // the bottom rung should not be paying four bytes a pixel for a target nothing samples.
        if (wasBloom !== this.flags.bloom) this.dropTargets();
    }

    // Does the scene need a second output this frame? gl.js asks, because the answer decides
    // whether the surface shader's emissive write goes anywhere.
    get wantsEmissive() { return this.available && this.flags.bloom; }

    dropTargets() {
        const gl = this.gl;
        if (this.sceneFbo) gl.deleteFramebuffer(this.sceneFbo);
        if (this.sceneColour) gl.deleteTexture(this.sceneColour);
        if (this.sceneEmissive) gl.deleteTexture(this.sceneEmissive);
        if (this.sceneDepth) gl.deleteRenderbuffer(this.sceneDepth);
        if (this.ldrFbo) gl.deleteFramebuffer(this.ldrFbo);
        if (this.ldrColour) gl.deleteTexture(this.ldrColour);
        for (const level of this.chain) {
            gl.deleteFramebuffer(level.fbo);
            gl.deleteTexture(level.texture);
        }
        this.chain = [];
        this.chainWanted = -1;
        this.sceneFbo = this.sceneColour = this.sceneEmissive = this.sceneDepth = null;
        this.ldrFbo = this.ldrColour = null;
        this.width = this.height = 0;
    }

    target(width, height, internalFormat, format, type, filter) {
        const gl = this.gl;
        const texture = gl.createTexture();
        gl.bindTexture(gl.TEXTURE_2D, texture);
        gl.texImage2D(gl.TEXTURE_2D, 0, internalFormat, width, height, 0, format, type, null);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, filter);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, filter);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
        return texture;
    }

    resize(width, height) {
        if (this.width === width && this.height === height) return;
        this.dropTargets();
        const gl = this.gl;
        this.width = width;
        this.height = height;

        this.sceneColour = this.target(width, height, gl.RGBA16F, gl.RGBA, gl.HALF_FLOAT,
                                       gl.LINEAR);
        this.sceneDepth = gl.createRenderbuffer();
        gl.bindRenderbuffer(gl.RENDERBUFFER, this.sceneDepth);
        // Stencil as well as depth: the cut face is a parity pass and there is nowhere else for it
        // to count. A scene target without one silently loses the cap.
        gl.renderbufferStorage(gl.RENDERBUFFER, gl.DEPTH24_STENCIL8, width, height);

        this.sceneFbo = gl.createFramebuffer();
        gl.bindFramebuffer(gl.FRAMEBUFFER, this.sceneFbo);
        gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D,
                                this.sceneColour, 0);
        gl.framebufferRenderbuffer(gl.FRAMEBUFFER, gl.DEPTH_STENCIL_ATTACHMENT, gl.RENDERBUFFER,
                                   this.sceneDepth);
        if (this.flags.bloom) {
            // R11F_G11F_B10F is an RGB format and takes RGB on declaration; asking for RGBA with
            // it is an INVALID_OPERATION that costs nothing visible and leaves the texture with no
            // storage at all, which is the sort of failure that only shows up as a wrong picture.
            const packed = this.bloomFormat === 0x8C3A;
            this.sceneEmissive = this.target(width, height, this.bloomFormat,
                                             packed ? gl.RGB : gl.RGBA,
                                             packed ? gl.FLOAT : gl.HALF_FLOAT, gl.LINEAR);
            gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT1, gl.TEXTURE_2D,
                                    this.sceneEmissive, 0);
        }
        if (gl.checkFramebufferStatus(gl.FRAMEBUFFER) !== gl.FRAMEBUFFER_COMPLETE) {
            // A target the driver will not draw into is not something to limp along with: the
            // viewer falls back to the four in-shader tone maps and says nothing else went wrong.
            this.available = false;
            this.dropTargets();
            gl.bindFramebuffer(gl.FRAMEBUFFER, null);
            return;
        }

        // The eight-bit copy FXAA reads. Only allocated when FXAA is on — without it the composite
        // draws straight to the screen and there is one full-screen pass fewer in the frame.
        if (this.flags.fxaa) {
            this.ldrColour = this.target(width, height, gl.RGBA8, gl.RGBA, gl.UNSIGNED_BYTE,
                                         gl.LINEAR);
            this.ldrFbo = gl.createFramebuffer();
            gl.bindFramebuffer(gl.FRAMEBUFFER, this.ldrFbo);
            gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D,
                                    this.ldrColour, 0);
        }
        gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    }

    buildChain() {
        const gl = this.gl;
        const wanted = this.flags.bloom ? this.flags.bloomMips : 0;
        if (this.chainWanted === wanted && this.chain.length) return;
        for (const level of this.chain) {
            gl.deleteFramebuffer(level.fbo);
            gl.deleteTexture(level.texture);
        }
        this.chain = [];
        this.chainWanted = wanted;
        if (!wanted) return;
        let w = Math.max(1, this.width >> Math.log2(this.flags.bloomBase));
        let h = Math.max(1, this.height >> Math.log2(this.flags.bloomBase));
        const packed = this.bloomFormat === 0x8C3A;
        for (let i = 0; i < wanted && w > 4 && h > 4; ++i) {
            const texture = this.target(w, h, this.bloomFormat, packed ? gl.RGB : gl.RGBA,
                                        packed ? gl.FLOAT : gl.HALF_FLOAT, gl.LINEAR);
            const fbo = gl.createFramebuffer();
            gl.bindFramebuffer(gl.FRAMEBUFFER, fbo);
            gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D,
                                    texture, 0);
            this.chain.push({ texture, fbo, width: w, height: h });
            w = Math.max(1, w >> 1);
            h = Math.max(1, h >> 1);
        }
        gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    }

    // Point the scene at our own target. Returns false when there is nothing to point it at, in
    // which case the caller draws to the screen and tone maps in its own shaders as it always did.
    beginScene(width, height) {
        if (!this.available) return false;
        this.resize(width, height);
        if (!this.available) return false;
        this.buildChain();
        const gl = this.gl;
        gl.bindFramebuffer(gl.FRAMEBUFFER, this.sceneFbo);
        gl.drawBuffers(this.flags.bloom
            ? [gl.COLOR_ATTACHMENT0, gl.COLOR_ATTACHMENT1]
            : [gl.COLOR_ATTACHMENT0]);
        gl.viewport(0, 0, width, height);
        this.active = true;
        return true;
    }

    blit(program, uniforms) {
        const gl = this.gl;
        gl.useProgram(program.program);
        if (uniforms) uniforms(program.uniforms);
        gl.drawArrays(gl.TRIANGLES, 0, 3);
    }

    // Bloom, composite, and the edges. Everything from here to the end of the function is the
    // whole of what this file costs, and `budget` brackets it in two named passes so that cost is
    // a number in the readout rather than an opinion.
    endScene() {
        if (!this.active) return;
        this.active = false;
        const gl = this.gl;
        const budget = this.budget;

        gl.bindVertexArray(this.empty);
        gl.disable(gl.DEPTH_TEST);
        gl.disable(gl.CULL_FACE);
        gl.disable(gl.STENCIL_TEST);
        gl.disable(gl.BLEND);
        gl.depthMask(false);

        const bloom = this.flags.bloom && this.chain.length > 0;
        if (bloom) {
            if (budget) budget.begin('bloom');
            // Down: the emissive target through the knee into the first mip, then a box at a time.
            for (let i = 0; i < this.chain.length; ++i) {
                const level = this.chain[i];
                const source = i === 0 ? this.sceneEmissive : this.chain[i - 1].texture;
                const sw = i === 0 ? this.width : this.chain[i - 1].width;
                const sh = i === 0 ? this.height : this.chain[i - 1].height;
                gl.bindFramebuffer(gl.FRAMEBUFFER, level.fbo);
                gl.viewport(0, 0, level.width, level.height);
                gl.activeTexture(gl.TEXTURE0);
                gl.bindTexture(gl.TEXTURE_2D, source);
                if (i === 0) {
                    this.blit(this.prefilter, (u) => {
                        gl.uniform1i(u.u_source, 0);
                        gl.uniform1f(u.u_knee, this.bloomKnee);
                    });
                } else {
                    this.blit(this.down, (u) => {
                        gl.uniform1i(u.u_source, 0);
                        gl.uniform2f(u.u_texel, 0.5 / sw, 0.5 / sh);
                    });
                }
            }
            // ...and back up, added on with the blender. Each level arrives at twice its size
            // through a tent, which is what turns a chain of boxes into one smooth spill.
            gl.enable(gl.BLEND);
            gl.blendFunc(gl.ONE, gl.ONE);
            for (let i = this.chain.length - 1; i > 0; --i) {
                const source = this.chain[i];
                const into = this.chain[i - 1];
                gl.bindFramebuffer(gl.FRAMEBUFFER, into.fbo);
                gl.viewport(0, 0, into.width, into.height);
                gl.activeTexture(gl.TEXTURE0);
                gl.bindTexture(gl.TEXTURE_2D, source.texture);
                this.blit(this.up, (u) => {
                    gl.uniform1i(u.u_source, 0);
                    gl.uniform2f(u.u_texel, 1 / source.width, 1 / source.height);
                });
            }
            gl.disable(gl.BLEND);
            if (budget) budget.end();
        }

        if (budget) budget.begin(this.flags.fxaa ? 'composite+fxaa' : 'composite');
        const toScreen = !this.flags.fxaa || !this.ldrFbo;
        gl.bindFramebuffer(gl.FRAMEBUFFER, toScreen ? null : this.ldrFbo);
        gl.viewport(0, 0, this.width, this.height);
        gl.activeTexture(gl.TEXTURE0);
        gl.bindTexture(gl.TEXTURE_2D, this.sceneColour);
        gl.activeTexture(gl.TEXTURE1);
        gl.bindTexture(gl.TEXTURE_2D, bloom ? this.chain[0].texture : this.sceneColour);
        this.blit(this.composite, (u) => {
            gl.uniform1i(u.u_scene, 0);
            gl.uniform1i(u.u_bloom, 1);
            gl.uniform1f(u.u_exposure, this.exposure);
            gl.uniform1f(u.u_bloomStrength, this.bloomStrength);
            gl.uniform1i(u.u_hasBloom, bloom ? 1 : 0);
        });

        if (!toScreen) {
            gl.bindFramebuffer(gl.FRAMEBUFFER, null);
            gl.viewport(0, 0, this.width, this.height);
            gl.activeTexture(gl.TEXTURE0);
            gl.bindTexture(gl.TEXTURE_2D, this.ldrColour);
            this.blit(this.fxaa, (u) => {
                gl.uniform1i(u.u_source, 0);
                gl.uniform2f(u.u_texel, 1 / this.width, 1 / this.height);
            });
        }
        if (budget) budget.end();

        gl.bindVertexArray(null);
        gl.depthMask(true);
        gl.activeTexture(gl.TEXTURE0);
    }
}
