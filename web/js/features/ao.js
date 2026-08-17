// Ambient occlusion, read back off the surface.
//
// `tools/bake/occlusion.hpp` is what this is and why it is an atlas rather than a volume. In one
// paragraph: the viewer already had corner occlusion, which is one voxel wide, and the light grid's
// sky visibility, which is a 0.4 m lattice, and between them is the whole middle scale the facility
// is actually made of — a 0.225 m deep coffer, a 0.12 m flute, a 0.675 m niche, the reveal of a
// window, the joint where a wall meets a floor. The baker casts a hemisphere of thirty-two rays
// about each exposed voxel face's own normal, out to 0.45 m, and writes one byte per face.
//
// # Where the texels are, and why the file does not say
//
// The runs are allocated in the order the quads are written to the file — every opaque face group
// in face order, then every transparent one — so where quad *i*'s run starts is the prefix sum of
// `w * h` over the quads before it. That is computed here rather than read, which is four bytes a
// quad the file never carries: 1.6 MB on the whole facility, for a number that is a sum of two
// fields sitting next to it in the record.
//
// Two things deriving one layout from one description is exactly the failure D204 is named for, so
// the sum is CHECKED: the chunk says how many texels it wrote and this throws if the walk does not
// land on the same number. A disagreement is then a message and not a building with its shading
// half a quad out of step.
//
// # It survives greedy meshing, which vertex data does not
//
// Two faces merge only when everything about them agrees, so a smooth gradient at the vertices
// makes every voxel face its own quad and the mesh stops being a mesh — that is why the light is a
// lattice and not a vertex colour, and it is exactly as true of this. A texture is read per
// fragment and merging cannot see it, so a wall stays one quad and still has a shadow in its
// corner.

export const AO_CHUNK = 'AOCC';

// Bytes at the head of the chunk, before the texels.
const AO_HEADER_BYTES = 32;

// Read the atlas out of a parsed clip. Returns null when the clip was baked before this existed,
// which is a state the viewer has to survive rather than throw on: an old file in a cache beside a
// fresh viewer draws with the corner occlusion it always had.
export function readOcclusion(clip) {
    const chunk = clip.chunk ? clip.chunk(AO_CHUNK) : null;
    if (!chunk || chunk.byteLength < AO_HEADER_BYTES) return null;
    const view = new DataView(chunk.buffer, chunk.byteOffset, chunk.byteLength);
    const version = view.getUint32(0, true);
    if (version !== 1) return null;
    const texelCount = view.getUint32(4, true);
    const width = view.getUint32(8, true);
    const quadCount = view.getUint32(12, true);
    const radius = view.getFloat32(16, true);
    const rays = view.getUint32(20, true);
    if (texelCount === 0 || width === 0) return null;
    if (quadCount !== clip.quads) {
        throw new Error('the occlusion atlas was baked for ' + quadCount + ' quads and this clip ' +
                        'has ' + clip.quads + ' — it needs baking again');
    }
    if (AO_HEADER_BYTES + texelCount > chunk.byteLength) {
        throw new Error('the occlusion atlas says ' + texelCount + ' texels and carries ' +
                        (chunk.byteLength - AO_HEADER_BYTES));
    }

    const rows = Math.ceil(texelCount / width);
    // The texture is width x rows and the last row is short, so it is copied into a full one
    // rather than uploaded from a view that runs off the end. 255 is open, which is what an
    // unwritten texel should read as if anything ever reaches one.
    const texels = new Uint8Array(width * rows);
    texels.fill(255);
    texels.set(chunk.subarray(AO_HEADER_BYTES, AO_HEADER_BYTES + texelCount));

    return {
        texels,
        width,
        height: rows,
        texelCount,
        radius,
        rays,
        // Where every quad's run starts, in texels. Uint32Array because the facility's whole
        // surface is several million texels and a Uint16 would wrap at the third wall.
        base: quadBases(clip, texelCount),
    };
}

// The prefix sum, over the quads in the order the file writes them. `w` and `h` are at bytes 6 and
// 8 of the sixteen-byte record and are extents in voxels, so a quad owns exactly `w * h` texels.
function quadBases(clip, texelCount) {
    const base = new Uint32Array(clip.quads);
    let running = 0;
    let at = 0;
    for (const list of [clip.opaque, clip.transparent]) {
        if (!list || list.byteLength === 0) continue;
        const view = new DataView(list.buffer, list.byteOffset, list.byteLength);
        const count = list.byteLength / 16;
        for (let i = 0; i < count; ++i) {
            base[at++] = running;
            running += view.getUint16(i * 16 + 6, true) * view.getUint16(i * 16 + 8, true);
        }
    }
    if (running !== texelCount) {
        throw new Error('the occlusion atlas has ' + texelCount + ' texels and the quads want ' +
                        running + ' — the baker and the viewer disagree about the layout');
    }
    return base;
}

