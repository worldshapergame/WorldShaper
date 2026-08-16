// Running a clip's FIELD in the browser — the same function `src/forge/field.cpp`'s `eval` is.
//
// The ◉ view draws the shapes an author wrote. It draws them all the same grey, because a shape in
// this language has no material: colour comes from a stack of paint rules, and a rule is a FIELD, a
// range that field must fall in, and optionally a direction the surface must face. So to paint the
// ray-marched view at all, something has to be able to answer "what is this field, here" at the
// point the ray hit — and that is this file.
//
// # What it is
//
//   FIELD_OPS      the op numbering, in ONE place. See below for why it is not the enum's.
//   fieldGlsl()    a GLSL ES 3.00 chunk defining `float field_eval(uint node, vec3 p)`
//   parseFieldGraph()  reads an FLDG chunk into the words a texture wants
//   FieldTexture   uploads those words and binds them
//
// Nothing here draws anything. It is a chunk to paste into somebody else's fragment shader and the
// three lines of setup that chunk needs, so that the rule evaluator and the marcher can each stay
// in their own file.
//
// # The op numbering is the baker's, not the enum's
//
// `src/forge/field.hpp`'s `Op` is a plain enum whose values shift the moment anybody inserts a
// solid into the middle of the list — which has already happened once, when `arc` was added. A
// format that shipped the enum's numbers would draw cylinders as capsules the first time somebody
// added a shape and nothing would say so. So the numbering is declared HERE, it is the numbering
// the exporter must write, and **0..7 are deliberately the same eight `web_op` in
// `tools/bake_web.cpp` already assigns**, so the shape/cutter pool and the field graph agree about
// the eight ops they share.
//
// # Recursion, and how it is not done
//
// `Field::eval` descends a tree and GLSL has no recursion. This is an **explicit stack**, and it is
// a transliteration of `Field::mirror_eval` — the non-recursive twin that already exists on the CPU
// in `src/forge/field.cpp`, written there precisely so the shader would be a transliteration of
// something already proved rather than something argued about. One frame per node on the way down,
// a `step` counter over SAMPLE POINTS rather than over children (which is what lets `curvature` ask
// its child seven times, `occlusion` fourteen and `repeat` up to eight), and one register for the
// value the child that just finished came back with.
//
// Two things this file does that the CPU twin does not have to:
//
// - **The frame is four slots and no more**: node, step, point, and a `vec3` of accumulators. The
//   CPU frame also caches `repeat`'s folded point and its leaning neighbours; here that is
//   RECOMPUTED on every revisit, because ten floats a frame times a stack depth is private memory
//   a fragment shader pays for on every pixel and the fold is a dozen instructions.
// - **The depth is compiled in.** `fieldGlsl({ stack })` takes the depth from the graph the page
//   actually loaded — `parseFieldGraph` measures it — so a clip whose deepest paint rule is four
//   nodes gets a stack of four. The hard ceiling is FIELD_MAX_STACK, and a graph past it is
//   REFUSED rather than truncated: `field_eval_ok` returns false, which a rule reads as "no match".
//   "I could not" and "the answer is nought" must never be the same reply.
//
// # What it costs
//
// One evaluation per fragment per rule, at the marched HIT POINT — not per march step. That is the
// whole reason this is affordable: the march is 96 steps of shape-and-cutters, and the paint is one
// walk of one rule's field afterwards. A rule keyed on a single `fbm` is four texel fetches and
// eight hashes; a rule keyed on a union of thirty solids is thirty leaf evaluations, once.
//
// # Where it is not the C++
//
// Everything here is `highp float` where the C++ is `double`, and the places that costs something
// are written down in documentation/24-clip-viewer.md §4c. The short of it: the arithmetic agrees
// to about 1e-6, and the one real hazard is `floor(p / size)` for a fine noise far from the origin,
// where a single-precision point can land in the neighbouring cell and the noise is then a
// completely different number. That is a property of f32 and not a bug to be fixed here.

// --- the op numbering ---------------------------------------------------------------------------
//
// One place. Swapping to another numbering is one edit to this table and nothing else in the
// viewer changes, because the GLSL constants are generated from it.
export const FIELD_OPS = {
    // leaf solids — 0..7 are `web_op`'s, unchanged, deliberately
    SPHERE: 0,
    BOX: 1,
    CYLINDER: 2,
    CAPSULE: 3,
    TORUS: 4,
    CONE: 5,
    PLANE: 6,
    ELLIPSOID: 7,
    PRISM: 8,
    PLATONIC: 9,
    WEDGE: 10,
    STAIRS: 11,
    ARC: 12,
    SPIRAL: 13,
    // leaf sources
    CONSTANT: 14,
    COORDINATE: 15,   // `axis`
    RADIUS: 16,       // `distance`
    // leaf patterns
    SINE: 17,
    WAVES: 18,
    NOISE: 19,
    FBM: 20,
    RIDGED: 21,
    RASP: 22,
    CELLS: 23,
    CELL_EDGE: 24,
    CHECKER: 25,
    STRIPES: 26,
    BRICKS: 27,
    // --- everything at or below 27 has no children. The shader tests exactly that. ---
    // combining
    UNION: 28,
    INTERSECTION: 29,
    DIFFERENCE: 30,
    SMOOTH_UNION: 31,
    SMOOTH_INTERSECTION: 32,
    SMOOTH_DIFFERENCE: 33,
    // moving the point before asking
    TRANSLATE: 34,
    ROTATE: 35,
    SCALE: 36,
    MIRROR: 37,
    REPEAT: 38,
    POLAR_REPEAT: 39,   // `around`
    TWIST: 40,
    BEND: 41,
    REVOLVE: 42,
    // changing the answer
    SHELL: 43,
    ROUND: 44,
    OFFSET: 45,
    DISPLACE: 46,
    // what the shape is doing here
    CURVATURE: 47,
    OCCLUSION: 48,
    FACING: 49,
    // arithmetic
    ADD: 50,
    MULTIPLY: 51,
    MIN: 52,
    MAX: 53,
    BLEND: 54,
    REMAP: 55,
    ABS: 56,
    NEGATE: 57,
    STEP: 58,
    SMOOTHSTEP: 59,
    CLAMP: 60,
    POWER: 61,
    // Not evaluable from the graph alone: a Parameter node holds a SLOT INDEX in a[0] and the value
    // lives in the field's parameter table, which the FLDG chunk does not carry. The exporter must
    // fold it to a CONSTANT holding the current value. Until it does, this returns "no match"
    // rather than painting with a slot number.
    PARAMETER: 62,
};

// The last op with no children. `fld_is_leaf` in the shader is `op <= FIELD_LEAF_MAX`, which is the
// only thing in the GLSL that depends on the ORDER of the table above rather than its values.
export const FIELD_LEAF_MAX = FIELD_OPS.BRICKS;

// --- the wire format --------------------------------------------------------------------------
//
// FLDG: u32 nodeCount, then nodeCount x
//   u32 op; u32 childCount; u32 child[4]; f32 a[8]; f32 lo[3]; f32 hi[3];
// which is twenty 32-bit words, so exactly five RGBA texels a node and nothing straddles a texel.
export const FIELD_NODE_WORDS = 20;
export const FIELD_NODE_BYTES = FIELD_NODE_WORDS * 4;
export const FIELD_NODE_TEXELS = FIELD_NODE_WORDS / 4;

// The ceiling on the compiled-in stack. 32 is well past the 24 the deepest paint rule in the
// facility reaches; the field's SOLID goes to 41, and a rule keyed on the whole building would be
// refused. That is the right failure: a stack of 41 vec3s per fragment is private memory nobody can
// afford, and a refusal says so where a silent truncation would paint the wrong stone.
export const FIELD_MAX_STACK = 32;

