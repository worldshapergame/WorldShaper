// The material volume and the thickness field, on the card.
//
// What the baker put in `MVOL` and `THCK` (tools/bake/matvol.hpp) is what goes to the GPU —
// unchanged, not decompressed. That is the whole reason it is a block index and not a run length:
// the facility's occupancy grid is nine million cells, two dense byte planes over it are 18 MB,
// and a phone is not going to hold that any more than it is going to download it.
//
// # What it answers
//
// The `.wsc` has always carried the exposed surface and a one-bit occupancy grid, so nothing in it
// could say what the stone at a point INSIDE a wall is. Two things are wrong for exactly that
// reason: the slice cap paints the whole cut in the clip's commonest stone, and every Beer-Lambert
// absorption and every translucency in the material table is a per-metre figure with no metres to
// apply it over. This is what both of them ask.
//
// # Three textures and two fetches
//
//   u_matvolIndex     R32UI, one texel a 4x4x4 block. Bit 31 set means the block is one value all
//                     through: bits 0..7 its material, 8..15 its thickness, and no second fetch.
//                     Bit 31 clear means it is the ordinal of a page.
//   u_matvolPages     RG8UI, the blocks that are not uniform, laid out as a 3D atlas of 4x4x4
//                     pages. r is the material, g is the thickness in the clip's own voxels.
//   u_matvolPalette   256x1 RGBA8. The volume's byte is its OWN palette index, because a byte
//                     holds 255 materials and a clip may hold more; this turns it into the clip's
//                     material index, which is what `u_materials` is addressed by.
//
// A uniform block — air, or the inside of a wall — costs one fetch. Only the blocks a surface
// actually passes through cost two.

// The sampling functions, for any shader that wants them. Paste with `${MATVOL_GLSL}` and call
// `bindMatvol` on the program. Every uniform here is set by `Matvol.bind`, and when a clip has no
// volume the dims come out zero and every function answers "no matter" — one code path, no branch
// for the reader to get wrong.
//
//   int   ws_material_at(vec3 world)    the CLIP's material index (what material_row takes), or
//                                       -1 where there is no matter and where there is no volume.
//   float ws_thickness_at(vec3 world)   metres of matter through that point, along the thinnest
//                                       of the three axes: a pane's thickness, a wall's thickness.
//                                       0 where there is no matter.
//   uvec2 ws_matvol_cell(vec3 world)    both raw: x the volume's own byte (0 is air), y the
//                                       thickness in the clip's own voxels.
export const MATVOL_GLSL = `
uniform highp usampler3D u_matvolIndex;
uniform highp usampler3D u_matvolPages;
uniform highp sampler2D u_matvolPalette;
uniform vec3 u_matvolOrigin;    // world metres at cell (0, 0, 0)
uniform float u_matvolCells;    // cells per metre
uniform float u_matvolVoxel;    // metres a thickness unit is worth
uniform ivec3 u_matvolDims;     // the volume, in cells
uniform ivec3 u_matvolAtlas;    // the page atlas, in pages

uvec2 ws_matvol_cell(vec3 world) {
    ivec3 cell = ivec3(floor((world - u_matvolOrigin) * u_matvolCells));
    if (any(lessThan(cell, ivec3(0))) || any(greaterThanEqual(cell, u_matvolDims))) {
        return uvec2(0u, 0u);
    }
    uint entry = texelFetch(u_matvolIndex, cell >> 2, 0).r;
    // A block that is one value all through is the whole answer, and most of them are: open air,
    // and the inside of every wall.
    if ((entry & 0x80000000u) != 0u) return uvec2(entry & 255u, (entry >> 8) & 255u);
    int page = int(entry);
    ivec3 at = ivec3(page % u_matvolAtlas.x,
                     (page / u_matvolAtlas.x) % u_matvolAtlas.y,
                     page / (u_matvolAtlas.x * u_matvolAtlas.y)) * 4 + (cell & ivec3(3));
    return texelFetch(u_matvolPages, at, 0).rg;
}

int ws_material_at(vec3 world) {
    uint value = ws_matvol_cell(world).x;
    if (value == 0u) return -1;
    vec4 entry = texelFetch(u_matvolPalette, ivec2(int(value), 0), 0);
    return int(entry.r * 255.0 + 0.5) | (int(entry.g * 255.0 + 0.5) << 8);
}

float ws_thickness_at(vec3 world) {
    return float(ws_matvol_cell(world).y) * u_matvolVoxel;
}
`;

