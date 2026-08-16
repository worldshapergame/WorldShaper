// Reading a .wsc file — what tools/bake_web.cpp writes.
//
// The format is deliberately something a phone can use without touching it: the quads are already
// in the layout the vertex buffer wants, so loading a clip is a fetch, a header read, and four
// subarray views. Nothing is decoded, unpacked or rebuilt per element, which is why a four
// hundred thousand quad building appears about as fast as it can be downloaded.
//
// Every offset here has a matching one in bake_web.cpp. If you change one, change both — the file
// carries a version and a magic so a mismatch says so instead of drawing nonsense.

export const HEADER_BYTES = 192;
export const QUAD_BYTES = 16;
export const MATERIAL_BYTES = 16;

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
    if (version !== 1) throw new Error('clip file version ' + version + ', this viewer reads 1');

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
