// Reading a .wsc file — what tools/bake_web.cpp writes.
//
// The format is deliberately something a phone can use without touching it: the quads are already
// in the layout the vertex buffer wants, so loading a clip is a fetch, a header read, and four
// subarray views. Nothing is decoded, unpacked or rebuilt per element, which is why a four
// hundred thousand quad building appears about as fast as it can be downloaded.
//
// Every offset here has a matching one in bake_web.cpp. If you change one, change both — the file
// carries a version and a magic so a mismatch says so instead of drawing nonsense.

// >>> probes
import { readProbes, PROBE_FOURCC } from './features/probes.js';

// Version 3 spends the last eight spare bytes of the header on a CHUNK DIRECTORY, so that anything
// added to the format from here on is a fourcc and a range instead of another fixed offset every
// block below it has to move for. That is what made version 1 unreadable as version 2, and it is
// not worth doing twice.
//
//   u32 at 200  chunkOffset      u32 at 204  chunkCount
//   each entry, 16 bytes:  char fourcc[4]   u32 offset   u32 size   u32 reserved
export const FORMAT_VERSION = 3;
// <<< probes
export const HEADER_BYTES = 208;
export const QUAD_BYTES = 16;
export const MATERIAL_BYTES = 16;
// 0 op, 4 cut_start, 8 scale, 12..59 a 3x4 world-to-local matrix, 60..91 eight parameters,
// 92..115 the world box it ends up in, 116 cut_count.
export const SHAPE_BYTES = 120;
// Six RGBA32F texels a cutter: three matrix rows, eight parameters, then (op, scale, pad, pad).
export const CUTTER_BYTES = 96;
export const CUTTER_TEXELS = 6;

// 0 +X   1 -X   2 +Y   3 -Y   4 +Z   5 -Z
export const FACE_NORMAL = [
    [1, 0, 0], [-1, 0, 0],
    [0, 1, 0], [0, -1, 0],
    [0, 0, 1], [0, 0, -1],
];

export function parseClip(buffer) {
    const view = new DataView(buffer);
    const magic = String.fromCharCode(view.getUint8(0), view.getUint8(1), view.getUint8(2),
                                      view.getUint8(3));
    if (magic !== 'WSCV') throw new Error('not a clip file (magic "' + magic + '")');
    const version = view.getUint32(4, true);
    // Version 1 packed the header into exactly 192 bytes with nothing spare, so version 2 — which
    // adds the cutter pool that makes the shapes view show holes rather than red ghosts — moved
    // every block offset. There is no reading one as the other, and a stale file in a cache beside
    // a fresh viewer is a real state: it says so here rather than drawing nonsense.
    if (version !== FORMAT_VERSION) {
        throw new Error('clip file version ' + version + ', this viewer reads ' + FORMAT_VERSION +
                        ' — it needs baking again');
    }

    const clip = {
        dims: [view.getInt32(8, true), view.getInt32(12, true), view.getInt32(16, true)],
        metre: view.getInt32(20, true),
        origin: [view.getFloat32(24, true), view.getFloat32(28, true), view.getFloat32(32, true)],
        sun: [view.getFloat32(36, true), view.getFloat32(40, true), view.getFloat32(44, true)],
        materialCount: view.getUint32(48, true),
        opaqueQuads: view.getUint32(52, true),
        transparentQuads: view.getUint32(56, true),
        opaqueFace: [],
        transparentFace: [],
        collisionDims: [view.getInt32(116, true), view.getInt32(120, true), view.getInt32(124, true)],
        collisionMetre: view.getInt32(128, true),
        lightDims: [view.getInt32(132, true), view.getInt32(136, true), view.getInt32(140, true)],
        lightCell: view.getFloat32(144, true),
        low: [view.getFloat32(148, true), view.getFloat32(152, true), view.getFloat32(156, true)],
        high: [view.getFloat32(160, true), view.getFloat32(164, true), view.getFloat32(168, true)],
        authoredMetre: view.getInt32(172, true),
        solid: view.getUint32(176, true),
        // 180..187 is the key the baker stamps to know whether this file still holds; the viewer
        // does not care what it is.
        shapeCount: view.getUint32(188, true),
        cutterCount: view.getUint32(192, true),
        cutterOffset: view.getUint32(196, true),
        // >>> probes
        chunkOffset: view.getUint32(200, true),
        chunkCount: view.getUint32(204, true),
        // <<< probes
    };
    // Seven entries: six starts and the end, so a range is start[i]..start[i + 1] everywhere.
    for (let i = 0; i < 7; ++i) {
        clip.opaqueFace.push(view.getUint32(60 + i * 4, true));
        clip.transparentFace.push(view.getUint32(88 + i * 4, true));
    }

    let at = HEADER_BYTES;
    clip.materials = new Uint8Array(buffer, at, clip.materialCount * MATERIAL_BYTES);
    at += clip.materialCount * MATERIAL_BYTES;

    clip.opaque = new Uint8Array(buffer, at, clip.opaqueQuads * QUAD_BYTES);
    at += clip.opaqueQuads * QUAD_BYTES;
    clip.transparent = new Uint8Array(buffer, at, clip.transparentQuads * QUAD_BYTES);
    at += clip.transparentQuads * QUAD_BYTES;

    const lightTexels = clip.lightDims[0] * clip.lightDims[1] * clip.lightDims[2] * 2;
    clip.light = new Uint8Array(buffer, at, lightTexels);
    at += lightTexels;

    const collisionCells = clip.collisionDims[0] * clip.collisionDims[1] * clip.collisionDims[2];
    clip.collision = new Uint8Array(buffer, at, (collisionCells + 7) >> 3);
    at += (collisionCells + 7) >> 3;

    // The clip as it was written: every shape the author typed, with the transform that places it
    // and the slice of the cutter pool that is subtracted from it. Absent from files baked before
    // this existed, which is why the length is checked rather than trusted.
    clip.shapes = (clip.shapeCount > 0 && at + clip.shapeCount * SHAPE_BYTES <= buffer.byteLength)
        ? new Uint8Array(buffer, at, clip.shapeCount * SHAPE_BYTES)
        : new Uint8Array(0);
    if (clip.shapes.length === 0) clip.shapeCount = 0;
    at += clip.shapeCount * SHAPE_BYTES;

    // The cutter pool: every `difference` subtrahend, in world space, pointed at by the shapes it
    // actually cuts. The baker writes where it starts as well as how big it is, so a reader that
    // computes the offset and a reader that reads it either agree or one of them says so.
    const cutterBytes = clip.cutterCount * CUTTER_BYTES;
    if (clip.cutterCount > 0 && clip.cutterOffset !== at) {
        throw new Error('cutter pool says it is at ' + clip.cutterOffset + ', the blocks end at ' +
                        at);
    }
    clip.cutters = (clip.cutterCount > 0 && at + cutterBytes <= buffer.byteLength)
        ? new Uint8Array(buffer, at, cutterBytes)
        : new Uint8Array(0);
    if (clip.cutters.length === 0) clip.cutterCount = 0;

    // >>> probes
    // The chunk directory: every block added after version 3. Read into a table by fourcc, so a
    // reader that does not know a chunk skips it rather than mis-reading what follows, and two of
    // them added independently cannot collide.
    clip.chunks = {};
    if (clip.chunkCount > 0 && clip.chunkOffset + clip.chunkCount * 16 <= buffer.byteLength) {
        for (let i = 0; i < clip.chunkCount; ++i) {
            const at = clip.chunkOffset + i * 16;
            const fourcc = String.fromCharCode(view.getUint8(at), view.getUint8(at + 1),
                                               view.getUint8(at + 2), view.getUint8(at + 3));
            const offset = view.getUint32(at + 4, true);
            const size = view.getUint32(at + 8, true);
            if (offset + size > buffer.byteLength) {
                throw new Error('chunk "' + fourcc + '" runs to ' + (offset + size) +
                                ' in a file of ' + buffer.byteLength);
            }
            clip.chunks[fourcc] = { offset, size };
        }
    }
    clip.probes = readProbes(buffer, clip.chunks[PROBE_FOURCC]);
    // <<< probes

    clip.size = [
        clip.dims[0] / clip.metre, clip.dims[1] / clip.metre, clip.dims[2] / clip.metre,
    ];
    clip.centre = [
        (clip.low[0] + clip.high[0]) * 0.5,
        (clip.low[1] + clip.high[1]) * 0.5,
        (clip.low[2] + clip.high[2]) * 0.5,
    ];
    clip.reach = Math.max(
        clip.high[0] - clip.low[0], clip.high[1] - clip.low[1], clip.high[2] - clip.low[2]) || 1;
    clip.quads = clip.opaqueQuads + clip.transparentQuads;
    return clip;
}