// What `MVOL` and `THCK` hold, read straight out of the file. Nothing here allocates a copy of the
// volume: the pages are interleaved into one RG array on the way to the card and that is the only
// pass over them.
export function parseMatvol(clip) {
    const material = clip.chunk ? clip.chunk('MVOL') : null;
    if (!material || material.byteLength < 48) return null;
    const view = new DataView(material.buffer, material.byteOffset, material.byteLength);
    const volume = {
        dims: [view.getUint32(0, true), view.getUint32(4, true), view.getUint32(8, true)],
        cellsPerMetre: view.getUint32(12, true),
        voxelsPerMetre: view.getUint32(16, true),
        blockDim: view.getUint32(20, true),
        blocks: [view.getUint32(24, true), view.getUint32(28, true), view.getUint32(32, true)],
        paletteCount: view.getUint32(36, true),
        blockCount: view.getUint32(40, true),
        pageCount: view.getUint32(44, true),
    };
    // The shader's block arithmetic is `cell >> 2` and `cell & 3`, which is the block dimension
    // written into the instruction stream rather than into a uniform. A file that says anything
    // else is a file this viewer cannot draw, and it says so rather than sampling nonsense.
    if (volume.blockDim !== 4) {
        throw new Error('material volume has ' + volume.blockDim + '-cell blocks, this viewer ' +
                        'reads 4');
    }

    let at = 48;
    volume.palette = new Uint16Array(volume.paletteCount);
    for (let i = 0; i < volume.paletteCount; ++i) {
        volume.palette[i] = view.getUint16(at + i * 2, true);
    }
    at += volume.paletteCount * 2;
    at = (at + 3) & ~3;
    // .slice(): the chunk starts wherever the block before it ended, and a Uint32Array wants its
    // own alignment rather than the file's.
    volume.index = new Uint32Array(
        material.slice(at, at + volume.blockCount * 4).buffer);
    at += volume.blockCount * 4;
    const pageBytes = volume.pageCount * 64;
    volume.materialPages = material.subarray(at, at + pageBytes);

    const thickness = clip.chunk('THCK');
    volume.thicknessPages = (thickness && thickness.byteLength >= 8 + pageBytes)
        ? thickness.subarray(8, 8 + pageBytes)
        : null;
    return volume;
}

// One clip's volume on the card. `upload` frees and reallocates, like everything else in gl.js
// that a clip owns; `bind` puts it on three texture units and sets every uniform the GLSL above
// names. A renderer with no volume still calls both — the textures are 1x1, the dims are zero, and
// every sample comes back air.
export class Matvol {
    constructor(gl) {
        this.gl = gl;
        this.index = gl.createTexture();
        this.pages = gl.createTexture();
        this.palette = gl.createTexture();
        this.present = false;
        this.dims = [0, 0, 0];
        this.atlas = [1, 1, 1];
        this.origin = [0, 0, 0];
        this.cellsPerMetre = 1;
        this.voxelMetres = 1;
        this.bytes = 0;
    }

