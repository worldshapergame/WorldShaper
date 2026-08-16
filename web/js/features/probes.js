// Baked reflection probes, read and sampled.
//
// `tools/bake/probes.hpp` is the other half of this file and says why probes exist, where they are
// placed and what a level means. This one reads the `RPRB` chunk, puts it on the card as two
// textures, and hands the fragment shader one function.
//
// # What the shader gets, and what the screen-space pass may call
//
//     vec4 probeReflection(vec3 world, vec3 normal, vec3 reflectDir, float roughness);
//
//         world       the shading point, in metres
//         normal      the surface normal, used only to refuse a probe standing behind the surface
//         reflectDir  where the reflection goes, normalised. Parallax-corrected inside.
//         roughness   0..1 straight off the material, NOT the clamped value the GGX lobe wants
//
//         returns     .rgb  linear radiance, pre-tonemap, at the sharpness `roughness` asks for
//                     .a    how much of the point's probe neighbourhood actually had probes in it.
//                           0 means fall back to the analytic sky; the caller decides how.
//
//     vec3 probeFresnel(vec3 f0, float ndv, float roughness);
//
//         Schlick with the roughness ceiling, which is the term that makes a rough floor
//         reflective at a grazing angle and not face on. Water reads as water because of it.
//
// A screen-space pass is welcome to call `probeReflection` for everything its own march could not
// find — off screen, behind the camera, or a ray that walked off the depth buffer — which on a
// mirror facing you is most of the image. `PROBE_GLSL` is a string; splice it into any fragment
// shader that also declares `precision highp float;`, then call `Probes.bind` with that program's
// uniform table before drawing.
//
// # Two textures, and why the index volume is one of them
//
// A probe IS a lattice point. The atlas holds what each one sees; the index volume says which
// lattice points became probes, as one 16-bit id a cell with 0xFFFF for "nothing here". So the
// shader finds the eight probes round a point with eight `texelFetch`es and no search, and the
// probe positions are never stored anywhere — a position derived twice is a position that can
// disagree with itself.
//
// # The atlas layout
//
// One tile a probe, laid out roughly square. Within a tile the roughness levels run left to right,
// each half the width of the one before, each with a one-texel border filled by the octahedral
// fold — so a bilinear fetch at the edge of a map reads the direction that is genuinely next to it,
// and no tile can bleed into its neighbour. There is no GPU mip chain: a mip of an atlas is exactly
// the bleed the border exists to prevent.

// Matched, field for field, by `probe_chunk` in tools/bake/probes.hpp.
export const PROBE_CHUNK_HEADER = 80;
export const PROBE_FOURCC = 'RPRB';

// TEXTURE0 is the material table and TEXTURE1 the light volume; these two are chosen high enough
// to leave the low units to whatever else the viewer grows.
export const PROBE_ATLAS_UNIT = 4;
export const PROBE_INDEX_UNIT = 5;

export function readProbes(buffer, entry) {
    if (!entry || entry.size < PROBE_CHUNK_HEADER) return null;
    const view = new DataView(buffer, entry.offset, entry.size);
    const probes = {
        count: view.getUint32(0, true),
        atlasWidth: view.getUint32(4, true),
        atlasHeight: view.getUint32(8, true),
        tileWidth: view.getUint32(12, true),
        tileHeight: view.getUint32(16, true),
        perRow: view.getUint32(20, true),
        base: view.getUint32(24, true),
        levels: view.getUint32(28, true),
        border: view.getUint32(32, true),
        spacing: view.getFloat32(36, true),
        dims: [view.getInt32(40, true), view.getInt32(44, true), view.getInt32(48, true)],
        gridOrigin: [view.getFloat32(52, true), view.getFloat32(56, true),
                     view.getFloat32(60, true)],
        range: view.getFloat32(64, true),
        curve: view.getFloat32(68, true),
    };
    if (probes.count === 0) return null;

    const indexAt = view.getUint32(72, true);
    const atlasAt = view.getUint32(76, true);
    const indexBytes = probes.dims[0] * probes.dims[1] * probes.dims[2] * 2;
    const atlasBytes = probes.atlasWidth * probes.atlasHeight * 4;
    // The chunk says how big it is and the blocks say where they start. If the two disagree the
    // file was written by a baker this reader does not match, and drawing something out of it is
    // worse than saying so.
    if (indexAt + indexBytes > entry.size || atlasAt + atlasBytes > entry.size) {
        throw new Error('probe chunk is ' + entry.size + ' bytes, its blocks need ' +
                        Math.max(indexAt + indexBytes, atlasAt + atlasBytes));
    }
    probes.index = new Uint8Array(buffer, entry.offset + indexAt, indexBytes);
    probes.atlas = new Uint8Array(buffer, entry.offset + atlasAt, atlasBytes);
    probes.bytes = entry.size;
    return probes;
}

