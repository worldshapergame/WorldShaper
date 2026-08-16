// The light list: emissive geometry, shaded as real lights.
//
// documentation/24-clip-viewer.md §4c is what this is for, and tools/bake/lights.hpp is what makes
// the file this reads. In one paragraph: these clips are full of emitters and the viewer treated
// every one of them as bright paint. `clips/many_lamps.clip` is a sealed hall with no sky and no
// sun in it, lit by thirty-six fittings, and it drew as thirty-six white rectangles on walls the
// colour of an ambient constant — everything the clip exists to test was invisible.
// `clips/facility/fittings.clip` says the same in its own header: two of the halls have no window
// at all, and everything past the first bay arrives from a sconce or from nothing.
//
// So the baker reduces each connected cluster of emissive voxels to a light — a position, an
// extent, a colour and an intensity derived from its EMITTING SURFACE AREA — and this shades with
// them. Two lobes, because they answer different questions:
//
//   DIFFUSE, falling off with the square of the distance. A room lit by lamps.
//   SPECULAR, a GGX lobe widened by the angle the fitting subtends. THIS is why the light list
//   exists rather than only an irradiance volume: a volume knows how much light arrives at a point
//   and not from WHERE, so it can never put a reflection of a sconce on a bronze arm. It is also
//   what tells the eye that a bowl is a small bright body and not a large soft one, which is
//   exactly the distinction fittings.clip says it is testing.
//
// # The cap, and what it costs
//
// A phone will not shade thirty-six lights a pixel, let alone the facility's. `MAX_LAMPS` are kept
// per draw, ranked by what each delivers at the camera — its intensity over the square of its
// distance, the same rank `src/world/light_list.cpp` uses for the game's own list. Sixteen is the
// number, and it is a CHOICE and not yet a measurement: it is the largest uniform array that fits
// comfortably inside the ES 3.0 minimum of 224 fragment uniform vectors alongside everything else
// this shader already holds. Nobody has run it against eight or against thirty-two on a phone, and
// it bites on both clips it has been tried on. §4c of the documentation carries what IS measured.
//
// The ranking is by the CAMERA and not by the surface, which is the honest limitation of doing it
// on the host: a lamp behind the eye is scored as if it lit what the eye is looking at. It is the
// same approximation the game makes and it is wrong in the same place — a corridor whose lamps are
// all behind you. What stops that being visible is the shadow term: a lamp that is kept and cannot
// be seen contributes nothing, so the cost of keeping the wrong one is a wasted lane and not a
// wrong picture.
//
// When the cap bites it says so once per clip on the console and in `renderer.lights`, because a
// silent truncation reads as "there were only sixteen".
//
// # Shadowing, and which lamps do not have any
//
// No rays at run time. Each of the strongest `shadowed` lights carries a baked cube of distances —
// six faces in one atlas — and the shader does one bilinear fetch per lamp per pixel, comparing the
// distance the light travelled with the distance stored for that direction. Unshadowed point lights
// leak through walls and it is instantly obvious: many_lamps is built out of exactly that case,
// four quarters walled off from each other with a lamp bolted to both sides of every partition.
//
// The lights past the shadow budget are lit UNSHADOWED. They are the weakest in the clip by
// construction, they are still ranked last for the per-draw cap, and what they look like when they
// do reach the screen is a faint wash that ignores a wall. The count is printed by the baker and
// carried here as `unshadowed`.

// How many lamps one draw may shade. See the note above and §4c of the documentation.
export const MAX_LAMPS = 16;

// The record the baker writes, in bytes. tools/bake/lights.hpp writes exactly this.
export const LIGHT_BYTES = 48;

// Everything the fragment shader needs, as two strings so gl.js can splice them into its own
// program rather than run a second pass. `LAMP_UNIFORMS` goes with the other uniforms and
// `LAMP_SHADING` with the other functions; `lamps()` is what main() calls.
export const LAMP_UNIFORMS = `
const int MAX_LAMPS = ${MAX_LAMPS};
uniform int u_lampCount;
uniform vec4 u_lampPos[MAX_LAMPS];    // xyz the fitting's centre, w the sphere that stands for it
uniform vec4 u_lampRgb[MAX_LAMPS];    // rgb intensity, w which shadow cube (< 0 is unshadowed)
uniform sampler2D u_lampShadow;
uniform vec4 u_lampAtlas;             // 1/width, 1/height, texels a tile, tiles a row
uniform vec2 u_lampDepth;             // the metres a stored byte spans, and the bias
`;