    upload(clip) {
        const gl = this.gl;
        this.present = false;
        this.dims = [0, 0, 0];
        this.atlas = [1, 1, 1];
        this.bytes = 0;

        let volume = null;
        try {
            volume = parseMatvol(clip);
        } catch (error) {
            console.warn('material volume: ' + error.message);
            volume = null;
        }

        if (volume && volume.pageCount > 0) {
            // The page atlas is a 3D texture of 4x4x4 pages, and how many fit along an axis is
            // decided by what the DEVICE says rather than by a constant. A phone is only promised
            // 256 texels an axis in WebGL 2, which is 64 pages; a desktop gives 2048. Laying the
            // pages out to a fixed 64 and letting the depth run would exceed that promise on the
            // one class of device this whole viewer exists for.
            const per = Math.max(1, Math.min(64, Math.floor(
                gl.getParameter(gl.MAX_3D_TEXTURE_SIZE) / 4)));
            const x = Math.min(volume.pageCount, per);
            const y = Math.min(Math.ceil(volume.pageCount / x), per);
            const z = Math.ceil(volume.pageCount / (x * y));
            if (z > per) {
                console.warn('material volume: ' + volume.pageCount + ' pages is past what a ' +
                             (per * 4) + '-texel 3D texture holds; drawn without it');
                volume = null;
            } else {
                this.atlas = [x, y, z];
            }
        }

        if (!volume) {
            this.uploadEmpty();
            return;
        }

        this.dims = volume.dims;
        this.origin = clip.origin;
        this.cellsPerMetre = volume.cellsPerMetre;
        this.voxelMetres = 1 / Math.max(1, volume.voxelsPerMetre);

        gl.bindTexture(gl.TEXTURE_3D, this.index);
        gl.pixelStorei(gl.UNPACK_ALIGNMENT, 4);
        gl.texImage3D(gl.TEXTURE_3D, 0, gl.R32UI, volume.blocks[0], volume.blocks[1],
                      volume.blocks[2], 0, gl.RED_INTEGER, gl.UNSIGNED_INT, volume.index);
        this.clamp(gl.TEXTURE_3D);

        // The two channels interleaved into one RG texture, so a cell is one fetch rather than
        // two. This is the only pass over the pages and it is over the PAGES — the blocks that are
        // one value all through never appear here at all.
        const [ax, ay, az] = this.atlas;
        const texels = ax * 4 * ay * 4 * az * 4;
        const pixels = new Uint8Array(texels * 2);
        const width = ax * 4;
        const height = ay * 4;
        for (let page = 0; page < volume.pageCount; ++page) {
            const px = (page % ax) * 4;
            const py = (Math.floor(page / ax) % ay) * 4;
            const pz = Math.floor(page / (ax * ay)) * 4;
            for (let lz = 0; lz < 4; ++lz) {
                for (let ly = 0; ly < 4; ++ly) {
                    let source = page * 64 + ly * 4 + lz * 16;
                    let target = ((pz + lz) * height * width + (py + ly) * width + px) * 2;
                    for (let lx = 0; lx < 4; ++lx) {
                        pixels[target] = volume.materialPages[source];
                        pixels[target + 1] =
                            volume.thicknessPages ? volume.thicknessPages[source] : 0;
                        ++source;
                        target += 2;
                    }
                }
            }
        }
        gl.bindTexture(gl.TEXTURE_3D, this.pages);
        gl.pixelStorei(gl.UNPACK_ALIGNMENT, 2);
        gl.texImage3D(gl.TEXTURE_3D, 0, gl.RG8UI, width, height, az * 4, 0, gl.RG_INTEGER,
                      gl.UNSIGNED_BYTE, pixels);
        this.clamp(gl.TEXTURE_3D);

        // Volume byte -> the clip's material index, as a low byte and a high byte. It is a texture
        // and not a uniform array because 255 ints is more uniform vectors than GLSL ES promises.
        const palette = new Uint8Array(256 * 4);
        for (let i = 0; i < volume.paletteCount; ++i) {
            palette[(i + 1) * 4 + 0] = volume.palette[i] & 255;
            palette[(i + 1) * 4 + 1] = (volume.palette[i] >> 8) & 255;
        }
        gl.bindTexture(gl.TEXTURE_2D, this.palette);
        gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);
        gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA8, 256, 1, 0, gl.RGBA, gl.UNSIGNED_BYTE, palette);
        this.clamp(gl.TEXTURE_2D);

        this.present = true;
        this.bytes = volume.index.byteLength + pixels.byteLength + palette.byteLength;
    }

    // A clip with no volume, or one this device cannot hold. Every sampler is still bound, because
    // a sampler bound to nothing in WebGL is undefined behaviour rather than a blank answer, and
    // `u_matvolDims` is zero so the bounds test in the shader returns air before it fetches.
    uploadEmpty() {
        const gl = this.gl;
        gl.bindTexture(gl.TEXTURE_3D, this.index);
        gl.pixelStorei(gl.UNPACK_ALIGNMENT, 4);
        gl.texImage3D(gl.TEXTURE_3D, 0, gl.R32UI, 1, 1, 1, 0, gl.RED_INTEGER, gl.UNSIGNED_INT,
                      new Uint32Array(1));
        this.clamp(gl.TEXTURE_3D);
        gl.bindTexture(gl.TEXTURE_3D, this.pages);
        gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);
        gl.texImage3D(gl.TEXTURE_3D, 0, gl.RG8UI, 1, 1, 1, 0, gl.RG_INTEGER, gl.UNSIGNED_BYTE,
                      new Uint8Array(2));
        this.clamp(gl.TEXTURE_3D);
        gl.bindTexture(gl.TEXTURE_2D, this.palette);
        gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA8, 1, 1, 0, gl.RGBA, gl.UNSIGNED_BYTE,
                      new Uint8Array(4));
        this.clamp(gl.TEXTURE_2D);
    }

    clamp(target) {
        const gl = this.gl;
        // NEAREST everywhere and no exceptions: a material index is a name and the average of two
        // names is a third material. An integer texture cannot be filtered at all, and the palette
        // is fetched by texelFetch, so this is what they all want anyway.
        gl.texParameteri(target, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
        gl.texParameteri(target, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
        gl.texParameteri(target, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
        gl.texParameteri(target, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
        if (target === gl.TEXTURE_3D) {
            gl.texParameteri(target, gl.TEXTURE_WRAP_R, gl.CLAMP_TO_EDGE);
        }
    }

    // Three consecutive texture units from `unit`, and every uniform the GLSL above names.
    bind(uniforms, unit) {
        const gl = this.gl;
        gl.activeTexture(gl.TEXTURE0 + unit);
        gl.bindTexture(gl.TEXTURE_3D, this.index);
        gl.uniform1i(uniforms.u_matvolIndex, unit);
        gl.activeTexture(gl.TEXTURE0 + unit + 1);
        gl.bindTexture(gl.TEXTURE_3D, this.pages);
        gl.uniform1i(uniforms.u_matvolPages, unit + 1);
        gl.activeTexture(gl.TEXTURE0 + unit + 2);
        gl.bindTexture(gl.TEXTURE_2D, this.palette);
        gl.uniform1i(uniforms.u_matvolPalette, unit + 2);
        gl.uniform3fv(uniforms.u_matvolOrigin, this.origin);
        gl.uniform1f(uniforms.u_matvolCells, this.cellsPerMetre);
        gl.uniform1f(uniforms.u_matvolVoxel, this.voxelMetres);
        gl.uniform3iv(uniforms.u_matvolDims, this.dims);
        gl.uniform3iv(uniforms.u_matvolAtlas, this.atlas);
    }
}