// The GLSL. Spliced into a fragment shader that already has `precision highp float;` and an
// `out vec4`; it declares its own uniforms and nothing else.
export const PROBE_GLSL = `
precision highp usampler3D;

uniform sampler2D u_probeAtlas;
uniform usampler3D u_probeIndex;
uniform float u_probeCount;
uniform float u_probeSpacing;
uniform vec3 u_probeGridOrigin;
uniform ivec3 u_probeGridDims;
uniform vec2 u_probeAtlasSize;
uniform vec2 u_probeTile;       // tile width, tile height, in texels
uniform float u_probePerRow;
uniform float u_probeBase;
uniform float u_probeLevels;
uniform float u_probeBorder;
uniform float u_probeRange;     // radiance is stored as sqrt(value / range)
uniform float u_probeCurve;     // roughness = (level / (levels - 1)) ^ curve
uniform vec3 u_probeBoxLow;     // the clip's own box, for the parallax correction
uniform vec3 u_probeBoxHigh;

// sign(), except that sign(0) is +1 rather than 0. A direction lying exactly in a plane is common
// in a voxel world -- every face normal is one -- and mapping it to the middle of the octahedron
// instead of to its edge puts the reflection of a wall on the floor.
vec2 probeSignNotZero(vec2 v) {
    return vec2(v.x >= 0.0 ? 1.0 : -1.0, v.y >= 0.0 ? 1.0 : -1.0);
}

// The octahedral mapping, edge-aligned: uv 0 and 1 land on the first and last texel centres, so the
// square's boundary is exactly where the border texels fold from. probe_oct_decode in
// tools/bake/probes.hpp is the inverse of this and they are the only two places it is written.
vec2 probeOctEncode(vec3 d) {
    d /= (abs(d.x) + abs(d.y) + abs(d.z));
    vec2 uv = (d.y >= 0.0) ? d.xz : (1.0 - abs(d.zx)) * probeSignNotZero(d.xz);
    return uv * 0.5 + 0.5;
}

vec3 probeTexel(int probe, int level, vec3 dir) {
    float size = u_probeBase;
    float ox = 0.0;
    for (int l = 0; l < 8; ++l) {
        if (l >= level) break;
        ox += size + 2.0 * u_probeBorder;
        size = max(2.0, floor(size * 0.5));
    }
    float row = floor(float(probe) / u_probePerRow);
    float col = float(probe) - row * u_probePerRow;
    vec2 uv = probeOctEncode(dir);
    vec2 at = vec2(col * u_probeTile.x + ox, row * u_probeTile.y) + vec2(u_probeBorder) +
              uv * (size - 1.0) + vec2(0.5);
    // No mip chain on the atlas -- a mip would blend one probe into the next -- so the level is
    // explicit and the fetch is legal inside the branches below.
    vec3 stored = textureLod(u_probeAtlas, at / u_probeAtlasSize, 0.0).rgb;
    return stored * stored * u_probeRange;
}

// Parallax: aim the lookup at where the reflected ray actually lands on the clip's own box, seen
// from where the probe stands. Without it a reflection slides across a surface as the camera moves,
// which is the first thing anybody notices and the reason an uncorrected probe reads as a sticker.
vec3 probeParallax(vec3 world, vec3 R, vec3 probePos) {
    vec3 safe = mix(R, vec3(1e-4), lessThan(abs(R), vec3(1e-4)));
    vec3 t1 = (u_probeBoxLow - world) / safe;
    vec3 t2 = (u_probeBoxHigh - world) / safe;
    vec3 far = max(t1, t2);
    float t = min(min(far.x, far.y), far.z);
    if (t <= 0.0) return R;
    vec3 towards = world + R * t - probePos;
    float reach = length(towards);
    return (reach > 1e-3) ? towards / reach : R;
}

vec4 probeReflection(vec3 world, vec3 normal, vec3 reflectDir, float roughness) {
    if (u_probeCount < 0.5) return vec4(0.0);

    vec3 lattice = (world - u_probeGridOrigin) / u_probeSpacing;
    vec3 corner0 = floor(lattice);
    vec3 frac = clamp(lattice - corner0, 0.0, 1.0);

    float wanted = pow(clamp(roughness, 0.0, 1.0), 1.0 / u_probeCurve) * (u_probeLevels - 1.0);
    int low = int(floor(wanted));
    int high = min(low + 1, int(u_probeLevels) - 1);
    float between = wanted - float(low);

    vec3 sum = vec3(0.0);
    float weight = 0.0;
    for (int c = 0; c < 8; ++c) {
        vec3 step = vec3(float(c & 1), float((c >> 1) & 1), float((c >> 2) & 1));
        ivec3 cell = ivec3(corner0 + step);
        if (any(lessThan(cell, ivec3(0))) || any(greaterThanEqual(cell, u_probeGridDims))) continue;
        vec3 axis = mix(1.0 - frac, frac, step);
        float w = axis.x * axis.y * axis.z;
        if (w < 0.002) continue;

        uvec2 packed = texelFetch(u_probeIndex, cell, 0).rg;
        int id = int(packed.x) + int(packed.y) * 256;
        if (id >= int(u_probeCount)) continue;      // 0xFFFF: this lattice point is not a probe

        vec3 at = u_probeGridOrigin + vec3(cell) * u_probeSpacing;
        // A probe standing behind the surface is looking at the next room along, and trilinear
        // weighting alone will happily bring it through the wall.
        w *= clamp(dot(at - world, normal) / max(u_probeSpacing * 0.5, 1e-3) + 0.6, 0.0, 1.0);
        if (w < 0.002) continue;

        vec3 dir = probeParallax(world, reflectDir, at);
        sum += mix(probeTexel(id, low, dir), probeTexel(id, high, dir), between) * w;
        weight += w;
    }
    if (weight < 0.002) return vec4(0.0);
    // The weight doubles as coverage: a surface at the edge of the probe volume fades into the
    // analytic sky rather than ending at a line.
    return vec4(sum / weight, clamp(weight, 0.0, 1.0));
}

// Schlick, with the ceiling a rough surface needs. Plain Schlick sends every material to white at
// ninety degrees, so a rough floor becomes a mirror at the horizon; capping the top at 1 - roughness
// keeps the grazing lift and takes the white edge off.
vec3 probeFresnel(vec3 f0, float ndv, float roughness) {
    float f = pow(1.0 - clamp(ndv, 0.0, 1.0), 5.0);
    vec3 ceiling = max(vec3(1.0 - roughness), f0);
    return f0 + (ceiling - f0) * f;
}
`;