export const LAMP_SHADING = `
// Which face of the cube a direction lands on, and where in that face. The inverse of
// cube_direction() in tools/bake/lights.hpp, and the two are only correct together — the face
// order is +X -X +Y -Y +Z -Z and the sign of every axis in here matches a line over there.
float lamp_visible(float slot, vec3 toPoint, float dist) {
    if (slot < 0.0) return 1.0;
    vec3 a = abs(toPoint);
    float ma;
    float face;
    vec2 uv;
    if (a.x >= a.y && a.x >= a.z) {
        ma = a.x;
        face = (toPoint.x > 0.0) ? 0.0 : 1.0;
        uv = (toPoint.x > 0.0) ? vec2(-toPoint.z, -toPoint.y) : vec2(toPoint.z, -toPoint.y);
    } else if (a.y >= a.z) {
        ma = a.y;
        face = (toPoint.y > 0.0) ? 2.0 : 3.0;
        uv = (toPoint.y > 0.0) ? vec2(toPoint.x, toPoint.z) : vec2(toPoint.x, -toPoint.z);
    } else {
        ma = a.z;
        face = (toPoint.z > 0.0) ? 4.0 : 5.0;
        uv = (toPoint.z > 0.0) ? vec2(toPoint.x, -toPoint.y) : vec2(-toPoint.x, -toPoint.y);
    }
    uv /= max(ma, 1e-6);

    float tile = u_lampAtlas.z;
    float perRow = u_lampAtlas.w;
    float index = slot * 6.0 + face;
    float col = floor(mod(index, perRow));
    float row = floor(index / perRow);
    // Half a texel in from the tile's own edge. A bilinear fetch reads four texels, and without the
    // inset the ones at a tile boundary are a different DIRECTION of a different LIGHT — which
    // draws a one-pixel frame of somebody else's shadow round every face of every cube.
    vec2 px = clamp((uv * 0.5 + 0.5) * tile, vec2(0.5), vec2(tile - 0.5));
    vec2 at = (vec2(col, row) * tile + px) * u_lampAtlas.xy;
    float stored = texture(u_lampShadow, at).r * u_lampDepth.x;

    // A ramp rather than a step. The stored distance is filtered between texels, so a shadow
    // boundary crosses this over a fraction of a texel and comes out soft instead of stair-stepped;
    // a step() there is a staircase whose treads are the cube's own resolution.
    float soft = max(0.10, dist * 0.05);
    return clamp((stored + u_lampDepth.y - dist) / soft, 0.0, 1.0);
}

// Every lamp the draw was given, at one point. fresnel() and smith() are the surface shader's own,
// so a highlight from a lamp is the same lobe as a highlight from the sun. (No back-quotes below
// this line: everything from the opening quote of LAMP_SHADING is inside a template string, and one
// back-quote in a comment ends it in the middle of a sentence.)
void lamps(vec3 P, vec3 N, vec3 V, vec3 albedo, float metal, float rough, vec3 f0,
           out vec3 lampDiffuse, out vec3 lampSpecular) {
    lampDiffuse = vec3(0.0);
    lampSpecular = vec3(0.0);
    if (u_lampCount <= 0) return;
    float ndv = max(dot(N, V), 1e-4);
    float alpha = rough * rough;
    for (int i = 0; i < MAX_LAMPS; ++i) {
        if (i >= u_lampCount) break;
        vec3 delta = u_lampPos[i].xyz - P;
        float d2 = dot(delta, delta);
        float dist = sqrt(d2);
        vec3 L = delta / max(dist, 1e-4);
        float ndl = dot(N, L);
        if (ndl <= 0.0) continue;

        float radius = u_lampPos[i].w;
        // The fitting is a sphere and not a point: inside its own radius an inverse square runs
        // away, so d^2 is clamped to it. fittings.clip is explicit that its bowls are small on
        // purpose, and this is where being small stops meaning "infinitely bright close up".
        float attenuation = 1.0 / max(d2, radius * radius);
        float visible = lamp_visible(u_lampRgb[i].w, -delta, dist);
        if (visible <= 0.0) continue;
        vec3 arriving = u_lampRgb[i].rgb * attenuation * visible;

        lampDiffuse += albedo * (1.0 - metal) * arriving * ndl;

        // The lobe, widened by the angle the fitting subtends and renormalised for the energy that
        // widening adds -- Karis's sphere light. Without it a 0.11 m bowl four metres away is one
        // blinding pixel on a polished floor instead of a highlight shaped like the bowl, and the
        // difference between a small bright source and a large soft one disappears.
        vec3 H = normalize(L + V);
        float ndh = max(dot(N, H), 0.0);
        float vdh = max(dot(V, H), 0.0);
        float wide = clamp(alpha + radius / (2.0 * max(dist, 1e-3)), 1e-4, 1.0);
        float energy = (alpha * alpha) / (wide * wide);
        float denom = ndh * ndh * (wide * wide - 1.0) + 1.0;
        float D = (wide * wide) / (3.14159265 * denom * denom + 1e-7);
        lampSpecular += fresnel(f0, vdh) * D * smith(ndv, ndl, rough) * energy /
                        (4.0 * ndv * max(ndl, 1e-4) + 1e-4) * arriving * ndl;
    }
}
`;

