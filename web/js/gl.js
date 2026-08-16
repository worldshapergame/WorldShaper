// The rasteriser.
//
// The game renders these clips with a path tracer, and a phone cannot. So this draws the same
// matter with the same materials the honest way a rasteriser can: one instanced quad per merged
// voxel face, a Cook-Torrance lobe per light, and every term that needs to know what is between a
// surface and the sky read out of the light grid the baker cast.
//
// What it deliberately does NOT do is fake the path tracer. There are no screen-space reflections,
// no ambient occlusion pass, no denoiser. A mirror is a rough metal with a sky in it, glass is
// blended, and neither claims to be what the game shows. What it IS faithful about is the matter:
// the geometry is the clip's own voxels and the materials are its own VisualRecords, unquantised.
//
// # Three passes and a cap
//
//   sky          a full-screen gradient with the sun in it
//   opaque       depth written; back faces culled only when nothing is sliced
//   transparent  blended, depth tested, depth not written
//
// The slice is a clip plane and nothing is drawn on the cut. There WAS a third pass that filled
// the cross-section with a flat quad, found by inverting the stencil over the geometry behind the
// plane, so a sliced wall read as solid stone instead of as a sheet of paper. It went because it
// paints over the one thing the slider exists to show. Cutting into a building now shows the
// building's own faces, from behind, which is what the inside of a voxel clip actually looks
// like.

const VERTEX_SOURCE = `#version 300 es
precision highp float;

layout(location = 0) in vec3 a_cell;
layout(location = 1) in vec2 a_size;
layout(location = 2) in float a_material;
layout(location = 3) in float a_ao;

uniform mat4 u_viewProj;
uniform vec3 u_origin;     // metres at cell (0, 0, 0)
uniform float u_scale;     // metres per voxel
uniform vec3 u_offset;     // the face's own axis, for the three faces on the far side of a cell
uniform vec3 u_eu;         // the face's two in-plane axes, in the order the baker merged along
uniform vec3 u_ev;
uniform float u_flip;      // 1 on a negative face, which is what keeps the winding consistent

out vec3 v_world;
out float v_ao;
flat out int v_material;

void main() {
    vec2 corner = vec2(float(gl_VertexID & 1), float((gl_VertexID >> 1) & 1));
    vec2 uv = (u_flip > 0.5) ? corner.yx : corner;

    vec3 cell = a_cell + u_offset + u_eu * (uv.x * a_size.x) + u_ev * (uv.y * a_size.y);
    v_world = u_origin + cell * u_scale;

    int packed = int(a_ao + 0.5);
    int which = int(uv.x) + 2 * int(uv.y);
    v_ao = float((packed >> (which * 2)) & 3) / 3.0;

    v_material = int(a_material + 0.5);
    gl_Position = u_viewProj * vec4(v_world, 1.0);
}`;