// Everything the card holds for one clip's probes. There is always a texture bound, even for a clip
// with no probes at all: a sampler pointing at nothing is undefined behaviour rather than a blank.
export class Probes {
    constructor(gl) {
        this.gl = gl;
        this.atlas = gl.createTexture();
        this.index = gl.createTexture();
        this.data = null;
        this.box = { low: [0, 0, 0], high: [0, 0, 0] };
        this.empty();
    }

    empty() {
        const gl = this.gl;
        this.data = null;
        gl.bindTexture(gl.TEXTURE_2D, this.atlas);
        gl.pixelStorei(gl.UNPACK_ALIGNMENT, 4);
        gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA8, 1, 1, 0, gl.RGBA, gl.UNSIGNED_BYTE,
                      new Uint8Array([0, 0, 0, 255]));
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);

        gl.bindTexture(gl.TEXTURE_3D, this.index);
        gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);
        gl.texImage3D(gl.TEXTURE_3D, 0, gl.RG8UI, 1, 1, 1, 0, gl.RG_INTEGER, gl.UNSIGNED_BYTE,
                      new Uint8Array([255, 255]));
        gl.texParameteri(gl.TEXTURE_3D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
        gl.texParameteri(gl.TEXTURE_3D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
        gl.texParameteri(gl.TEXTURE_3D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
        gl.texParameteri(gl.TEXTURE_3D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
        gl.texParameteri(gl.TEXTURE_3D, gl.TEXTURE_WRAP_R, gl.CLAMP_TO_EDGE);
    }

    // `clip` is what format.js parsed. The parallax box is the clip's MATTER box rather than its
    // sampled box: the sampled box is nearly always bigger, and correcting against air puts the
    // reflection of a wall a metre behind the wall.
    set(clip) {
        const gl = this.gl;
        const probes = clip && clip.probes ? clip.probes : null;
        this.box = {
            low: clip ? clip.low.slice() : [0, 0, 0],
            high: clip ? clip.high.slice() : [0, 0, 0],
        };
        if (!probes || probes.count === 0) {
            this.empty();
            return;
        }
        this.data = probes;

        gl.bindTexture(gl.TEXTURE_2D, this.atlas);
        gl.pixelStorei(gl.UNPACK_ALIGNMENT, 4);
        gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA8, probes.atlasWidth, probes.atlasHeight, 0,
                      gl.RGBA, gl.UNSIGNED_BYTE, probes.atlas);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);

        gl.bindTexture(gl.TEXTURE_3D, this.index);
        // One byte a row is not four, and a lattice with an odd width would be read shifted.
        gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);
        gl.texImage3D(gl.TEXTURE_3D, 0, gl.RG8UI, probes.dims[0], probes.dims[1], probes.dims[2], 0,
                      gl.RG_INTEGER, gl.UNSIGNED_BYTE, probes.index);
        gl.texParameteri(gl.TEXTURE_3D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
        gl.texParameteri(gl.TEXTURE_3D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
        gl.texParameteri(gl.TEXTURE_3D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
        gl.texParameteri(gl.TEXTURE_3D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
        gl.texParameteri(gl.TEXTURE_3D, gl.TEXTURE_WRAP_R, gl.CLAMP_TO_EDGE);
        gl.pixelStorei(gl.UNPACK_ALIGNMENT, 4);
    }

    // Call once per program per frame, after `useProgram`.
    bind(uniforms) {
        const gl = this.gl;
        const p = this.data;
        gl.activeTexture(gl.TEXTURE0 + PROBE_ATLAS_UNIT);
        gl.bindTexture(gl.TEXTURE_2D, this.atlas);
        gl.uniform1i(uniforms.u_probeAtlas, PROBE_ATLAS_UNIT);
        gl.activeTexture(gl.TEXTURE0 + PROBE_INDEX_UNIT);
        gl.bindTexture(gl.TEXTURE_3D, this.index);
        gl.uniform1i(uniforms.u_probeIndex, PROBE_INDEX_UNIT);
        gl.activeTexture(gl.TEXTURE0);

        gl.uniform1f(uniforms.u_probeCount, p ? p.count : 0);
        if (!p) return;
        gl.uniform1f(uniforms.u_probeSpacing, p.spacing);
        gl.uniform3fv(uniforms.u_probeGridOrigin, p.gridOrigin);
        gl.uniform3iv(uniforms.u_probeGridDims, p.dims);
        gl.uniform2fv(uniforms.u_probeAtlasSize, [p.atlasWidth, p.atlasHeight]);
        gl.uniform2fv(uniforms.u_probeTile, [p.tileWidth, p.tileHeight]);
        gl.uniform1f(uniforms.u_probePerRow, p.perRow);
        gl.uniform1f(uniforms.u_probeBase, p.base);
        gl.uniform1f(uniforms.u_probeLevels, p.levels);
        gl.uniform1f(uniforms.u_probeBorder, p.border);
        gl.uniform1f(uniforms.u_probeRange, p.range);
        gl.uniform1f(uniforms.u_probeCurve, p.curve);
        gl.uniform3fv(uniforms.u_probeBoxLow, this.box.low);
        gl.uniform3fv(uniforms.u_probeBoxHigh, this.box.high);
    }
}