// One texel row cannot exceed the card's MAX_TEXTURE_SIZE; 1020 is a multiple of five under every
// floor anybody ships, so a node never straddles two rows and the index arithmetic in the shader
// stays one divide and one modulo.
const FIELD_TEXTURE_WIDTH = 1020;

// --- reading an FLDG chunk ----------------------------------------------------------------------

// `bytes` is the chunk's payload — a Uint8Array starting at the `u32 nodeCount`. Returns the words
// a texture wants plus the two numbers the shader has to be compiled with.
//
// The bytes are copied rather than viewed, deliberately: a chunk starts wherever the block before it
// ended, which is not a multiple of four, and `Uint32Array` wants alignment. One copy of a few
// hundred kilobytes at load beside a fetch of megabytes.
export function parseFieldGraph(bytes) {
    if (!bytes || bytes.length < 4) return emptyGraph();
    const copy = (bytes.byteOffset % 4 === 0)
        ? new Uint32Array(bytes.buffer, bytes.byteOffset, bytes.length >> 2)
        : new Uint32Array(bytes.slice().buffer);
    const nodeCount = copy[0];
    const wanted = 1 + nodeCount * FIELD_NODE_WORDS;
    if (nodeCount === 0 || copy.length < wanted) {
        if (nodeCount !== 0) {
            throw new Error('field graph says ' + nodeCount + ' nodes, which wants ' + wanted +
                            ' words and the chunk holds ' + copy.length);
        }
        return emptyGraph();
    }
    // The node table alone; the count stays out of the texture so node n is texel 5n and no reader
    // has to remember a one-word bias.
    const words = copy.slice(1, wanted);
    return { nodeCount, words, floats: new Float32Array(words.buffer) };
}

function emptyGraph() {
    return { nodeCount: 0, words: new Uint32Array(0), floats: new Float32Array(0) };
}

// How deep a walk of this subtree goes, and whether every op in it is one the shader implements.
//
// Both are wanted before the shader is compiled: the depth decides the stack, and an unimplemented
// op decides whether a rule can be shown at all. Iterative — a checker that recurses to work out
// how deep something recurses is a poor joke, and the graph really can be 3474 nodes.
//
// Depth is measured the way the stack is used: a node's depth is one plus the deepest of its
// children, and a leaf is one.
export function fieldDepth(graph, root) {
    if (graph.nodeCount === 0 || root >= graph.nodeCount) return { depth: 0, ok: false, missing: -1 };
    const depth = new Int32Array(graph.nodeCount).fill(-1);
    const order = [];
    const seen = new Uint8Array(graph.nodeCount);
    const stack = [root >>> 0];
    let missing = -1;
    while (stack.length > 0) {
        const at = stack.pop();
        if (at >= graph.nodeCount || seen[at]) continue;
        seen[at] = 1;
        order.push(at);
        const base = at * FIELD_NODE_WORDS;
        const op = graph.words[base];
        if (!fieldOpImplemented(op)) missing = op;
        const children = Math.min(graph.words[base + 1], 4);
        for (let c = 0; c < children; ++c) stack.push(graph.words[base + 2 + c]);
    }
    // Children before parents: `order` is a pre-order push list, so walking it backwards settles
    // every child before the node that asks about it, whatever shape the sharing takes.
    for (let i = order.length - 1; i >= 0; --i) {
        const at = order[i];
        const base = at * FIELD_NODE_WORDS;
        const children = Math.min(graph.words[base + 1], 4);
        let best = 0;
        for (let c = 0; c < children; ++c) {
            const child = graph.words[base + 2 + c];
            if (child < graph.nodeCount && depth[child] > best) best = depth[child];
        }
        depth[at] = best + 1;
    }
    return { depth: depth[root], ok: missing < 0, missing };
}

// Whether the shader has this op at all. `PARAMETER` is the only number in the table that is not
// implemented, and it is listed rather than left out so the answer to "why is this rule not
// painting" is a name.
export function fieldOpImplemented(op) {
    return op <= FIELD_OPS.POWER;
}

// The op's name, for a diagnostic. A histogram over op numbers is a histogram nobody can act on.
export function fieldOpName(op) {
    for (const [name, value] of Object.entries(FIELD_OPS)) {
        if (value === op) return name.toLowerCase();
    }
    return 'op ' + op;
}

// --- the texture --------------------------------------------------------------------------------

// The graph as one RGBA32UI texture, read with `texelFetch` and nothing else.
//
// UI rather than F, and one texture rather than two: `uintBitsToFloat` is in GLSL ES 3.00, so a
// single unsigned fetch serves both halves of a node — the op and the children as integers, the
// eight arguments and the box as floats — for half the memory and half the fetches of shipping the
// same bytes twice.
export class FieldTexture {
    constructor(gl, unit = 0) {
        this.gl = gl;
        this.unit = unit;
        this.texture = gl.createTexture();
        this.width = 1;
        this.height = 1;
        this.nodeCount = 0;
        // There is always a texture, even a 1x1 one: a sampler bound to nothing is undefined
        // behaviour and not a blank hole.
        this.upload(emptyGraph());
    }

    // Leaves `this.unit` active and this texture bound to it. It has to bind something to upload,
    // and WebGL has no cheap way to put back what was there — so it names the unit it will disturb
    // rather than disturbing whichever one happened to be current. Getting that wrong is not loud:
    // an integer texture left under somebody's `sampler2D` is INVALID_OPERATION at DRAW time, with
    // nothing on screen and no hint which of the four textures did it.
    upload(graph) {
        const gl = this.gl;
        gl.activeTexture(gl.TEXTURE0 + this.unit);
        this.nodeCount = graph.nodeCount;
        const texels = Math.max(1, graph.nodeCount * FIELD_NODE_TEXELS);
        this.width = Math.min(FIELD_TEXTURE_WIDTH, texels);
        this.height = Math.ceil(texels / this.width);
        const padded = new Uint32Array(this.width * this.height * 4);
        if (graph.nodeCount > 0) padded.set(graph.words);
        gl.bindTexture(gl.TEXTURE_2D, this.texture);
        gl.pixelStorei(gl.UNPACK_ALIGNMENT, 4);
        gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA32UI, this.width, this.height, 0,
                      gl.RGBA_INTEGER, gl.UNSIGNED_INT, padded);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
        return this;
    }

    // Binds the texture and sets the two uniforms the chunk declares. `uniforms` is the map `link`
    // in gl.js builds; a program without the chunk in it simply has neither and nothing happens.
    bind(uniforms, unit) {
        const gl = this.gl;
        if (unit !== undefined) this.unit = unit;
        gl.activeTexture(gl.TEXTURE0 + this.unit);
        gl.bindTexture(gl.TEXTURE_2D, this.texture);
        if (uniforms.u_field !== undefined) gl.uniform1i(uniforms.u_field, this.unit);
        if (uniforms.u_fieldWidth !== undefined) gl.uniform1i(uniforms.u_fieldWidth, this.width);
    }

    dispose() {
        this.gl.deleteTexture(this.texture);
        this.texture = null;
    }
}

// --- the GLSL -----------------------------------------------------------------------------------

function opConstants() {
    const lines = [];
    for (const [name, value] of Object.entries(FIELD_OPS)) {
        lines.push('const int FLD_' + name + ' = ' + value + ';');
    }
    lines.push('const int FLD_LEAF_MAX = ' + FIELD_LEAF_MAX + ';');
    lines.push('const int FLD_TEXELS = ' + FIELD_NODE_TEXELS + ';');
    return lines.join('\n');
}