// Is this cell matter? Anything outside the clip is air, which is what makes the edge of the world
// something you walk off rather than something you stand inside.
export function solidAt(clip, wx, wy, wz) {
    const cell = clip.collisionMetre;
    const x = Math.floor((wx - clip.origin[0]) * cell);
    const y = Math.floor((wy - clip.origin[1]) * cell);
    const z = Math.floor((wz - clip.origin[2]) * cell);
    const d = clip.collisionDims;
    if (x < 0 || y < 0 || z < 0 || x >= d[0] || y >= d[1] || z >= d[2]) return false;
    const i = x + y * d[0] + z * d[0] * d[1];
    return (clip.collision[i >> 3] & (1 << (i & 7))) !== 0;
}

// Any matter inside the box? Used by the walker, which resolves one axis at a time and asks this
// after each move.
export function boxHitsMatter(clip, lowX, lowY, lowZ, highX, highY, highZ) {
    const cell = clip.collisionMetre;
    const d = clip.collisionDims;
    const x0 = Math.floor((lowX - clip.origin[0]) * cell);
    const y0 = Math.floor((lowY - clip.origin[1]) * cell);
    const z0 = Math.floor((lowZ - clip.origin[2]) * cell);
    // The high corner is exclusive: a box whose face sits exactly on a cell boundary is touching
    // that cell, not inside it, and treating it as inside makes a player 12 cm wider than they are
    // and unable to fit through their own doorway.
    const x1 = Math.ceil((highX - clip.origin[0]) * cell) - 1;
    const y1 = Math.ceil((highY - clip.origin[1]) * cell) - 1;
    const z1 = Math.ceil((highZ - clip.origin[2]) * cell) - 1;
    if (x1 < 0 || y1 < 0 || z1 < 0 || x0 >= d[0] || y0 >= d[1] || z0 >= d[2]) return false;
    const ax0 = Math.max(0, x0), ay0 = Math.max(0, y0), az0 = Math.max(0, z0);
    const ax1 = Math.min(d[0] - 1, x1), ay1 = Math.min(d[1] - 1, y1), az1 = Math.min(d[2] - 1, z1);
    for (let z = az0; z <= az1; ++z) {
        const zBase = z * d[0] * d[1];
        for (let y = ay0; y <= ay1; ++y) {
            const yBase = zBase + y * d[0];
            for (let x = ax0; x <= ax1; ++x) {
                const i = yBase + x;
                if ((clip.collision[i >> 3] & (1 << (i & 7))) !== 0) return true;
            }
        }
    }
    return false;
}
