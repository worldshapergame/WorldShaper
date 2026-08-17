// The colour irradiance volume, in the browser — global illumination and colour bleeding.
//
// `tools/bake/irradiance.hpp` is what makes this, and its header is the argument for the shape of
// it. The short version: the light grid the viewer has always had is two bytes a point, how much
// of the sun reaches it and how much of the sky. That is a visibility term and it cannot carry
// colour, so nothing in this viewer bounced light off a red floor onto a white vault. This is the
// block that does, and it holds ONLY light that has bounced — the direct sun and the sky are still
// the two visibility bytes, and nothing here is added to them twice.
//
// # Six values, one fetch, and no reconstruction at all
//
// It is a Half-Life 2 ambient cube: six RGB values a point, one per world axis. Every surface this
// rasteriser draws is a merged voxel face, and a voxel face's normal is exactly one of +X -X +Y -Y
// +Z -Z — it is `u_normal`, a UNIFORM, set once per draw call. So the right face of the cube is
// known before a single fragment runs, and the whole basis reduces to binding one 3D texture per
// face pass and taking one trilinear fetch. No dot products, no nine-term reconstruction, no
// ringing, and no negative lobes to clamp. Second-order spherical harmonics would be nine RGB
// instead of six, would cost that reconstruction per fragment, and would ring on exactly the
// surfaces this exists for.
//
// # The lattice is half the light grid's
//
// Eighteen bytes a point against the light grid's two is only affordable at an eighth of the
// points, and 0.8 m is plenty for a term that is low-frequency by construction. On the facility's
// rotunda that is 18 x 17 x 18 = 5,508 points and 97 KB, against 666 KB at the light grid's own
// pitch. See the baker's header for the numbers.
//
// # The encoding
//
// `L = (v / 255)^2 * range`, range 4.0. A square rather than a gamma curve because it is one
// multiply on a phone, and a curve rather than a linear ramp because a linear 8-bit encoding of
// 0..4 steps by 0.0157, which is a third of the whole indirect term in a dark interior and bands
// visibly across a vault.

// The 32-byte sub-header the GIRR chunk starts with. It carries its own dimensions rather than
// spending header bytes, so the fixed layout does not have to know this block exists.
const CHUNK_HEADER_BYTES = 32;
const FACES = 6;

// Bound at this texture unit. 0 is the materials and 1 is the light grid; see setShared in gl.js.
export const GI_TEXTURE_UNIT = 2;   // UNIT.gi in web/js/gl.js, which is the register

// The uniforms the fragment shader needs. Declared here rather than in gl.js so that the block and
// its declarations cannot drift apart.
export const GI_FRAGMENT_UNIFORMS = `
uniform sampler3D u_gi;        // one face of the ambient cube: the one this draw call's normal is
uniform vec3 u_giOrigin;
uniform vec3 u_giScale;        // 1 / (the volume's size in metres)
uniform vec3 u_giTexel;        // half a texel, so a fetch lands on a lattice point
uniform float u_giBias;        // how far along the normal to sample, in metres
uniform float u_giRange;       // the decode range; L = v * v * range
`;

// ...and the one function that reads it. The texture bound is already the right face of the cube,
// so there is nothing to select: the normal only decides where along it to sample.
export const GI_FRAGMENT_FUNCTION = `
vec3 gi_indirect(vec3 world, vec3 n) {
    vec3 at = (world + n * u_giBias - u_giOrigin) * u_giScale;
    at = clamp(at + u_giTexel, vec3(0.0), vec3(1.0));
    vec3 v = texture(u_gi, at).rgb;
    return v * v * u_giRange;
}
`;

// Read the GIRR chunk out of a parsed clip. Returns null when the clip has none, which is every
// clip baked before this existed and is not an error — the viewer draws them exactly as it did.
export function parseIrradiance(clip) {
    const bytes = clip.chunk ? clip.chunk('GIRR') : null;
    if (!bytes || bytes.byteLength < CHUNK_HEADER_BYTES) return null;
    const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    const version = view.getUint32(0, true);
    if (version !== 1) return null;
    const dims = [view.getUint32(4, true), view.getUint32(8, true), view.getUint32(12, true)];
    const cell = view.getFloat32(16, true);
    const range = view.getFloat32(20, true);
    const planes = view.getUint32(24, true);
    const points = dims[0] * dims[1] * dims[2];
    if (points <= 0 || planes !== FACES) return null;
    if (CHUNK_HEADER_BYTES + points * 3 * FACES > bytes.byteLength) return null;
    return {
        dims, cell, range,
        // Six planes of RGB8 in the file's own face order: +X -X +Y -Y +Z -Z.
        plane: (face) => bytes.subarray(CHUNK_HEADER_BYTES + face * points * 3,
                                        CHUNK_HEADER_BYTES + (face + 1) * points * 3),
        points,
    };
}