// The shading. Every constant in here is a look rather than a measurement, and it is written to
// flatter stone in daylight because that is what the facility is.
const FRAGMENT_SOURCE = `#version 300 es
precision highp float;
precision highp sampler3D;

in vec3 v_world;
in float v_ao;
flat in int v_material;

uniform sampler2D u_materials;
uniform sampler3D u_light;
uniform vec3 u_lightOrigin;
uniform vec3 u_lightScale;    // 1 / (light grid size in metres)
uniform vec3 u_lightTexel;    // half a texel, so a fetch lands on a point and not between two
uniform float u_lightBias;    // how far along the normal to sample, in metres

uniform vec3 u_normal;
uniform vec3 u_eye;
uniform vec3 u_sun;
uniform vec3 u_sunColour;
uniform vec3 u_skyUp;
uniform vec3 u_skyDown;
uniform float u_exposure;
uniform vec4 u_clip;          // dot(world, xyz) + w > 0 is cut away
uniform float u_cutSide;      // 1 draws only what the slice removes, for the stencil pass
uniform float u_blended;      // 1 on the blended pass, where the material's opacity is used

out vec4 o_colour;

vec4 material_row(int which) {
    int at = v_material * 4 + which;
    return texelFetch(u_materials, ivec2(at & 255, at >> 8), 0);
}

float ggx(float ndh, float rough) {
    float a = rough * rough;
    float d = ndh * ndh * (a * a - 1.0) + 1.0;
    return (a * a) / (3.14159265 * d * d + 1e-7);
}

float smith(float ndv, float ndl, float rough) {
    float k = (rough + 1.0) * (rough + 1.0) / 8.0;
    float gv = ndv / (ndv * (1.0 - k) + k);
    float gl = ndl / (ndl * (1.0 - k) + k);
    return gv * gl;
}

vec3 fresnel(vec3 f0, float vdh) {
    float f = pow(1.0 - vdh, 5.0);
    return f0 + (vec3(1.0) - f0) * f;
}

vec3 sky_colour(vec3 direction) {
    float up = clamp(direction.y * 0.5 + 0.5, 0.0, 1.0);
    vec3 base = mix(u_skyDown, u_skyUp, up);
    // The sun's own disc, and a wide glow round it. Both are what a rough metal reflects, and
    // without them a mirror in this viewer reflects a flat blue card.
    float towards = max(dot(direction, u_sun), 0.0);
    base += u_sunColour * pow(towards, 400.0) * 3.0;
    base += u_sunColour * pow(towards, 8.0) * 0.05;
    return base;
}

// ACES, in the fitted form. A path tracer's output needs a curve like this and so does anything
// with a sun in it: without one every lit face of pale limestone is the same clipped white.
vec3 tonemap(vec3 x) {
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

void main() {
    float side = dot(v_world, u_clip.xyz) + u_clip.w;
    if (u_cutSide > 0.5) {
        if (side <= 0.0) discard;
    } else if (side > 0.0) {
        discard;
    }

    vec4 base = material_row(0);
    vec4 surface = material_row(1);
    vec4 depth = material_row(2);
    vec4 extra = material_row(3);

    vec3 albedo = base.rgb * base.rgb;            // close enough to sRGB, and one multiply
    float opacity = base.a;
    float rough = clamp(surface.r, 0.045, 1.0);
    float metal = surface.g;
    float iorByte = surface.b * 255.0;
    float emissive = surface.a;
    float translucency = depth.a;

    int flags = int(extra.b * 255.0 + 0.5);
    int coat = int(extra.a * 255.0 + 0.5);
    float clearcoat = float(coat & 15) / 15.0;
    float sheen = float((coat >> 4) & 15) / 15.0;

    vec3 N = u_normal;
    vec3 V = normalize(u_eye - v_world);
    if (dot(N, V) < 0.0) N = -N;                  // the far side of a pane, seen through the near
    vec3 L = u_sun;
    vec3 H = normalize(L + V);

    float ndl = max(dot(N, L), 0.0);
    float ndv = max(dot(N, V), 1e-4);
    float ndh = max(dot(N, H), 0.0);
    float vdh = max(dot(V, H), 0.0);

    // What the baker cast: how much of the sun and how much of the sky reach this place. Sampled
    // a little along the normal so that a face reads the air in front of it rather than the stone
    // it is the surface of.
    vec3 at = (v_world + N * u_lightBias - u_lightOrigin) * u_lightScale;
    at = clamp(at + u_lightTexel, vec3(0.0), vec3(1.0));
    vec2 visible = texture(u_light, at).rg;
    float sunVisible = visible.r;
    float skyVisible = visible.g;

    // Corner occlusion is a voxel's own shape and the light grid is the room it stands in; both
    // are needed, and neither substitutes for the other.
    float occluded = mix(0.35, 1.0, v_ao) * mix(0.25, 1.0, skyVisible);

    float dielectric = (iorByte > 0.5)
        ? pow((iorByte / 128.0) / (2.0 + iorByte / 128.0), 2.0)
        : 0.04;
    vec3 f0 = mix(vec3(dielectric), albedo, metal);

    vec3 ambient = mix(u_skyDown, u_skyUp, clamp(N.y * 0.5 + 0.5, 0.0, 1.0)) * 0.5;
    vec3 direct = u_sunColour * ndl * sunVisible;

    vec3 diffuse = albedo * (1.0 - metal) * (direct + ambient * occluded);

    // Translucent matter lights from behind: leaves, thin marble, wax. One term, and it is the
    // only place in this shader where light arrives through something.
    if (translucency > 0.0) {
        float through = max(dot(-N, L), 0.0) * 0.6 + 0.25;
        diffuse += albedo * translucency * through * u_sunColour * sunVisible;
    }

    // Brushed metal. A voxel world has no UVs and no tangent frame, so a VisualRecord can name
    // exactly one thing here — which of the three world axes the grain runs along — and that is
    // what bits 3 and 4 hold. The grain is projected into the face and the highlight is stretched
    // across it, which is the whole visible difference between brushed bronze and brown paint.
    // On a face the grain runs straight out of, there is no direction left and it stays round.
    float distribution = ggx(ndh, rough);
    int brush = (flags >> 3) & 3;
    if (brush != 0) {
        vec3 axis = (brush == 1) ? vec3(1.0, 0.0, 0.0)
                  : (brush == 2) ? vec3(0.0, 1.0, 0.0) : vec3(0.0, 0.0, 1.0);
        vec3 along = axis - N * dot(N, axis);
        float reach = length(along);
        if (reach > 0.05) {
            along /= reach;
            vec3 across = cross(N, along);
            float alpha = rough * rough;
            float ax = max(alpha * 0.35, 0.002);
            float ay = max(alpha * 1.60, 0.002);
            float th = dot(along, H) / ax;
            float bh = dot(across, H) / ay;
            float d = th * th + bh * bh + ndh * ndh;
            distribution = 1.0 / (3.14159265 * ax * ay * d * d + 1e-7);
        }
    }

    vec3 specular = fresnel(f0, vdh) * distribution * smith(ndv, ndl, rough) /
                    (4.0 * ndv * max(ndl, 1e-4) + 1e-4) * direct * ndl;

    // The sky, reflected. A rough surface takes a blurred sky, which here is the sky colour along
    // the reflection lerped towards the flat ambient — cheap, and it is what makes bronze read as
    // metal rather than as brown paint.
    vec3 R = reflect(-V, N);
    vec3 reflected = mix(sky_colour(R), ambient, rough * rough);
    vec3 ambientSpecular = reflected * fresnel(f0, ndv) * mix(0.25, 1.0, skyVisible) *
                           mix(0.4, 1.0, v_ao);

    vec3 colour = diffuse + specular + ambientSpecular;

    if (clearcoat > 0.0) {
        float lacquer = ggx(ndh, 0.1) * smith(ndv, ndl, 0.1) / (4.0 * ndv * max(ndl, 1e-4) + 1e-4);
        colour += clearcoat * 0.25 * (lacquer * direct * ndl + reflected * 0.15 * skyVisible);
    }
    if (sheen > 0.0) {
        float rim = pow(1.0 - ndv, 3.0);
        colour += sheen * rim * albedo * (ambient * skyVisible + direct * 0.3);
    }
    if (emissive > 0.0) {
        int tint = int(extra.r * 255.0 + 0.5) | (int(extra.g * 255.0 + 0.5) << 8);
        vec3 glow = vec3(float((tint >> 11) & 31) / 31.0,
                         float((tint >> 5) & 63) / 63.0,
                         float(tint & 31) / 31.0);
        colour += glow * emissive * 6.0;
    }

    // Beer-Lambert, over a thickness nobody knows. A rasteriser has no path length, so this takes
    // the material's absorption as a tint on what shows through and says so rather than pretending
    // to integrate anything.
    float alpha = (u_blended < 0.5) ? 1.0 : opacity;
    if (alpha < 1.0) {
        vec3 absorb = vec3(1.0) - depth.rgb * 0.75;
        colour *= absorb;
        // Glancing angles of a pane are more reflective and less see-through, which is the one
        // thing about glass everybody notices when it is missing.
        alpha = clamp(alpha + (1.0 - alpha) * pow(1.0 - ndv, 4.0), 0.0, 1.0);
    }

    o_colour = vec4(pow(tonemap(colour * u_exposure), vec3(1.0 / 2.2)), alpha);
}`;