// The chunk. Paste it into a fragment shader after its `precision` lines and before anything that
// calls `field_eval`.
//
//   stack    how deep the walk may go. Take it from `fieldDepth`, not from a guess.
//   steps    the loop's own bound, in pushes and pops. GLSL ES 3.00 allows an unbounded loop and
//            a card that meets one because of a bug in this file hangs the tab, so it is bounded.
//   octaves  the most `fbm` will stack. The C++ has no cap; this is one, and a clip past it is a
//            difference in the answer rather than a crash, so it is generous.
//   sides    the most a `prism` will have faces. Same reasoning.
export function fieldGlsl(options = {}) {
    const stack = Math.max(1, Math.min(FIELD_MAX_STACK, options.stack || 16));
    const steps = Math.max(16, options.steps || 4096);
    const octaves = Math.max(1, options.octaves || 16);
    const sides = Math.max(3, options.sides || 64);
    return `
// ================= field_eval: src/forge/field.cpp's eval, walked the way a shader must =========
${opConstants()}

#define FLD_STACK ${stack}
#define FLD_STEPS ${steps}
#define FLD_OCTAVES ${octaves}
#define FLD_SIDES ${sides}

const float FLD_PI = 3.14159265358979;
const float FLD_TAU = 6.28318530717959;
const float FLD_FAR = 1e30;

uniform highp usampler2D u_field;
uniform int u_fieldWidth;

uvec4 fld_word(int texel) {
    return texelFetch(u_field, ivec2(texel % u_fieldWidth, texel / u_fieldWidth), 0);
}

// --- the small shared arithmetic ---------------------------------------------------------------

float fld_axis(vec3 p, int axis) { return (axis == 0) ? p.x : ((axis == 1) ? p.y : p.z); }

vec3 fld_set(vec3 p, int axis, float v) {
    if (axis == 0) p.x = v; else if (axis == 1) p.y = v; else p.z = v;
    return p;
}

// The two axes that are not this one, ASCENDING — not cyclic. Cyclic order makes the first axis of
// a y-prism's cross-section z, so a hexagon with no turn points along z about y and along y about
// x: three conventions wearing one name. See other_axes in field.cpp.
void fld_other(int axis, out int u, out int v) {
    if (axis == 0) { u = 1; v = 2; } else if (axis == 1) { u = 0; v = 2; } else { u = 0; v = 1; }
}

float fld_smooth_min(float a, float b, float k) {
    if (k <= 0.0) return min(a, b);
    float h = clamp(0.5 + 0.5 * (b - a) / k, 0.0, 1.0);
    return b * (1.0 - h) + a * h - k * h * (1.0 - h);
}

float fld_smooth_max(float a, float b, float k) { return -fld_smooth_min(-a, -b, k); }

// std::round: to nearest, ties AWAY FROM ZERO. GLSL's own round() leaves ties to the driver and
// floor(x + 0.5) rounds a negative tie the other way, which puts a repeat's fold in the next cell
// along at exactly half a period. Measure-zero and still wrong, so it is written out.
float fld_round(float x) { return (x < 0.0) ? -floor(-x + 0.5) : floor(x + 0.5); }

// A turn folded into [0, 1). floor rather than mod because mod keeps the sign, and a tiny negative
// turn can round up to exactly 1.
float fld_wrap_turn(float t) {
    float w = t - floor(t);
    return (w < 1.0) ? w : 0.0;
}

bool fld_partial(float span) { return span > 0.0 && span < 1.0; }

// Which end of an arc a point outside it is nearer to, in turns: positive past to, negative short
// of from. The nearer end by ANGLE is the nearer end by DISTANCE — see nearer_end in field.cpp.
float fld_nearer_end(float rel, float span) {
    float past = rel - span;
    float before = 1.0 - rel;
    return (past <= before) ? past : -before;
}

// Which of a partial around's copies a point belongs to, as the turn that copy stands at. n copies
// and n-1 gaps, the first ON from and the last ON to.
float fld_polar_copy(float turn, float from, float span, int count) {
    if (count <= 1) return from;
    float step = span / float(count - 1);
    float rel = fld_wrap_turn(turn - from);
    if (rel <= span) {
        float k = clamp(fld_round(rel / step), 0.0, float(count - 1));
        return from + k * step;
    }
    return (fld_nearer_end(rel, span) > 0.0) ? from + span : from;
}

// --- deterministic value noise: the same bits the C++ hashes ------------------------------------
//
// THIS IS THE PART THAT CANNOT BE CHECKED BY LOOKING. A rule keyed on above=0.55 paints somewhere
// else entirely if the hash, the gradient weights, the octave seeds or the octave count differ by
// anything at all, and the picture stays perfectly plausible. Every line below is
// src/forge/field.cpp's, transliterated, and it is checked numerically against that file rather
// than by eye.
uint fld_hash(uint x) {
    x ^= x >> 16u;
    x *= 0x7feb352du;
    x ^= x >> 15u;
    x *= 0x846ca68bu;
    x ^= x >> 16u;
    return x;
}

float fld_unit(ivec3 c, uint seed) {
    uint h = fld_hash(uint(c.x) * 0x8da6b343u ^ uint(c.y) * 0xd8163841u ^
                      uint(c.z) * 0xcb1ab31fu ^ seed);
    return float(h) * (1.0 / 4294967296.0);
}

float fld_smoother(float t) { return t * t * t * (t * (t * 6.0 - 15.0) + 10.0); }

// Value noise in [-1, 1], with a feature size in metres.
float fld_value_noise(vec3 p, float size, uint seed) {
    if (size <= 0.0) size = 1.0;
    vec3 q = p / size;
    vec3 f = floor(q);
    ivec3 i = ivec3(f);
    vec3 t = vec3(fld_smoother(q.x - f.x), fld_smoother(q.y - f.y), fld_smoother(q.z - f.z));
    float accum = 0.0;
    // The order is the C++'s — dz outer, dx inner — because a sum of eight floats is not
    // associative and this is meant to agree to the last bit it can.
    for (int dz = 0; dz < 2; ++dz) {
        for (int dy = 0; dy < 2; ++dy) {
            for (int dx = 0; dx < 2; ++dx) {
                float wx = (dx != 0) ? t.x : (1.0 - t.x);
                float wy = (dy != 0) ? t.y : (1.0 - t.y);
                float wz = (dz != 0) ? t.z : (1.0 - t.z);
                accum += wx * wy * wz * fld_unit(i + ivec3(dx, dy, dz), seed);
            }
        }
    }
    return accum * 2.0 - 1.0;
}

float fld_fbm(vec3 p, float size, int octaves, float gain, float lacunarity, uint seed) {
    if (octaves <= 0) return 0.0;
    float sum = 0.0;
    float amplitude = 1.0;
    float total = 0.0;
    float current = size;
    for (int i = 0; i < FLD_OCTAVES; ++i) {
        if (i >= octaves) break;
        sum += amplitude * fld_value_noise(p, current, seed + uint(i) * 131u);
        total += amplitude;
        amplitude *= gain;
        current /= (lacunarity > 0.0) ? lacunarity : 2.0;
    }
    return (total > 0.0) ? sum / total : 0.0;
}

// Distance to the nearest scattered point, and to the second nearest. The nearest gives grains and
// cobbles; the DIFFERENCE gives the seam between two cells, which is what a crack is.
void fld_cells(vec3 p, float size, uint seed, out float nearest, out float second) {
    if (size <= 0.0) size = 1.0;
    vec3 q = p / size;
    ivec3 base = ivec3(floor(q));
    float best = FLD_FAR;
    float next = FLD_FAR;
    for (int dz = -1; dz <= 1; ++dz) {
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                ivec3 c = base + ivec3(dx, dy, dz);
                vec3 jitter = vec3(fld_unit(c, seed), fld_unit(c, seed + 7919u),
                                   fld_unit(c, seed + 104729u));
                float d = length(q - (vec3(c) + jitter));
                if (d < best) { next = best; best = d; }
                else if (d < next) { next = d; }
            }
        }
    }
    nearest = best * size;
    second = next * size;
}

// --- the exact distance functions ---------------------------------------------------------------

float fld_sd_box(vec3 p, vec3 half_) {
    vec3 q = abs(p) - half_;
    return length(max(q, vec3(0.0))) + min(max(q.x, max(q.y, q.z)), 0.0);
}

float fld_sd_cylinder(vec3 p, float r, float half_height, int axis) {
    int a, b;
    fld_other(axis, a, b);
    float radial = length(vec2(fld_axis(p, a), fld_axis(p, b))) - r;
    float along = abs(fld_axis(p, axis)) - half_height;
    float outside = length(vec2(max(radial, 0.0), max(along, 0.0)));
    return min(max(radial, along), 0.0) + outside;
}

float fld_sd_capsule(vec3 p, vec3 a, vec3 b, float r) {
    vec3 pa = p - a;
    vec3 ba = b - a;
    float denom = dot(ba, ba);
    float h = (denom > 0.0) ? clamp(dot(pa, ba) / denom, 0.0, 1.0) : 0.0;
    return length(pa - ba * h) - r;
}

float fld_sd_torus(vec3 p, float ring, float tube, int axis) {
    int a, b;
    fld_other(axis, a, b);
    float radial = length(vec2(fld_axis(p, a), fld_axis(p, b))) - ring;
    return length(vec2(radial, fld_axis(p, axis))) - tube;
}

float fld_sd_cone(vec3 p, float base_r, float height, int axis) {
    int a, b;
    fld_other(axis, a, b);
    float radial = length(vec2(fld_axis(p, a), fld_axis(p, b)));
    float along = fld_axis(p, axis);
    float slope = length(vec2(height, base_r));
    float side = (radial * height + along * base_r - base_r * height) / ((slope > 0.0) ? slope : 1.0);
    return max(side, max(-along, along - height));
}

// The standard bounded approximation. An ellipsoid has no closed-form distance; this is correct in
// sign everywhere and accurate near the surface, which is what a sampler and a displacement need.
float fld_sd_ellipsoid(vec3 p, vec3 r) {
    vec3 safe = max(r, vec3(1e-9));
    float k0 = length(p / safe);
    float k1 = length(p / (safe * safe));
    if (k1 == 0.0) return -min(safe.x, min(safe.y, safe.z));
    return k0 * (k0 - 1.0) / k1;
}

// A regular n-gon prism as the intersection of n half planes. A circumradius is the distance to a
// CORNER, which is what an author means by "a hexagon this big", so the faces sit in by cos(pi/n).
float fld_sd_prism(vec3 p, float circumradius, float half_height, int sides, int axis, float turn) {
    sides = max(sides, 3);
    int a, b;
    fld_other(axis, a, b);
    float u = fld_axis(p, a);
    float v = fld_axis(p, b);
    float apothem = circumradius * cos(FLD_PI / float(sides));
    float cross_ = -FLD_FAR;
    for (int i = 0; i < FLD_SIDES; ++i) {
        if (i >= sides) break;
        float angle = FLD_TAU * (float(i) / float(sides) + turn);
        cross_ = max(cross_, u * cos(angle) + v * sin(angle) - apothem);
    }
    float along = abs(fld_axis(p, axis)) - half_height;
    float outside = length(vec2(max(cross_, 0.0), max(along, 0.0)));
    return min(max(cross_, along), 0.0) + outside;
}

// The five Platonic solids, each as the intersection of its face planes, sized by CIRCUMRADIUS —
// the distance to a vertex, which is the size a person means.
//
// A face normal and its negation both appear in four of the five, and max(x, -x) is abs(x) exactly,
// so those pairs are one abs rather than two dot products. The octahedron's eight normals are every
// sign of (1,1,1)/sqrt(3), whose maximum over the signs is the same dot taken against abs(p).
float fld_sd_platonic(vec3 p, float circumradius, int which) {
    float phi = (1.0 + sqrt(5.0)) * 0.5;
    if (which == 1) {   // the cube, exactly, rather than as six planes
        float h = circumradius / sqrt(3.0);
        return fld_sd_box(p, vec3(h, h, h));
    }
    float d = -FLD_FAR;
    float face_over_circum = 1.0;
    if (which == 0) {                       // tetrahedron
        face_over_circum = 1.0 / 3.0;
        d = max(d, dot(p, normalize(vec3(-1.0, -1.0, -1.0))));
        d = max(d, dot(p, normalize(vec3(-1.0, 1.0, 1.0))));
        d = max(d, dot(p, normalize(vec3(1.0, -1.0, 1.0))));
        d = max(d, dot(p, normalize(vec3(1.0, 1.0, -1.0))));
    } else if (which == 2) {                // octahedron
        face_over_circum = 1.0 / sqrt(3.0);
        d = dot(abs(p), normalize(vec3(1.0, 1.0, 1.0)));
    } else if (which == 3) {                // dodecahedron: twelve faces, six axes
        face_over_circum = 0.7947;
        d = max(d, abs(dot(p, normalize(vec3(0.0, 1.0, phi)))));
        d = max(d, abs(dot(p, normalize(vec3(0.0, 1.0, -phi)))));
        d = max(d, abs(dot(p, normalize(vec3(1.0, phi, 0.0)))));
        d = max(d, abs(dot(p, normalize(vec3(1.0, -phi, 0.0)))));
        d = max(d, abs(dot(p, normalize(vec3(phi, 0.0, 1.0)))));
        d = max(d, abs(dot(p, normalize(vec3(-phi, 0.0, 1.0)))));
    } else {                                // icosahedron: twenty faces, ten axes
        face_over_circum = 0.7947;
        d = max(d, abs(dot(p, normalize(vec3(1.0, 1.0, 1.0)))));
        d = max(d, abs(dot(p, normalize(vec3(1.0, 1.0, -1.0)))));
        d = max(d, abs(dot(p, normalize(vec3(1.0, -1.0, 1.0)))));
        d = max(d, abs(dot(p, normalize(vec3(1.0, -1.0, -1.0)))));
        d = max(d, abs(dot(p, normalize(vec3(0.0, 1.0 / phi, phi)))));
        d = max(d, abs(dot(p, normalize(vec3(0.0, 1.0 / phi, -phi)))));
        d = max(d, abs(dot(p, normalize(vec3(1.0 / phi, phi, 0.0)))));
        d = max(d, abs(dot(p, normalize(vec3(-1.0 / phi, phi, 0.0)))));
        d = max(d, abs(dot(p, normalize(vec3(phi, 0.0, 1.0 / phi)))));
        d = max(d, abs(dot(p, normalize(vec3(phi, 0.0, -1.0 / phi)))));
    }
    return d - circumradius * face_over_circum;
}

// A ramp: a box cut by the diagonal plane that rises along one axis as another advances.
float fld_sd_wedge(vec3 p, vec3 half_, int rise_axis, int run_axis) {
    float box = fld_sd_box(p, half_);
    float rise = fld_axis(half_, rise_axis);
    float run = fld_axis(half_, run_axis);
    if (rise <= 0.0 || run <= 0.0) return box;
    float u = fld_axis(p, run_axis);
    float v = fld_axis(p, rise_axis);
    float inv = 1.0 / length(vec2(rise, -run));
    return max(box, (u * rise + v * (-run)) * inv);
}

// A flight of steps, by folding the run into one step — so a staircase of any length costs the same.
float fld_sd_stairs(vec3 p, vec3 half_, float run, float rise, int run_axis, int rise_axis) {
    if (run <= 0.0 || rise <= 0.0) return fld_sd_box(p, half_);
    float u = fld_axis(p, run_axis) + fld_axis(half_, run_axis);
    float v = fld_axis(p, rise_axis) + fld_axis(half_, rise_axis);
    float tread = floor(u / run);
    float top = (tread + 1.0) * rise;
    return max(fld_sd_box(p, half_), v - top);
}

// The distance to a logarithmic spiral swept as a round tube — the Ionic volute. Walked as a chain
// of straight pieces rather than solved: the spiral has no closed form for the nearest point, and a
// chain of capsules has an exact distance, so what comes back is the distance to the thing that
// actually gets built.
float fld_sd_spiral(vec3 p, float start_r, float per_turn, float tube, float turns, int axis) {
    if (start_r <= 0.0 || turns <= 0.0) return FLD_FAR;
    int u, v;
    fld_other(axis, u, v);
    float px = fld_axis(p, u);
    float py = fld_axis(p, v);
    float along = fld_axis(p, axis);

    float wanted = abs(turns) * 24.0;
    int pieces = (wanted < 4.0) ? 4 : ((wanted > 320.0) ? 320 : int(wanted + 0.5));
    float step = FLD_TAU * turns / float(pieces);
    float c = cos(step), s = sin(step);
    float grow = pow((per_turn > 0.0) ? per_turn : 1.0, turns / float(pieces));

    float dirx = 1.0, diry = 0.0;
    float radius = start_r;
    float ax = radius, ay = 0.0;
    float best = FLD_FAR;
    for (int i = 0; i < 320; ++i) {
        if (i >= pieces) break;
        float ndirx = dirx * c - diry * s;
        float ndiry = dirx * s + diry * c;
        radius *= grow;
        float bx = ndirx * radius, by = ndiry * radius;
        float ex = bx - ax, ey = by - ay;
        float wx = px - ax, wy = py - ay;
        float denom = ex * ex + ey * ey;
        float t = (denom > 0.0) ? clamp((wx * ex + wy * ey) / denom, 0.0, 1.0) : 0.0;
        float dx = wx - ex * t, dy = wy - ey * t;
        float d = sqrt(dx * dx + dy * dy + along * along);
        if (d < best) best = d;
        dirx = ndirx; diry = ndiry;
        ax = bx; ay = by;
    }
    return best - tube;
}

// --- the leaves ---------------------------------------------------------------------------------
//
// Everything with no children, in one function, because none of it needs the stack. A is a[0..3]
// and B is a[4..7].
float fld_leaf(int op, vec4 A, vec4 B, vec3 p) {
    if (op == FLD_SPHERE) return length(p - A.xyz) - A.w;
    if (op == FLD_BOX) {
        // The corner radius is taken off the half extent and added back to the distance, which is
        // what makes round= a real fillet rather than a shrink.
        return fld_sd_box(p - A.xyz, vec3(A.w, B.x, B.y) - vec3(B.z)) - B.z;
    }
    if (op == FLD_CYLINDER) return fld_sd_cylinder(p - A.xyz, A.w, B.x, int(B.y));
    if (op == FLD_CAPSULE) return fld_sd_capsule(p, A.xyz, vec3(A.w, B.x, B.y), B.z);
    if (op == FLD_TORUS) return fld_sd_torus(p - A.xyz, A.w, B.x, int(B.y));
    if (op == FLD_CONE) return fld_sd_cone(p - A.xyz, A.w, B.x, int(B.y));
    if (op == FLD_PLANE) return dot(p, A.xyz) - A.w;
    if (op == FLD_ELLIPSOID) return fld_sd_ellipsoid(p - A.xyz, vec3(A.w, B.x, B.y));
    if (op == FLD_PRISM) return fld_sd_prism(p - A.xyz, A.w, B.x, int(B.y), int(B.z), B.w);
    if (op == FLD_PLATONIC) return fld_sd_platonic(p - A.xyz, A.w, int(B.x));
    if (op == FLD_WEDGE) return fld_sd_wedge(p - A.xyz, vec3(A.w, B.x, B.y), int(B.z), int(B.w));
    if (op == FLD_STAIRS) {
        return fld_sd_stairs(p - A.xyz, vec3(A.w, B.x, B.y), B.z, B.w, 2, 1);
    }
    if (op == FLD_ARC) {
        vec3 q = p - A.xyz;
        int axis = int(B.y);
        if (!fld_partial(B.w)) return fld_sd_torus(q, A.w, B.x, axis);
        int u, v;
        fld_other(axis, u, v);
        float x = fld_axis(q, u), y = fld_axis(q, v);
        float rel = fld_wrap_turn(atan(y, x) / FLD_TAU - B.z);
        // Within the arc the nearest point of the centre-line is at the asking point's own angle,
        // which is exactly what the torus already computes — so the same call answers it and the
        // two cannot drift apart.
        if (rel <= B.w) return fld_sd_torus(q, A.w, B.x, axis);
        float turn = (B.z + ((fld_nearer_end(rel, B.w) > 0.0) ? B.w : 0.0)) * FLD_TAU;
        float ex = A.w * cos(turn), ey = A.w * sin(turn);
        return length(vec2(length(vec2(x - ex, y - ey)), fld_axis(q, axis))) - B.x;
    }
    if (op == FLD_SPIRAL) {
        return fld_sd_spiral(p - A.xyz, A.w, B.x, B.y, B.z, int(B.w));
    }
    if (op == FLD_CONSTANT) return A.x;
    if (op == FLD_COORDINATE) return fld_axis(p, int(A.x));
    if (op == FLD_RADIUS) return length(p - A.xyz);
    if (op == FLD_SINE) {
        float period = (A.y != 0.0) ? A.y : 1.0;
        return sin(FLD_TAU * (fld_axis(p, int(A.x)) / period + A.z));
    }
    if (op == FLD_WAVES) {
        int u, v;
        fld_other(int(A.x), u, v);
        float pa = (A.y != 0.0) ? A.y : 1.0;
        float pb = (A.z != 0.0) ? A.z : 1.0;
        return sin(FLD_TAU * (fld_axis(p, u) / pa + A.w)) *
               sin(FLD_TAU * (fld_axis(p, v) / pb + A.w));
    }
    if (op == FLD_NOISE) return fld_value_noise(p, A.x, uint(A.y));
    if (op == FLD_FBM) return fld_fbm(p, A.x, int(A.y), A.z, A.w, uint(B.x));
    if (op == FLD_RIDGED) return 1.0 - 2.0 * abs(fld_fbm(p, A.x, int(A.y), A.z, A.w, uint(B.x)));
    if (op == FLD_RASP) {
        // Ridges an order finer than the surface they sit on: many shallow parallel gouges rather
        // than lumps. Three octaves and a lacunarity of 2.7 are fixed in the C++ and fixed here.
        return -abs(fld_fbm(p, A.x, 3, 0.5, 2.7, uint(A.z))) * A.y;
    }
    if (op == FLD_CELLS) {
        float nearest, second;
        fld_cells(p, A.x, uint(A.y), nearest, second);
        return nearest;
    }
    if (op == FLD_CELL_EDGE) {
        float nearest, second;
        fld_cells(p, A.x, uint(A.y), nearest, second);
        return second - nearest;   // zero on a seam, growing towards a cell's middle
    }
    if (op == FLD_CHECKER) {
        float sum = 0.0;
        for (int axis = 0; axis < 3; ++axis) {
            float cell = (axis == 0) ? A.x : ((axis == 1) ? A.y : A.z);
            if (cell <= 0.0) continue;
            sum += floor(fld_axis(p, axis) / cell);
        }
        return (mod(abs(sum), 2.0) < 1.0) ? -1.0 : 1.0;
    }
    if (op == FLD_STRIPES) {
        float period = (A.y != 0.0) ? A.y : 1.0;
        float t = fld_axis(p, int(A.x)) / period;
        t -= floor(t);
        return (t < A.z) ? -1.0 : 1.0;
    }
    if (op == FLD_BRICKS) {
        // Running bond: every other course offset by half a brick, and the value is how deep into
        // the mortar this point is — negative on a brick face, positive in a joint. So the same
        // node can carve the joints or colour them.
        int u, v;
        fld_other(int(B.x), u, v);
        float course_height = (A.y != 0.0) ? A.y : 1.0;
        float length_ = (A.x != 0.0) ? A.x : 1.0;
        float course = floor(fld_axis(p, v) / course_height);
        float shift = (mod(abs(course), 2.0) < 1.0) ? 0.0 : 0.5;
        float along = fld_axis(p, u) / length_ + shift;
        along -= floor(along);
        float up = fld_axis(p, v) / course_height;
        up -= floor(up);
        float joint_u = A.w / (2.0 * length_);
        float joint_v = A.w / (2.0 * course_height);
        float du = min(along, 1.0 - along) - joint_u;
        float dv = min(up, 1.0 - up) - joint_v;
        return -min(du, dv);
    }
    return FLD_FAR;
}

// What a one-child op does to its child's answer on the way OUT.
float fld_after(int op, vec4 A, float v) {
    if (op == FLD_SHELL) return abs(v) - A.x;
    if (op == FLD_ROUND) return v - A.x;
    if (op == FLD_OFFSET) return v + A.x;
    if (op == FLD_NEGATE) return -v;
    if (op == FLD_ABS) return abs(v);
    if (op == FLD_STEP) return (v > A.x) ? 1.0 : 0.0;
    if (op == FLD_SMOOTHSTEP) {
        float span = A.y - A.x;
        if (span == 0.0) return (v > A.x) ? 1.0 : 0.0;
        float t = clamp((v - A.x) / span, 0.0, 1.0);
        return t * t * (3.0 - 2.0 * t);
    }
    if (op == FLD_CLAMP) return clamp(v, A.x, A.y);
    if (op == FLD_POWER) return ((v < 0.0) ? -1.0 : 1.0) * pow(abs(v), A.x);
    if (op == FLD_REMAP) {
        float span = A.y - A.x;
        float t = (span == 0.0) ? 0.0 : clamp((v - A.x) / span, 0.0, 1.0);
        return A.z + (A.w - A.z) * t;
    }
    return v;
}

// ...and how a many-child op folds the next child into the answer so far.
float fld_fold(int op, vec4 A, float acc, float v) {
    if (op == FLD_UNION || op == FLD_MIN) return min(acc, v);
    if (op == FLD_INTERSECTION || op == FLD_MAX) return max(acc, v);
    if (op == FLD_DIFFERENCE) return max(acc, -v);
    if (op == FLD_SMOOTH_UNION) return fld_smooth_min(acc, v, A.x);
    if (op == FLD_SMOOTH_INTERSECTION) return fld_smooth_max(acc, v, A.x);
    if (op == FLD_SMOOTH_DIFFERENCE) return fld_smooth_max(acc, -v, A.x);
    if (op == FLD_ADD) return acc + v;
    if (op == FLD_MULTIPLY) return acc * v;
    return v;
}

// The fourteen directions occlusion asks along — the six axes and the eight diagonals — fixed
// rather than a random spray, because a weathering pattern that shimmers when the clip is
// re-sampled is not a pattern.
vec3 fld_occlusion_dir(int i) {
    const float k = 0.5773502691896258;
    if (i == 0) return vec3(1.0, 0.0, 0.0);
    if (i == 1) return vec3(-1.0, 0.0, 0.0);
    if (i == 2) return vec3(0.0, 1.0, 0.0);
    if (i == 3) return vec3(0.0, -1.0, 0.0);
    if (i == 4) return vec3(0.0, 0.0, 1.0);
    if (i == 5) return vec3(0.0, 0.0, -1.0);
    if (i == 6) return vec3(k, k, k);
    if (i == 7) return vec3(k, k, -k);
    if (i == 8) return vec3(k, -k, k);
    if (i == 9) return vec3(k, -k, -k);
    if (i == 10) return vec3(-k, k, k);
    if (i == 11) return vec3(-k, k, -k);
    if (i == 12) return vec3(-k, -k, k);
    return vec3(-k, -k, -k);
}

// repeat folds the point into its nearest cell and then checks the neighbouring cell on the side
// the point LEANS toward, because the fold alone answers "how far to the copy in this cell", which
// overstates whenever a copy sits off-centre — and an overstatement is the dangerous direction.
//
// Recomputed on every revisit of the frame rather than cached in it: ten floats a frame times the
// stack depth is private memory paid on every pixel, and this is a dozen instructions.
void fld_repeat_fold(vec3 p, vec4 A, vec4 B, out vec3 folded, out ivec3 axes, out vec3 lean,
                     out int neighbours) {
    folded = p;
    axes = ivec3(0);
    lean = vec3(0.0);
    neighbours = 0;
    for (int axis = 0; axis < 3; ++axis) {
        float period = (axis == 0) ? A.x : ((axis == 1) ? A.y : A.z);
        if (period <= 0.0) continue;
        float limit = (axis == 0) ? A.w : ((axis == 1) ? B.x : B.y);
        float value = fld_axis(p, axis);
        float cell = fld_round(value / period);
        if (limit > 0.0) cell = clamp(cell, -limit, limit);
        float here = value - period * cell;
        folded = fld_set(folded, axis, here);
        float other = cell + ((here >= 0.0) ? 1.0 : -1.0);
        if (limit > 0.0) other = clamp(other, -limit, limit);
        // A cell the limit has clamped away has no copy in it and must not be consulted: taking a
        // minimum against a copy that does not exist invents matter.
        if (other != cell) {
            if (neighbours == 0) { axes.x = axis; lean.x = value - period * other; }
            else if (neighbours == 1) { axes.y = axis; lean.y = value - period * other; }
            else { axes.z = axis; lean.z = value - period * other; }
            ++neighbours;
        }
    }
}

// --- the walk -----------------------------------------------------------------------------------

// Set by every call below: false when the walk met an op this file does not implement, ran out of
// stack, or ran out of steps. A caller using the plain field_eval reads this instead of trying to
// tell an answer from a refusal by its value — "I could not" and "the answer is nought" must never
// be the same reply.
bool g_field_ok = true;

bool field_eval_ok(uint root, vec3 point, out float answer) {
    int s_node[FLD_STACK];
    int s_step[FLD_STACK];
    vec3 s_p[FLD_STACK];
    vec3 s_acc[FLD_STACK];

    answer = 0.0;
    g_field_ok = false;
    int top = 1;
    s_node[0] = int(root);
    s_step[0] = 0;
    s_p[0] = point;
    s_acc[0] = vec3(0.0);

    float ret = 0.0;    // what the child that just finished came back with

    for (int guard = 0; guard < FLD_STEPS; ++guard) {
        if (top <= 0) break;
        int fi = top - 1;
        int node = s_node[fi];
        int step = s_step[fi];
        vec3 p = s_p[fi];
        vec3 acc = s_acc[fi];

        int base = node * FLD_TEXELS;
        uvec4 w0 = fld_word(base);
        uvec4 w1 = fld_word(base + 1);
        uvec4 w2 = fld_word(base + 2);
        uvec4 w3 = fld_word(base + 3);
        int op = int(w0.x);
        int nch = int(w0.y);
        ivec4 kids = ivec4(int(w0.z), int(w0.w), int(w1.x), int(w1.y));
        vec4 A = vec4(uintBitsToFloat(w1.z), uintBitsToFloat(w1.w),
                      uintBitsToFloat(w2.x), uintBitsToFloat(w2.y));
        vec4 B = vec4(uintBitsToFloat(w2.z), uintBitsToFloat(w2.w),
                      uintBitsToFloat(w3.x), uintBitsToFloat(w3.y));

        // The two ways out of a frame, and the only two. A case that forgets both cannot silently
        // hand back the previous node's answer — it spins, and the guard catches a spin.
        int pushNode = -1;
        vec3 pushAt = p;
        float done = 0.0;
        bool finished = false;

        if (op <= FLD_LEAF_MAX) {
            done = fld_leaf(op, A, B, p);
            finished = true;
        } else if (op == FLD_TRANSLATE) {
            if (step == 0) { s_step[fi] = 1; pushNode = kids.x; pushAt = p - A.xyz; }
            else { done = ret; finished = true; }
        } else if (op == FLD_ROTATE) {
            if (step == 0) {
                // Applied backwards, because moving the shape one way is asking about the point the
                // other. Euler xyz, in turns, because a quarter is a rounder thing to type than 90.
                float cx = cos(-A.x * FLD_TAU), sx = sin(-A.x * FLD_TAU);
                float cy = cos(-A.y * FLD_TAU), sy = sin(-A.y * FLD_TAU);
                float cz = cos(-A.z * FLD_TAU), sz = sin(-A.z * FLD_TAU);
                vec3 q = p;
                q = vec3(q.x, q.y * cx - q.z * sx, q.y * sx + q.z * cx);
                q = vec3(q.x * cy + q.z * sy, q.y, -q.x * sy + q.z * cy);
                q = vec3(q.x * cz - q.y * sz, q.x * sz + q.y * cz, q.z);
                s_step[fi] = 1; pushNode = kids.x; pushAt = q;
            } else { done = ret; finished = true; }
        } else if (op == FLD_SCALE) {
            vec3 s = vec3(A.x != 0.0 ? A.x : 1.0, A.y != 0.0 ? A.y : 1.0, A.z != 0.0 ? A.z : 1.0);
            if (step == 0) { s_step[fi] = 1; pushNode = kids.x; pushAt = p / s; }
            else {
                // Scaled back by the SMALLEST factor on the way out, so the result never overstates
                // the distance however unevenly the shape is stretched. This is the half of the op
                // a transliteration is most likely to drop.
                done = ret * min(abs(s.x), min(abs(s.y), abs(s.z)));
                finished = true;
            }
        } else if (op == FLD_MIRROR) {
            if (step == 0) {
                int axis = int(A.x);
                s_step[fi] = 1; pushNode = kids.x;
                pushAt = fld_set(p, axis, abs(fld_axis(p, axis)));
            } else { done = ret; finished = true; }
        } else if (op == FLD_TWIST || op == FLD_BEND) {
            if (step == 0) {
                int axis = int(A.y);
                int u, v;
                fld_other(axis, u, v);
                float along = (op == FLD_TWIST) ? fld_axis(p, axis) : fld_axis(p, u);
                float angle = -A.x * FLD_TAU * along;
                float c = cos(angle), sn = sin(angle);
                float x = fld_axis(p, u), y = fld_axis(p, v);
                vec3 q = p;
                q = fld_set(q, u, x * c - y * sn);
                q = fld_set(q, v, x * sn + y * c);
                s_step[fi] = 1; pushNode = kids.x; pushAt = q;
            } else { done = ret; finished = true; }
        } else if (op == FLD_POLAR_REPEAT) {
            if (step == 0) {
                int count = max(1, int(A.x));
                int axis = int(A.y);
                int u, v;
                fld_other(axis, u, v);
                float x = fld_axis(p, u), y = fld_axis(p, v);
                float angle = atan(y, x);
                if (!fld_partial(A.w)) {
                    float sector = FLD_TAU / float(count);
                    angle -= sector * fld_round(angle / sector);
                } else {
                    angle -= FLD_TAU * fld_polar_copy(angle / FLD_TAU, A.z, A.w, count);
                }
                float r = length(vec2(x, y));
                vec3 q = p;
                q = fld_set(q, u, cos(angle) * r);
                q = fld_set(q, v, sin(angle) * r);
                s_step[fi] = 1; pushNode = kids.x; pushAt = q;
            } else { done = ret; finished = true; }
        } else if (op == FLD_REVOLVE) {
            // The point folded into the profile's half plane: how far from the axis, how far along
            // it, and nothing else. That fold IS what revolving means, and it is why the answer is
            // a real distance — the nearest point of a surface of revolution is always at the
            // asking point's own angle.
            int axis = int(A.w);
            int u, v;
            fld_other(axis, u, v);
            vec3 q = p - A.xyz;
            float r = length(vec2(fld_axis(q, u), fld_axis(q, v)));
            float from = B.x, span = B.y;
            if (!fld_partial(span)) {
                if (step == 0) {
                    vec3 inplane = vec3(0.0);
                    inplane = fld_set(inplane, u, r);
                    inplane = fld_set(inplane, axis, fld_axis(q, axis));
                    s_step[fi] = 1; pushNode = kids.x; pushAt = inplane;
                } else { done = ret; finished = true; }
            } else {
                // Part of the way round. Outside the wedge the honest distance is to the END CAP,
                // not to the surface of the full revolution — return the latter and every normal
                // near the cut is wrong while a slice through the shape still looks perfect.
                float rel = fld_wrap_turn(atan(fld_axis(q, v), fld_axis(q, u)) / FLD_TAU - from);
                // Which cap this step is measuring, in turns away from the point.
                float delta = (step == 1) ? fld_nearer_end(rel, span)
                            : ((step == 3) ? rel : rel - span);
                float leg = r * sin(delta * FLD_TAU);
                if (step == 0) {
                    if (rel > span) {
                        s_step[fi] = 1;
                        vec3 inplane = vec3(0.0);
                        inplane = fld_set(inplane, u, r * cos(fld_nearer_end(rel, span) * FLD_TAU));
                        inplane = fld_set(inplane, axis, fld_axis(q, axis));
                        pushNode = kids.x; pushAt = inplane;
                    } else {
                        s_step[fi] = 2;
                        vec3 inplane = vec3(0.0);
                        inplane = fld_set(inplane, u, r);
                        inplane = fld_set(inplane, axis, fld_axis(q, axis));
                        pushNode = kids.x; pushAt = inplane;
                    }
                } else if (step == 1) {
                    // Outside the wedge: one ask, at the nearer cap. The distance to a flat region
                    // is the hypotenuse of the distance IN its plane and the leg out of it.
                    done = length(vec2(max(ret, 0.0), leg));
                    finished = true;
                } else if (step == 2) {
                    // Inside the wedge and outside the profile: the profile's own answer is already
                    // the true distance, because the nearest matter is at the point's own angle.
                    if (ret >= 0.0) { done = ret; finished = true; }
                    else {
                        s_acc[fi] = vec3(ret, 0.0, 0.0);
                        s_step[fi] = 3;
                        vec3 inplane = vec3(0.0);
                        inplane = fld_set(inplane, u, r * cos(rel * FLD_TAU));
                        inplane = fld_set(inplane, axis, fld_axis(q, axis));
                        pushNode = kids.x; pushAt = inplane;
                    }
                } else if (step == 3) {
                    s_acc[fi] = vec3(acc.x, length(vec2(max(ret, 0.0), leg)), 0.0);
                    s_step[fi] = 4;
                    vec3 inplane = vec3(0.0);
                    inplane = fld_set(inplane, u, r * cos((rel - span) * FLD_TAU));
                    inplane = fld_set(inplane, axis, fld_axis(q, axis));
                    pushNode = kids.x; pushAt = inplane;
                } else {
                    // Inside the solid the caps are surface too, and either may be nearer than the
                    // swept face. Magnitude is what normals are made of, so this is not optional.
                    done = max(acc.x, -min(acc.y, length(vec2(max(ret, 0.0), leg))));
                    finished = true;
                }
            }
        } else if (op == FLD_CURVATURE) {
            // The mean of the field around a point against the field at it — the Laplacian, which
            // for a distance field is curvature: positive on an arris, negative in a hollow.
            float r = (A.x > 0.0) ? A.x : 0.05;
            if (step == 1) acc.y = ret;          // the centre
            else if (step > 1) acc.x += ret;     // the six around it
            if (step == 7) { done = (acc.x / 6.0 - acc.y) / r; finished = true; }
            else {
                vec3 where = p;
                if (step == 1) where.x += r;
                else if (step == 2) where.x -= r;
                else if (step == 3) where.y += r;
                else if (step == 4) where.y -= r;
                else if (step == 5) where.z += r;
                else if (step == 6) where.z -= r;
                s_acc[fi] = acc;
                s_step[fi] = step + 1;
                pushNode = kids.x; pushAt = where;
            }
        } else if (op == FLD_OCCLUSION) {
            // How much of a small sphere around this point is solid: 0 out in the open, 1 buried.
            float r = (A.x > 0.0) ? A.x : 0.15;
            if (step > 0 && ret < 0.0) acc.x += 1.0;
            if (step == 14) { done = acc.x / 14.0; finished = true; }
            else {
                s_acc[fi] = acc;
                s_step[fi] = step + 1;
                pushNode = kids.x; pushAt = p + fld_occlusion_dir(step) * r;
            }
        } else if (op == FLD_FACING) {
            // The child's surface normal along one axis, by central differences: up-facing
            // collects, down-facing stays dry and takes soot.
            float h = (A.y > 0.0) ? A.y : 0.02;
            if (step > 0) {
                int which = (step - 1) / 2;
                float signed_ = ((step % 2) == 1) ? ret : -ret;
                if (which == 0) acc.x += signed_;
                else if (which == 1) acc.y += signed_;
                else acc.z += signed_;
            }
            if (step == 6) {
                vec3 n = (length(acc) > 0.0) ? normalize(acc) : vec3(0.0, 1.0, 0.0);
                done = fld_axis(n, int(A.x));
                finished = true;
            } else {
                vec3 where = p;
                float amount = ((step % 2) == 0) ? h : -h;
                if (step / 2 == 0) where.x += amount;
                else if (step / 2 == 1) where.y += amount;
                else where.z += amount;
                s_acc[fi] = acc;
                s_step[fi] = step + 1;
                pushNode = kids.x; pushAt = where;
            }
        } else if (op == FLD_SHELL || op == FLD_ROUND || op == FLD_OFFSET || op == FLD_NEGATE ||
                   op == FLD_ABS || op == FLD_STEP || op == FLD_SMOOTHSTEP || op == FLD_CLAMP ||
                   op == FLD_POWER || op == FLD_REMAP) {
            if (step == 0) { s_step[fi] = 1; pushNode = kids.x; pushAt = p; }
            else { done = fld_after(op, A, ret); finished = true; }
        } else if (op == FLD_DISPLACE || op == FLD_BLEND) {
            if (step == 0) { s_step[fi] = 1; pushNode = kids.x; pushAt = p; }
            else if (step == 1) {
                if (nch < 2) { done = ret; finished = true; }
                else { s_acc[fi] = vec3(ret, 0.0, 0.0); s_step[fi] = 2; pushNode = kids.y; pushAt = p; }
            } else {
                // blend clamps its mix, which is what eval does and what mirror_eval does
                // NOT — the two disagree in the C++ for a t outside [0, 1]. eval is the
                // authority: it is the one the sampler builds voxels with.
                float t = clamp(A.x, 0.0, 1.0);
                done = (op == FLD_DISPLACE) ? (acc.x + A.x * ret)
                                            : (acc.x * (1.0 - t) + ret * t);
                finished = true;
            }
        } else if (op == FLD_REPEAT) {
            vec3 folded;
            ivec3 axes;
            vec3 lean;
            int neighbours;
            fld_repeat_fold(p, A, B, folded, axes, lean, neighbours);
            if (step > 0) acc.x = (step == 1) ? ret : min(acc.x, ret);
            if (step >= (1 << neighbours)) { done = acc.x; finished = true; }
            else {
                // Every combination of leaning neighbours, because with two axes repeating it is
                // the diagonal copy that can be nearest. Step 0 is the fold itself, mask 0.
                vec3 shifted = folded;
                if ((step & 1) != 0) shifted = fld_set(shifted, axes.x, lean.x);
                if ((step & 2) != 0) shifted = fld_set(shifted, axes.y, lean.y);
                if ((step & 4) != 0) shifted = fld_set(shifted, axes.z, lean.z);
                s_acc[fi] = acc;
                s_step[fi] = step + 1;
                pushNode = kids.x; pushAt = shifted;
            }
        } else if (op <= FLD_SMOOTH_DIFFERENCE || op == FLD_ADD || op == FLD_MULTIPLY ||
                   op == FLD_MIN || op == FLD_MAX) {
            // Every child at the same point, folded together.
            if (step > 0) {
                acc.x = (step == 1) ? ret : fld_fold(op, A, acc.x, ret);
                // multiply stops at the first factor that is nought — a mask exists to be zero
                // nearly everywhere and the factors it multiplies are usually the expensive ones.
                if (op == FLD_MULTIPLY && acc.x == 0.0) { done = 0.0; finished = true; }
            }
            if (!finished) {
                if (step >= nch) { done = acc.x; finished = true; }
                else {
                    int child = (step == 0) ? kids.x : ((step == 1) ? kids.y
                              : ((step == 2) ? kids.z : kids.w));
                    s_acc[fi] = acc;
                    s_step[fi] = step + 1;
                    pushNode = child; pushAt = p;
                }
            }
        } else {
            // An op this file has not learned — today that is only PARAMETER, whose value lives in
            // a table the graph does not carry. Never a number.
            return false;
        }

        if (finished) {
            ret = done;
            --top;
        } else {
            if (top >= FLD_STACK) return false;   // deeper than this shader was compiled for
            s_node[top] = pushNode;
            s_step[top] = 0;
            s_p[top] = pushAt;
            s_acc[top] = vec3(0.0);
            ++top;
        }
    }

    if (top > 0) return false;   // the guard ran out, which is not an answer either
    answer = ret;
    g_field_ok = true;
    return true;
}

// The plain form. It answers FLD_FAR when it could not, and a caller that cares reads g_field_ok
// rather than trying to tell a refusal from an answer by its value.
float field_eval(uint node, vec3 p) {
    float v;
    return field_eval_ok(node, p, v) ? v : FLD_FAR;
}
// ================= end field_eval ===============================================================
`;
}