// Six 3D textures and the uniform setup, and a black one for the clips that have no volume.
//
// There is ALWAYS a texture bound, even a 1x1x1 black one, because a sampler bound to nothing is
// undefined behaviour rather than a blank hole — the same reason the cutter pool always has one.
export class IrradianceVolume {
    constructor(gl) {
        this.gl = gl;
        this.textures = [];
        for (let face = 0; face < FACES; ++face) this.textures.push(gl.createTexture());
        this.empty = gl.createTexture();
        this.present = false;
        this.dims = [1, 1, 1];
        this.cell = 1;
        this.range = 1;
        this.upload3D(this.empty, 1, 1, 1, new Uint8Array([0, 0, 0, 255]));
    }

    upload3D(texture, w, h, d, data) {
        const gl = this.gl;
        gl.bindTexture(gl.TEXTURE_3D, texture);
        gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);
        gl.texImage3D(gl.TEXTURE_3D, 0, gl.RGBA8, w, h, d, 0, gl.RGBA, gl.UNSIGNED_BYTE, data);
        gl.texParameteri(gl.TEXTURE_3D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
        gl.texParameteri(gl.TEXTURE_3D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
        gl.texParameteri(gl.TEXTURE_3D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
        gl.texParameteri(gl.TEXTURE_3D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
        gl.texParameteri(gl.TEXTURE_3D, gl.TEXTURE_WRAP_R, gl.CLAMP_TO_EDGE);
    }

    setClip(clip) {
        const gl = this.gl;
        const volume = parseIrradiance(clip);
        this.present = !!volume;
        if (!volume) return;
        this.dims = volume.dims;
        this.cell = volume.cell;
        this.range = volume.range;

        // RGB8 would upload with no copy at all, and it is expanded to RGBA8 here anyway: a
        // three-byte 3D texture is a format several mobile drivers accept and then convert one row
        // at a time, and the copy is one pass over a few hundred kilobytes done once at load.
        const points = volume.points;
        const wide = new Uint8Array(points * 4);
        gl.activeTexture(gl.TEXTURE0 + GI_TEXTURE_UNIT);
        for (let face = 0; face < FACES; ++face) {
            const plane = volume.plane(face);
            for (let i = 0; i < points; ++i) {
                wide[i * 4 + 0] = plane[i * 3 + 0];
                wide[i * 4 + 1] = plane[i * 3 + 1];
                wide[i * 4 + 2] = plane[i * 3 + 2];
                wide[i * 4 + 3] = 255;
            }
            this.upload3D(this.textures[face], volume.dims[0], volume.dims[1], volume.dims[2],
                          wide);
        }
        // Left where every other texture upload in this viewer leaves it.
        gl.activeTexture(gl.TEXTURE0);
    }

    // Everything that does not change between the six face passes.
    setUniforms(uniforms) {
        const gl = this.gl;
        if (uniforms.u_gi === undefined) return;
        const size = [this.dims[0] * this.cell, this.dims[1] * this.cell,
                      this.dims[2] * this.cell];
        gl.uniform1i(uniforms.u_gi, GI_TEXTURE_UNIT);
        gl.uniform3fv(uniforms.u_giScale, [1 / size[0], 1 / size[1], 1 / size[2]]);
        gl.uniform3fv(uniforms.u_giTexel, [0.5 / this.dims[0], 0.5 / this.dims[1],
                                           0.5 / this.dims[2]]);
        // Half a cell out along the normal, which is the same 0.4 m the light grid is read at: far
        // enough to leave the wall the face is the surface of, near enough not to read the room
        // next door.
        gl.uniform1f(uniforms.u_giBias, this.present ? this.cell * 0.5 : 0);
        gl.uniform1f(uniforms.u_giRange, this.present ? this.range : 0);
    }

    // The volume's own origin is the clip's, exactly as the light grid's is.
    setOrigin(uniforms, origin) {
        if (uniforms.u_giOrigin === undefined) return;
        this.gl.uniform3fv(uniforms.u_giOrigin, origin);
    }

    // One face of the cube per draw call, because the normal is a uniform and the face it wants is
    // therefore known before any fragment runs. This is the whole of the per-frame cost: six
    // binds, and one trilinear fetch a fragment.
    bindFace(face) {
        const gl = this.gl;
        gl.activeTexture(gl.TEXTURE0 + GI_TEXTURE_UNIT);
        gl.bindTexture(gl.TEXTURE_3D, this.present ? this.textures[face] : this.empty);
    }
}