// One light, as the baker wrote it. Read once at load; nothing here is touched per frame except
// `score`.
function readLights(bytes) {
    const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    const version = view.getUint32(0, true);
    if (version !== 1) throw new Error('LGTS version ' + version + ', this viewer reads 1');
    const count = view.getUint32(4, true);
    const head = {
        count,
        shadowed: view.getUint32(8, true),
        tile: view.getUint32(12, true),
        atlasWidth: view.getUint32(16, true),
        atlasHeight: view.getUint32(20, true),
        range: view.getFloat32(24, true),
        tilesPerRow: view.getUint32(28, true),
    };
    const lights = [];
    let at = 32;
    for (let i = 0; i < count; ++i) {
        lights.push({
            x: view.getFloat32(at, true),
            y: view.getFloat32(at + 4, true),
            z: view.getFloat32(at + 8, true),
            radius: view.getFloat32(at + 12, true),
            r: view.getFloat32(at + 16, true),
            g: view.getFloat32(at + 20, true),
            b: view.getFloat32(at + 24, true),
            area: view.getFloat32(at + 28, true),
            hx: view.getFloat32(at + 32, true),
            hy: view.getFloat32(at + 36, true),
            hz: view.getFloat32(at + 40, true),
            shadow: view.getInt32(at + 44, true),
            // Luminance, not the sum: what the eye gets out of it, which is what the rank is for.
            power: 0.2126 * view.getFloat32(at + 16, true) + 0.7152 * view.getFloat32(at + 20, true) +
                   0.0722 * view.getFloat32(at + 24, true),
            score: 0,
        });
        at += LIGHT_BYTES;
    }
    const atlasBytes = head.atlasWidth * head.atlasHeight;
    const atlas = (atlasBytes > 0 && at + atlasBytes <= bytes.byteLength)
        ? bytes.subarray(at, at + atlasBytes)
        : null;
    return { head, lights, atlas };
}

export class LightSet {
    constructor(gl) {
        this.gl = gl;
        this.lights = [];
        this.head = null;
        this.count = 0;        // how many the clip has
        this.active = 0;       // how many this draw is shading
        this.shadowed = 0;
        this.unshadowed = 0;
        this.capped = false;
        // Two arms of one control, so the difference this makes can be seen without a rebuild:
        // `?nolamps` on the URL turns the whole term off and leaves everything else alone.
        this.enabled = !(typeof location !== 'undefined' &&
                         /(\?|&)nolamps\b/.test(location.search));
        // And the same for the shadows alone, which is the arm that shows what the baked cubes are
        // buying: with `?noshadow` every lamp lights through every wall, and many_lamps -- four
        // quarters walled off from each other -- lights up as one room.
        this.shadowsOn = !(typeof location !== 'undefined' &&
                           /(\?|&)noshadow\b/.test(location.search));
        this.position = new Float32Array(MAX_LAMPS * 4);
        this.colour = new Float32Array(MAX_LAMPS * 4);
        this.atlas = gl.createTexture();
        this.blank = gl.createTexture();
        // A sampler bound to nothing is undefined behaviour rather than a blank hole, so there is
        // always a texture. 255 is "nothing between here and the range", which is what a clip with
        // no shadow atlas should read everywhere.
        gl.bindTexture(gl.TEXTURE_2D, this.blank);
        gl.texImage2D(gl.TEXTURE_2D, 0, gl.R8, 1, 1, 0, gl.RED, gl.UNSIGNED_BYTE,
                      new Uint8Array([255]));
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
        this.hasAtlas = false;
        this.bias = 0.1;
    }