const SKY_VERTEX = `#version 300 es
precision highp float;
uniform mat4 u_invViewProj;
uniform vec3 u_eye;
out vec3 v_ray;
void main() {
    vec2 corner = vec2(float((gl_VertexID & 1) * 4 - 1), float((gl_VertexID >> 1) * 4 - 1));
    vec4 far = u_invViewProj * vec4(corner, 1.0, 1.0);
    v_ray = far.xyz / far.w - u_eye;
    gl_Position = vec4(corner, 1.0, 1.0);
}`;

const SKY_FRAGMENT = `#version 300 es
precision highp float;
in vec3 v_ray;
uniform vec3 u_sun;
uniform vec3 u_sunColour;
uniform vec3 u_skyUp;
uniform vec3 u_skyDown;
uniform float u_exposure;
out vec4 o_colour;
vec3 tonemap(vec3 x) {
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}
void main() {
    vec3 direction = normalize(v_ray);
    float up = clamp(direction.y * 0.5 + 0.5, 0.0, 1.0);
    vec3 colour = mix(u_skyDown, u_skyUp, up);
    float towards = max(dot(direction, u_sun), 0.0);
    colour += u_sunColour * pow(towards, 900.0) * 4.0;
    colour += u_sunColour * pow(towards, 8.0) * 0.05;
    o_colour = vec4(pow(tonemap(colour * u_exposure), vec3(1.0 / 2.2)), 1.0);
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

// --- a very small amount of matrix arithmetic ------------------------------------------------

export function perspective(out, fovY, aspect, near, far) {
    const f = 1 / Math.tan(fovY / 2);
    out.fill(0);
    out[0] = f / aspect;
    out[5] = f;
    out[10] = (far + near) / (near - far);
    out[11] = -1;
    out[14] = (2 * far * near) / (near - far);
    return out;
}

export function lookAt(out, eye, at, up) {
    let zx = eye[0] - at[0], zy = eye[1] - at[1], zz = eye[2] - at[2];
    let length = Math.hypot(zx, zy, zz) || 1;
    zx /= length; zy /= length; zz /= length;
    let xx = up[1] * zz - up[2] * zy, xy = up[2] * zx - up[0] * zz, xz = up[0] * zy - up[1] * zx;
    length = Math.hypot(xx, xy, xz) || 1;
    xx /= length; xy /= length; xz /= length;
    const yx = zy * xz - zz * xy, yy = zz * xx - zx * xz, yz = zx * xy - zy * xx;
    out[0] = xx; out[1] = yx; out[2] = zx; out[3] = 0;
    out[4] = xy; out[5] = yy; out[6] = zy; out[7] = 0;
    out[8] = xz; out[9] = yz; out[10] = zz; out[11] = 0;
    out[12] = -(xx * eye[0] + xy * eye[1] + xz * eye[2]);
    out[13] = -(yx * eye[0] + yy * eye[1] + yz * eye[2]);
    out[14] = -(zx * eye[0] + zy * eye[1] + zz * eye[2]);
    out[15] = 1;
    return out;
}

export function multiply(out, a, b) {
    for (let c = 0; c < 4; ++c) {
        const b0 = b[c * 4], b1 = b[c * 4 + 1], b2 = b[c * 4 + 2], b3 = b[c * 4 + 3];
        out[c * 4 + 0] = a[0] * b0 + a[4] * b1 + a[8] * b2 + a[12] * b3;
        out[c * 4 + 1] = a[1] * b0 + a[5] * b1 + a[9] * b2 + a[13] * b3;
        out[c * 4 + 2] = a[2] * b0 + a[6] * b1 + a[10] * b2 + a[14] * b3;
        out[c * 4 + 3] = a[3] * b0 + a[7] * b1 + a[11] * b2 + a[15] * b3;
    }
    return out;
}

export function invert(out, m) {
    const a00 = m[0], a01 = m[1], a02 = m[2], a03 = m[3];
    const a10 = m[4], a11 = m[5], a12 = m[6], a13 = m[7];
    const a20 = m[8], a21 = m[9], a22 = m[10], a23 = m[11];
    const a30 = m[12], a31 = m[13], a32 = m[14], a33 = m[15];
    const b00 = a00 * a11 - a01 * a10, b01 = a00 * a12 - a02 * a10;
    const b02 = a00 * a13 - a03 * a10, b03 = a01 * a12 - a02 * a11;
    const b04 = a01 * a13 - a03 * a11, b05 = a02 * a13 - a03 * a12;
    const b06 = a20 * a31 - a21 * a30, b07 = a20 * a32 - a22 * a30;
    const b08 = a20 * a33 - a23 * a30, b09 = a21 * a32 - a22 * a31;
    const b10 = a21 * a33 - a23 * a31, b11 = a22 * a33 - a23 * a32;
    let det = b00 * b11 - b01 * b10 + b02 * b09 + b03 * b08 - b04 * b07 + b05 * b06;
    if (!det) return out;
    det = 1 / det;
    out[0] = (a11 * b11 - a12 * b10 + a13 * b09) * det;
    out[1] = (a02 * b10 - a01 * b11 - a03 * b09) * det;
    out[2] = (a31 * b05 - a32 * b04 + a33 * b03) * det;
    out[3] = (a22 * b04 - a21 * b05 - a23 * b03) * det;
    out[4] = (a12 * b08 - a10 * b11 - a13 * b07) * det;
    out[5] = (a00 * b11 - a02 * b08 + a03 * b07) * det;
    out[6] = (a32 * b02 - a30 * b05 - a33 * b01) * det;
    out[7] = (a20 * b05 - a22 * b02 + a23 * b01) * det;
    out[8] = (a10 * b10 - a11 * b08 + a13 * b06) * det;
    out[9] = (a01 * b08 - a00 * b10 - a03 * b06) * det;
    out[10] = (a30 * b04 - a31 * b02 + a33 * b00) * det;
    out[11] = (a21 * b02 - a20 * b04 - a23 * b00) * det;
    out[12] = (a11 * b07 - a10 * b09 - a12 * b06) * det;
    out[13] = (a00 * b09 - a01 * b07 + a02 * b06) * det;
    out[14] = (a31 * b01 - a30 * b03 - a32 * b00) * det;
    out[15] = (a20 * b03 - a21 * b01 + a22 * b00) * det;
    return out;
}

// The six faces, matching bake_web.cpp's order: +X -X +Y -Y +Z -Z.
const FACES = [
    { normal: [1, 0, 0], offset: [1, 0, 0], eu: [0, 1, 0], ev: [0, 0, 1], flip: 0 },
    { normal: [-1, 0, 0], offset: [0, 0, 0], eu: [0, 1, 0], ev: [0, 0, 1], flip: 1 },
    { normal: [0, 1, 0], offset: [0, 1, 0], eu: [0, 0, 1], ev: [1, 0, 0], flip: 0 },
    { normal: [0, -1, 0], offset: [0, 0, 0], eu: [0, 0, 1], ev: [1, 0, 0], flip: 1 },
    { normal: [0, 0, 1], offset: [0, 0, 1], eu: [1, 0, 0], ev: [0, 1, 0], flip: 0 },
    { normal: [0, 0, -1], offset: [0, 0, 0], eu: [1, 0, 0], ev: [0, 1, 0], flip: 1 },
];

export class Renderer {
    constructor(canvas) {
        const gl = canvas.getContext('webgl2', {
            alpha: false,
            antialias: true,
            depth: true,
            stencil: true,
            powerPreference: 'high-performance',
            preserveDrawingBuffer: false,
        });
        if (!gl) throw new Error('this browser has no WebGL 2');
        this.canvas = canvas;
        this.gl = gl;

        this.surface = link(gl, VERTEX_SOURCE, FRAGMENT_SOURCE, 'surface');
        this.sky = link(gl, SKY_VERTEX, SKY_FRAGMENT, 'sky');

        this.vao = gl.createVertexArray();
        this.buffer = gl.createBuffer();
        this.materials = gl.createTexture();
        this.light = gl.createTexture();
        this.clip = null;

        this.viewProj = new Float32Array(16);
        this.invViewProj = new Float32Array(16);
        this.projection = new Float32Array(16);
        this.view = new Float32Array(16);

        // A sun at fifty degrees in the south-east, which is where the baker cast its shadows
        // from. The two have to agree or a shadow lies on the ground pointing the wrong way.
        this.sun = [0.42, 0.80, -0.43];
        const length = Math.hypot(this.sun[0], this.sun[1], this.sun[2]);
        this.sun = this.sun.map((v) => v / length);
        // Radiance, not a colour picker's idea of one. The sun is three times the sky it stands
        // in, which is the ratio that makes a lit face and a shaded face look like the same stone
        // in two places rather than like two materials — the first attempt had them within a fifth
        // of each other and the whole building read as a paper model.
        this.sunColour = [3.30, 3.10, 2.78];
        this.skyUp = [0.30, 0.47, 0.92];
        this.skyDown = [0.62, 0.67, 0.74];
        this.exposure = 1.0;

        this.stats = { draws: 0, quads: 0 };
    }

    // Everything that lands on the card for one clip: one vertex buffer, one material texture, one
    // light volume. Called again when the clip is rebuilt, so it frees before it allocates.
    setClip(clip) {
        const gl = this.gl;
        this.clip = clip;

        gl.bindVertexArray(this.vao);
        gl.bindBuffer(gl.ARRAY_BUFFER, this.buffer);
        const bytes = clip.opaque.byteLength + clip.transparent.byteLength;
        gl.bufferData(gl.ARRAY_BUFFER, bytes, gl.STATIC_DRAW);
        if (clip.opaque.byteLength > 0) gl.bufferSubData(gl.ARRAY_BUFFER, 0, clip.opaque);
        if (clip.transparent.byteLength > 0) {
            gl.bufferSubData(gl.ARRAY_BUFFER, clip.opaque.byteLength, clip.transparent);
        }
        this.transparentBase = clip.opaque.byteLength;
        for (let i = 0; i < 4; ++i) {
            gl.enableVertexAttribArray(i);
            gl.vertexAttribDivisor(i, 1);
        }
        gl.bindVertexArray(null);

        // Materials, verbatim: each VisualRecord is four RGBA8 texels and the shader fetches the
        // row it wants. Nothing is converted on the way in, which is the point — the browser reads
        // the same sixteen bytes the world stores.
        const texels = clip.materialCount * 4;
        const width = 256;
        const height = Math.max(1, Math.ceil(texels / width));
        const padded = new Uint8Array(width * height * 4);
        padded.set(clip.materials.subarray(0, Math.min(clip.materials.length, padded.length)));
        gl.bindTexture(gl.TEXTURE_2D, this.materials);
        gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);
        gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA8, width, height, 0, gl.RGBA, gl.UNSIGNED_BYTE,
                      padded);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);

        gl.bindTexture(gl.TEXTURE_3D, this.light);
        gl.texImage3D(gl.TEXTURE_3D, 0, gl.RG8, clip.lightDims[0], clip.lightDims[1],
                      clip.lightDims[2], 0, gl.RG, gl.UNSIGNED_BYTE, clip.light);
        gl.texParameteri(gl.TEXTURE_3D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
        gl.texParameteri(gl.TEXTURE_3D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
        gl.texParameteri(gl.TEXTURE_3D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
        gl.texParameteri(gl.TEXTURE_3D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
        gl.texParameteri(gl.TEXTURE_3D, gl.TEXTURE_WRAP_R, gl.CLAMP_TO_EDGE);
    }

    attributesAt(byteOffset) {
        const gl = this.gl;
        gl.bindBuffer(gl.ARRAY_BUFFER, this.buffer);
        gl.vertexAttribPointer(0, 3, gl.UNSIGNED_SHORT, false, 16, byteOffset + 0);
        gl.vertexAttribPointer(1, 2, gl.UNSIGNED_SHORT, false, 16, byteOffset + 6);
        gl.vertexAttribPointer(2, 1, gl.UNSIGNED_SHORT, false, 16, byteOffset + 10);
        gl.vertexAttribPointer(3, 1, gl.UNSIGNED_BYTE, false, 16, byteOffset + 12);
    }

    // One pass over the mesh: six ranges, one per face, each with its own normal and basis.
    drawFaces(uniforms, starts, base, blended) {
        const gl = this.gl;
        for (let face = 0; face < 6; ++face) {
            const count = starts[face + 1] - starts[face];
            if (count === 0) continue;
            const f = FACES[face];
            gl.uniform3fv(uniforms.u_normal, f.normal);
            gl.uniform3fv(uniforms.u_offset, f.offset);
            gl.uniform3fv(uniforms.u_eu, f.eu);
            gl.uniform3fv(uniforms.u_ev, f.ev);
            gl.uniform1f(uniforms.u_flip, f.flip);
            if (uniforms.u_blended) gl.uniform1f(uniforms.u_blended, blended);
            this.attributesAt(base + starts[face] * 16);
            gl.drawArraysInstanced(gl.TRIANGLE_STRIP, 0, 4, count);
            this.stats.draws += 1;
            this.stats.quads += count;
        }
    }

    setShared(uniforms, camera) {
        const gl = this.gl;
        const clip = this.clip;
        gl.uniformMatrix4fv(uniforms.u_viewProj, false, this.viewProj);
        gl.uniform3fv(uniforms.u_origin, clip.origin);
        gl.uniform1f(uniforms.u_scale, 1 / clip.metre);
        gl.uniform3fv(uniforms.u_eye, camera.eye);
        gl.uniform3fv(uniforms.u_sun, this.sun);
        gl.uniform3fv(uniforms.u_sunColour, this.sunColour);
        gl.uniform3fv(uniforms.u_skyUp, this.skyUp);
        gl.uniform3fv(uniforms.u_skyDown, this.skyDown);
        gl.uniform1f(uniforms.u_exposure, this.exposure);

        const size = [
            clip.lightDims[0] * clip.lightCell,
            clip.lightDims[1] * clip.lightCell,
            clip.lightDims[2] * clip.lightCell,
        ];
        gl.uniform3fv(uniforms.u_lightOrigin, clip.origin);
        gl.uniform3fv(uniforms.u_lightScale, [1 / size[0], 1 / size[1], 1 / size[2]]);
        gl.uniform3fv(uniforms.u_lightTexel, [
            0.5 / clip.lightDims[0], 0.5 / clip.lightDims[1], 0.5 / clip.lightDims[2],
        ]);
        gl.uniform1f(uniforms.u_lightBias, clip.lightCell);

        gl.activeTexture(gl.TEXTURE0);
        gl.bindTexture(gl.TEXTURE_2D, this.materials);
        gl.uniform1i(uniforms.u_materials, 0);
        gl.activeTexture(gl.TEXTURE1);
        gl.bindTexture(gl.TEXTURE_3D, this.light);
        gl.uniform1i(uniforms.u_light, 1);
    }

    // `slice` is null, or { axis: 0..2, sign: +1 | -1, at: metres }. The plane keeps everything on
    // the side the sign points away from, so dragging the slider walks the cut through the clip.
    render(camera, slice) {
        const gl = this.gl;
        const clip = this.clip;
        this.stats.draws = 0;
        this.stats.quads = 0;
        if (!clip) return;

        const width = this.canvas.width;
        const height = this.canvas.height;
        gl.viewport(0, 0, width, height);

        perspective(this.projection, camera.fov, width / Math.max(1, height), camera.near,
                    camera.far);
        lookAt(this.view, camera.eye, camera.at, [0, 1, 0]);
        multiply(this.viewProj, this.projection, this.view);
        invert(this.invViewProj, this.viewProj);

        gl.clearColor(0, 0, 0, 1);
        gl.clearDepth(1);
        gl.clearStencil(0);
        gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT | gl.STENCIL_BUFFER_BIT);

        // --- sky -------------------------------------------------------------------------------
        gl.disable(gl.DEPTH_TEST);
        gl.disable(gl.CULL_FACE);
        gl.disable(gl.BLEND);
        gl.disable(gl.STENCIL_TEST);
        gl.useProgram(this.sky.program);
        gl.uniformMatrix4fv(this.sky.uniforms.u_invViewProj, false, this.invViewProj);
        gl.uniform3fv(this.sky.uniforms.u_eye, camera.eye);
        gl.uniform3fv(this.sky.uniforms.u_sun, this.sun);
        gl.uniform3fv(this.sky.uniforms.u_sunColour, this.sunColour);
        gl.uniform3fv(this.sky.uniforms.u_skyUp, this.skyUp);
        gl.uniform3fv(this.sky.uniforms.u_skyDown, this.skyDown);
        gl.uniform1f(this.sky.uniforms.u_exposure, this.exposure);
        gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);

        // dot(world, xyz) + w > 0 is cut away. With no slice the plane is put where nothing can
        // reach it, so the same shader runs either way and there is no branch to get wrong.
        const plane = [0, 0, 0, -1];
        if (slice) {
            plane[slice.axis] = slice.sign;
            plane[3] = -slice.sign * slice.at;
        }

        gl.bindVertexArray(this.vao);
        gl.useProgram(this.surface.program);
        this.setShared(this.surface.uniforms, camera);
        gl.uniform4fv(this.surface.uniforms.u_clip, plane);
        gl.uniform1f(this.surface.uniforms.u_cutSide, 0);

        // --- opaque ----------------------------------------------------------------------------
        gl.enable(gl.DEPTH_TEST);
        gl.depthFunc(gl.LESS);
        gl.depthMask(true);
        gl.frontFace(gl.CCW);
        gl.cullFace(gl.BACK);
        // Back faces are what the inside of a clip IS. Unsliced, nothing can see one and culling
        // them is free; sliced, every surface the cut opens up is seen from behind, and culling
        // them is the difference between looking into a room and looking through the building.
        if (slice) {
            gl.disable(gl.CULL_FACE);
        } else {
            gl.enable(gl.CULL_FACE);
        }
        this.drawFaces(this.surface.uniforms, clip.opaqueFace, 0, 0);

        // Nothing is drawn ON the cut, and that is the change the slider was asked for.
        //
        // It used to fill the cross-section with a flat grey quad, found with a stencil parity
        // pass, so that a sliced wall read as solid stone rather than as a sheet of paper. That is
        // a defensible picture of a building and it is the wrong one HERE: it paints over exactly
        // the faces somebody dragged the slider to see, and what you get is a clean grey plane
        // where the inside of the clip should be. Reported as "make the slicing show the inside of
        // the clip, not culling the faces of sliced voxels", which is the whole of it.
        //
        // So the cut shows the mesh's own faces and the only thing that changes is that the pass
        // above stops culling: see `render`'s cull decision. A wall cut through shows its two
        // surfaces and the gap between them, because that is what the voxels are.

        // --- glass -----------------------------------------------------------------------------
        if (clip.transparentQuads > 0) {
            gl.enable(gl.BLEND);
            gl.blendFuncSeparate(gl.SRC_ALPHA, gl.ONE_MINUS_SRC_ALPHA, gl.ONE,
                                 gl.ONE_MINUS_SRC_ALPHA);
            gl.depthMask(false);
            if (slice) {
                gl.disable(gl.CULL_FACE);
            } else {
                gl.enable(gl.CULL_FACE);
            }
            this.drawFaces(this.surface.uniforms, clip.transparentFace, this.transparentBase, 1);
            gl.depthMask(true);
            gl.disable(gl.BLEND);
        }

        gl.bindVertexArray(null);
    }
}