// --- what the shader needs -------------------------------------------------------------------
//
// The vertex shader carries the quad's first texel and its extent; the fragment shader turns the
// two into an index, unwraps that index into the atlas, and filters four of them by hand.
//
// BY HAND, and not with a sampler, for a reason worth writing down: a quad's run wraps at the edge
// of the atlas and sits against its neighbour's with no border, so a bilinear fetch would blend
// the last texel of a windowsill into the first texel of whatever was meshed after it. Clamping
// inside the tile costs four texelFetches and no padding at all — a one-texel border round every
// quad would be half as much memory again on a mesh with four hundred thousand of them.

export const AO_VERTEX_INPUTS = `
layout(location = 4) in highp uint a_aoBase;

flat out highp uint v_aoBase;
flat out vec2 v_aoSize;
out vec2 v_aoTexel;
`;

// `uv` is the corner, 0..1 along the quad's own two in-plane axes, and `size` is its extent in
// voxels — the same pair the geometry is built from, so a texel lands on its own voxel face.
export const AO_VERTEX_BODY = `
    v_aoBase = a_aoBase;
    v_aoSize = a_size;
    v_aoTexel = uv * a_size;
`;

export const AO_FRAGMENT = `
// A fragment shader's default integer precision is mediump, which is sixteen bits, and the whole
// facility's atlas is several million texels — an index into it computed at mediump wraps and
// shades a wall with a windowsill's occlusion. There is no error and no warning; the picture is
// simply wrong. So the fragment stage is told to keep integers whole.
precision highp int;

flat in highp uint v_aoBase;
flat in vec2 v_aoSize;
in vec2 v_aoTexel;

uniform highp usampler2D u_ao;
uniform int u_aoWidth;
uniform float u_aoFloor;      // what a fully occluded face keeps of its ambient light

float ao_texel(ivec2 t) {
    int index = int(v_aoBase) + t.y * int(v_aoSize.x) + t.x;
    return float(texelFetch(u_ao, ivec2(index % u_aoWidth, index / u_aoWidth), 0).r) / 255.0;
}

// Bilinear inside the quad's own run, clamped at its edges. Texel centres are at 0.5, 1.5, ...
// so the coordinate is shifted by half a texel and then held inside [0, w - 1]: the outer half
// texel of a quad is flat, which is what makes two quads that share an edge meet without a seam.
float ao_here() {
    if (u_aoWidth <= 1) return 1.0;
    vec2 s = clamp(v_aoTexel - 0.5, vec2(0.0), max(v_aoSize - 1.0, vec2(0.0)));
    ivec2 i0 = ivec2(floor(s));
    ivec2 i1 = min(i0 + 1, ivec2(v_aoSize) - 1);
    vec2 f = s - vec2(i0);
    float a = mix(ao_texel(ivec2(i0.x, i0.y)), ao_texel(ivec2(i1.x, i0.y)), f.x);
    float b = mix(ao_texel(ivec2(i0.x, i1.y)), ao_texel(ivec2(i1.x, i1.y)), f.x);
    return mix(a, b, f.y);
}
`;

// One texture, one integer attribute, and nothing else on the card.
//
// R8UI rather than R8: an unsigned integer texture is never filtered by the sampler, which is what
// is wanted here — the filtering is done by hand above, and a driver that quietly interpolated
// across a run boundary would be a hard fault to see and an easy one to draw.
export function upload(gl, texture, occlusion) {
    gl.bindTexture(gl.TEXTURE_2D, texture);
    gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);
    if (!occlusion) {
        // A sampler bound to nothing is undefined behaviour rather than a blank hole, so there is
        // always a texture. One texel, and `u_aoWidth` of 1 is what tells the shader to skip it.
        gl.texImage2D(gl.TEXTURE_2D, 0, gl.R8UI, 1, 1, 0, gl.RED_INTEGER, gl.UNSIGNED_BYTE,
                      new Uint8Array([255]));
    } else {
        gl.texImage2D(gl.TEXTURE_2D, 0, gl.R8UI, occlusion.width, occlusion.height, 0,
                      gl.RED_INTEGER, gl.UNSIGNED_BYTE, occlusion.texels);
    }
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
}

// The per-quad first texel, as one buffer the instanced attribute reads.
export function uploadBases(gl, buffer, occlusion) {
    if (!occlusion) return;
    gl.bindBuffer(gl.ARRAY_BUFFER, buffer);
    gl.bufferData(gl.ARRAY_BUFFER, occlusion.base, gl.STATIC_DRAW);
}

// How dark a fully occluded face gets, as a fraction of the ambient light it would otherwise take.
//
// It multiplies the AMBIENT and not the sun: a surface in shadow is the shadow map's business and
// darkening direct light by occlusion as well is how a rasteriser ends up with black under every
// eave at noon. 0.18 is chosen against the rotunda, which has no direct light in it at all —
// every photon in that room has bounced, so the ambient term is the whole picture and the range
// this floor sets is the range the room has. At 0.4 a coffer is a lighter square rather than a
// recess; below about 0.12 the pans go to a single flat black and the two ledges in them vanish,
// which is the fault this was written to fix, arrived at from the other side.
export const AO_FLOOR = 0.18;