    setClip(clip) {
        const gl = this.gl;
        this.lights = [];
        this.head = null;
        this.count = 0;
        this.active = 0;
        this.shadowed = 0;
        this.unshadowed = 0;
        this.capped = false;
        this.hasAtlas = false;
        const bytes = clip.chunks ? clip.chunks.get('LGTS') : null;
        if (!bytes || bytes.byteLength < 32) return;

        let read = null;
        try {
            read = readLights(bytes);
        } catch (error) {
            console.warn('lights: ' + error.message);
            return;
        }
        this.head = read.head;
        this.lights = read.lights;
        this.count = read.lights.length;
        this.shadowed = read.head.shadowed;
        this.unshadowed = Math.max(0, this.count - this.shadowed);
        // Two voxels and a bit. The baked byte is rounded UP to the next quantum, so the bias is
        // only paying for the ray's own step and not for the quantisation -- see the note in
        // tools/bake/lights.hpp about which way that rounding has to go.
        this.bias = 2.5 / Math.max(1, clip.metre);

        if (read.atlas) {
            gl.bindTexture(gl.TEXTURE_2D, this.atlas);
            gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);
            gl.texImage2D(gl.TEXTURE_2D, 0, gl.R8, read.head.atlasWidth, read.head.atlasHeight, 0,
                          gl.RED, gl.UNSIGNED_BYTE, read.atlas);
            // LINEAR, and it is what makes the shadow soft: the stored value is a DISTANCE, so a
            // filtered one is a distance partway between two directions and the comparison against
            // it lands partway through a texel instead of on its edge.
            gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
            gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
            gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
            gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
            this.hasAtlas = true;
        }

        // Said once, on the console, rather than left to be inferred from a picture. The two
        // numbers that matter are how many are shaded and how many are unshadowed, and both of them
        // are things somebody looking at a wrong-looking room needs to be able to read off.
        const kept = Math.min(this.count, MAX_LAMPS);
        console.log('lights: ' + this.count + ' in this clip, ' + kept + ' shaded per draw' +
                    (this.count > MAX_LAMPS ? ' (the cap bites)' : '') + ', ' + this.shadowed +
                    ' shadowed, ' + this.unshadowed + ' NOT — atlas ' + read.head.atlasWidth + '×' +
                    read.head.atlasHeight + (this.enabled ? '' : ' — turned off by ?nolamps'));
    }

    // Rank and cap. Sorted by what each delivers at the eye — intensity over the square of the
    // distance — and then the strongest MAX_LAMPS are packed into the two uniform arrays. A partial
    // selection rather than a sort: the list can be 256 long and only sixteen of it is wanted.
    rank(eye) {
        this.active = 0;
        this.capped = false;
        if (!this.enabled || this.count === 0) return;
        const lights = this.lights;
        for (let i = 0; i < lights.length; ++i) {
            const l = lights[i];
            const dx = l.x - eye[0], dy = l.y - eye[1], dz = l.z - eye[2];
            const d2 = Math.max(dx * dx + dy * dy + dz * dz, l.radius * l.radius);
            l.score = l.power / d2;
        }
        const keep = Math.min(MAX_LAMPS, lights.length);
        this.capped = lights.length > MAX_LAMPS;
        // Selection: `keep` passes over the list, which for 16 of 256 is a quarter of what a sort
        // would cost and does not disturb the array the shadow indices are read from.
        const taken = new Uint8Array(lights.length);
        for (let k = 0; k < keep; ++k) {
            let best = -1;
            let most = -1;
            for (let i = 0; i < lights.length; ++i) {
                if (taken[i]) continue;
                if (lights[i].score > most) { most = lights[i].score; best = i; }
            }
            if (best < 0 || most <= 0) break;
            taken[best] = 1;
            const l = lights[best];
            const at = this.active * 4;
            this.position[at + 0] = l.x;
            this.position[at + 1] = l.y;
            this.position[at + 2] = l.z;
            this.position[at + 3] = l.radius;
            this.colour[at + 0] = l.r;
            this.colour[at + 1] = l.g;
            this.colour[at + 2] = l.b;
            // Where its shadow cube is, or -1. Carried as a float because it rides in a vec4.
            this.colour[at + 3] = (this.hasAtlas && this.shadowsOn && l.shadow >= 0) ? l.shadow : -1;
            this.active += 1;
        }
    }

    // Into a program's uniforms. `unit` is the texture unit the atlas goes on; gl.js owns the
    // numbering and 0 and 1 are the materials and the light grid.
    bind(uniforms, unit) {
        const gl = this.gl;
        if (uniforms.u_lampCount === undefined) return;
        gl.uniform1i(uniforms.u_lampCount, this.active);
        if (this.active > 0) {
            gl.uniform4fv(uniforms.u_lampPos, this.position);
            gl.uniform4fv(uniforms.u_lampRgb, this.colour);
        }
        const head = this.head;
        gl.uniform4fv(uniforms.u_lampAtlas, head && this.hasAtlas
            ? [1 / head.atlasWidth, 1 / head.atlasHeight, head.tile, head.tilesPerRow]
            : [1, 1, 1, 1]);
        gl.uniform2fv(uniforms.u_lampDepth, [head ? head.range : 1, this.bias]);
        gl.activeTexture(gl.TEXTURE0 + unit);
        gl.bindTexture(gl.TEXTURE_2D, this.hasAtlas ? this.atlas : this.blank);
        gl.uniform1i(uniforms.u_lampShadow, unit);
    }
}
