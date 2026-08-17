// The paint stack — which material a point is, evaluated where the ray hit.
//
// The ◉ view marches the clip as it was written, at no resolution at all, and until now every
// shape came back the same flat grey. It had to: a shape has no material. Colour in this project
// is not a property of a shape, it is the result of a **stack of rules** evaluated at a point,
// which is `documentation/20-clip-forge.md` §2 and is the thing that lets one wall be stone,
// except where it is damp, except where the damp has moss, except in the mortar — one shape and
// four rules rather than four shapes.
//
// `src/forge/sample.cpp` runs that stack at the centre of every voxel. This runs the same stack at
// the marched hit point, which is strictly better information: the point is exact and the normal
// is the analytic gradient rather than a six-tap finite difference at voxel spacing.
//
// The rule, and it is worth writing out because getting it backwards is not a crash:
//
//     material = the first coat
//     for each rule in file order:
//         v = field_eval(rule.node, p)
//         if v is within [above, below] and the facing test passes:
//             material = rule.material     <-- LAST match wins, not the first
//
// "Each painting over the last" is the whole design. A first-match reading produces a picture that
// looks entirely plausible and is wrong everywhere two rules overlap, which in the facility is most
// of the building — the weathering coats in `surface.clip` are laid over everything.
//
// ## What the numbers mean
//
// `above` is the LOW end of the accepted band and `below` is the HIGH end, which reads backwards
// until you remember they are written about the field's value: `above=0.55` is "where the grain is
// above 0.55", `below=0.02` is "where the distance is below 2 cm". For a rule keyed on a shape the
// value is a signed distance, so `below=0.02` means "inside that shape, or within 2 cm outside it",
// and `clips/facility/BRIEF.md` rule 5 is three pages on why it is 0.02 and not 0 and why anything
// a transform placed needs 0.035. Both ends are inclusive, matching `paint_solid`.
//
// `facing` is a threshold on one component of the surface normal, and the SIGN of the threshold
// picks the direction of the test: `at=0.6` means "the normal's y is at least 0.6", `at=-0.6`
// means "at most -0.6", i.e. pointing down. That is `sample.cpp`'s own rule and it is easy to get
// wrong by taking an absolute value, which paints ceilings with the floor's moss.
//
// ## The cost, which is the whole problem
//
// A hundred and thirty-three rules on the facility, each of them a walk of a field graph, at every
// pixel, on a phone. Three things cut it, in the order they pay:
//
//  1. **A box per rule.** Most rules cannot possibly apply at a given point — a rule keyed on a
//     shape, accepted below a threshold, can only paint inside that shape's box grown by the
//     threshold. Six floats and a compare, no field walk. The sampler measured the same thing from
//     the other side: five unboxed rules were three quarters of all the paint work in a build.
//  2. **Walk it backwards and stop at the first match.** Last match wins, so the first rule that
//     matches walking from the end IS the answer and everything before it is dead. That turns
//     "always a hundred and thirty-three" into "usually one or two".
//  3. **A budget on the walk.** `PAINT_MAX_EVALS` field evaluations per pixel and then the base
//     coat, so a pathological clip costs a bounded frame rather than a hung tab. It is visible
//     when it bites — see `?paint=cap` — because a silent cap reads as "it worked".
//
// The first coat is never evaluated. It is the fallback either way: `sample.cpp` ends with
// "a cell with matter in it and no rule that matched is still matter", handing back
// `paint.front().type` whether or not the first rule's own test would have passed. So the walk
// stops at rule 1 and returns rule 0's material, which is one field evaluation saved at every
// pixel of every clip.

// ---------------------------------------------------------------------------------------------
// The wire format
// ---------------------------------------------------------------------------------------------

// Bytes 200..207 of the 208-byte header are the chunk directory: `u32 chunkOffset` then
// `u32 chunkCount`, pointing at 16-byte entries of `char fourcc[4]; u32 offset; u32 size;
// u32 reserved`. Shared by every agent working on this format at once, so it is read defensively:
// a file without it is a file baked before any of this existed and must draw the old picture
// rather than throw.
export const CHUNK_DIRECTORY_AT = 200;
export const CHUNK_ENTRY_BYTES = 16;

// PANT: `u32 ruleCount`, then one of these each.
//
// **52 bytes, not 28.** The first 28 are the record as it was first published — node, below, above,
// facingAxis, facingAt, material, flags — and the last 24 are `f32 lo[3]; f32 hi[3]`, the box
// outside which the rule cannot apply, which the published record had nowhere to put. Reading the
// stride as 28 does not fail: it reads rule i out of the middle of an earlier rule and paints a
// building out of numbers that were somebody else's box.
export const RULE_BYTES = 52;
export const RULE_BOX_AT = 28;
// FLDG is the field graph, 80 bytes a node, and it is `web/js/features/field.js`'s to read: the
// boxes this file rejects against come down in the rule itself now, so nothing here opens it.

// Four RGBA32F texels a rule. WebGL2 has no storage buffers, so this is the same trick the cutter
// pool uses. The order is chosen so the common path — a rule whose box the point is nowhere near —
// costs exactly two fetches and stops:
//
//     0: boxLow.xyz, hasBox
//     1: boxHigh.xyz, node
//     2: above (low end), below (high end), facingAxis, facingAt
//     3: material, flags, 0, 0
export const RULE_TEXELS = 4;

// A constant loop bound, because GLSL ES wants one even where the driver would take a dynamic one.
// A facility fragment carries 348 rules. When a clip carries more than this the FIRST coat and the
// LAST 511 are kept, because the end of the stack is the end that wins — truncating the other way
// throws away exactly the rules that decide the picture.
export const PAINT_MAX_RULES = 512;

// The budget, in field evaluations per pixel, after box rejection.
export const PAINT_MAX_EVALS = 32;

// What the exporter says about a rule. Only BOXED matters here, and it matters a great deal: it is
// the promise that `lo`/`hi` are a real box outside which the rule cannot apply. Rejecting on a box
// that was never filled in would paint a whole clip in its first coat, with nothing to see but a
// building that lost its paint, so the box is used only when this bit says it is there.
export const RULE_METRIC = 1;
export const RULE_BOUNDED = 2;
export const RULE_FACING = 4;
export const RULE_BOXED = 8;
export const RULE_PLACED = 16;
export const RULE_COSTLY = 32;
export const RULE_UNDERCOAT = 64;

// `facingAxis` is -1 when the rule does not ask for a normal. The engine's own PaintRule writes 3
// for that, and the exporter normalises it to -1; a shader testing `axis < 3` reads -1 as "yes, ask
// about axis -1" and indexes a normal with it, which is every rule suddenly caring which way a
// surface points. The test here is `axis >= 0`.
export const FACING_NONE = -1;

const INFINITE = 1e29;

function fourcc(view, at) {
    return String.fromCharCode(view.getUint8(at), view.getUint8(at + 1), view.getUint8(at + 2),
                               view.getUint8(at + 3));
}

// Every chunk in the file, by name. `null` when the file has no directory at all, which is what a
// clip baked before version 3 looks like and is not an error.
export function readChunks(buffer) {
    if (!buffer || buffer.byteLength < CHUNK_DIRECTORY_AT + 8) return null;
    const view = new DataView(buffer);
    const at = view.getUint32(CHUNK_DIRECTORY_AT, true);
    const count = view.getUint32(CHUNK_DIRECTORY_AT + 4, true);
    if (at === 0 || count === 0) return null;
    if (at + count * CHUNK_ENTRY_BYTES > buffer.byteLength) return null;
    const chunks = new Map();
    for (let i = 0; i < count; ++i) {
        const entry = at + i * CHUNK_ENTRY_BYTES;
        const name = fourcc(view, entry);
        const offset = view.getUint32(entry + 4, true);
        const size = view.getUint32(entry + 8, true);
        if (offset + size > buffer.byteLength) continue;   // a truncated file says nothing at all
        chunks.set(name, { offset, size });
    }
    return chunks;
}

// The stack, in file order. `node` indexes the field graph, `above`/`below` are the ends of the
// accepted band, `facingAxis` is -1 when the rule does not ask for a normal, and `lo`/`hi` are a
// box that means anything only when `flags` has RULE_BOXED in it.
export function readRules(buffer, chunk) {
    if (!chunk || chunk.size < 4) return [];
    const view = new DataView(buffer, chunk.offset, chunk.size);
    const count = view.getUint32(0, true);
    if (4 + count * RULE_BYTES > chunk.size) return [];
    const rules = [];
    for (let i = 0; i < count; ++i) {
        const at = 4 + i * RULE_BYTES;
        rules.push({
            node: view.getUint32(at + 0, true),
            below: view.getFloat32(at + 4, true),     // the HIGH end of the band
            above: view.getFloat32(at + 8, true),     // the LOW end of the band
            facingAxis: view.getInt32(at + 12, true),
            facingAt: view.getFloat32(at + 16, true),
            material: view.getUint32(at + 20, true),
            flags: view.getUint32(at + 24, true),
            low: [view.getFloat32(at + RULE_BOX_AT, true),
                  view.getFloat32(at + RULE_BOX_AT + 4, true),
                  view.getFloat32(at + RULE_BOX_AT + 8, true)],
            high: [view.getFloat32(at + RULE_BOX_AT + 12, true),
                   view.getFloat32(at + RULE_BOX_AT + 16, true),
                   view.getFloat32(at + RULE_BOX_AT + 20, true)],
        });
    }
    return rules;
}

// Where a rule could possibly say yes, or null when nothing can be said.
//
// The exporter has already done the thinking `plan_sample` does — which rules can be placed at all
// (`where=<shape> below=0.02` accepts a small signed distance, so it accepts only inside that shape
// or just outside it; `above=0.55` on a noise is the complement of a shape and can be true
// anywhere) — and says so with RULE_BOXED. So this trusts the flag and checks the numbers, rather
// than deriving a second opinion that could be tighter than the exporter's and reject a rule that
// should have fired.
export function ruleBox(rule) {
    if (!rule || (rule.flags & RULE_BOXED) === 0 || !rule.low || !rule.high) return null;
    for (let k = 0; k < 3; ++k) {
        if (!(Math.abs(rule.low[k]) < INFINITE) || !(Math.abs(rule.high[k]) < INFINITE)) return null;
        if (!(rule.low[k] <= rule.high[k])) return null;
    }
    return { low: rule.low, high: rule.high };
}

// Everything the shader needs for one clip. Reads the chunks out of whatever ArrayBuffer the clip
// was parsed from — `format.js` keeps typed-array views onto it, so any of them can hand it back,
// and nothing here needs `format.js` changed to reach the new chunks.
export function paintFromClip(clip, options) {
    const mode = (options && options.mode) || 'auto';
    if (mode === 'off') return { rules: [], source: 'off' };
    // A reader that lands after this one may put the chunks on the clip itself. Prefer that.
    if (clip && clip.paintRules && clip.paintRules.length > 0) {
        return { rules: clip.paintRules, source: 'clip' };
    }
    const buffer = bufferOf(clip);
    const chunks = buffer ? readChunks(buffer) : null;
    if (chunks && chunks.has('PANT')) {
        return { rules: readRules(buffer, chunks.get('PANT')), source: 'PANT' };
    }
    if (mode === 'demo') return samplerFixture(clip);
    return { rules: [], source: 'none' };
}

function bufferOf(clip) {
    if (!clip) return null;
    for (const key of ['materials', 'opaque', 'transparent', 'light', 'collision', 'shapes']) {
        const view = clip[key];
        if (view && view.buffer && view.buffer.byteLength > CHUNK_DIRECTORY_AT) return view.buffer;
    }
    return null;
}

// ---------------------------------------------------------------------------------------------
// The stack, in JavaScript
// ---------------------------------------------------------------------------------------------

// The same decision the shader makes, and the reason it exists twice is that this one can be
// tested against hand-worked cases without a GPU. `evalField(node, p)` stands in for the field.
// Returns a material index, or -1 when the clip has no paint at all.
export function decideMaterial(rules, evalField, p, normal) {
    if (!rules || rules.length === 0) return -1;
    // Backwards, because the last match is the answer: the first one found walking from the end is
    // the one every earlier rule would have been painted over by.
    for (let i = rules.length - 1; i >= 1; --i) {
        const rule = rules[i];
        const box = ruleBox(rule);
        if (box && (p[0] < box.low[0] || p[1] < box.low[1] || p[2] < box.low[2] ||
                    p[0] > box.high[0] || p[1] > box.high[1] || p[2] > box.high[2])) {
            continue;
        }
        const v = evalField(rule.node, p);
        // The evaluator refuses past a graph depth of 64, and a refusal is no match — the walk goes
        // on to the rule underneath rather than stopping on a number nobody produced.
        if (v === null || v === undefined || !Number.isFinite(v)) continue;
        if (v < rule.above || v > rule.below) continue;   // above is the low end, below the high
        if (rule.facingAxis >= 0) {
            const c = normal[rule.facingAxis];
            // The sign of the threshold picks the direction of the test. sample.cpp, verbatim.
            if (rule.facingAt >= 0) { if (c < rule.facingAt) continue; }
            else { if (c > rule.facingAt) continue; }
        }
        return rule.material;
    }
    return rules[0].material;   // the first coat, which is what a cell no rule claimed falls back to
}

// ---------------------------------------------------------------------------------------------
// Onto the card
// ---------------------------------------------------------------------------------------------

// Four texels a rule, in the order the shader reads them. Rules past PAINT_MAX_RULES are dropped
// from the MIDDLE of the stack, keeping the first coat and the last 511.
export function packRules(rules) {
    let use = rules;
    if (rules.length > PAINT_MAX_RULES) {
        use = [rules[0]].concat(rules.slice(rules.length - (PAINT_MAX_RULES - 1)));
        console.warn('paint: ' + rules.length + ' rules, the shader holds ' + PAINT_MAX_RULES +
                     ' — the first coat and the last ' + (PAINT_MAX_RULES - 1) + ' are kept');
    }
    const data = new Float32Array(Math.max(1, use.length) * RULE_TEXELS * 4);
    let boxed = 0;
    for (let i = 0; i < use.length; ++i) {
        const rule = use[i];
        const box = ruleBox(rule);
        const at = i * RULE_TEXELS * 4;
        if (box) {
            boxed += 1;
            data[at + 0] = box.low[0];
            data[at + 1] = box.low[1];
            data[at + 2] = box.low[2];
            data[at + 3] = 1;
            data[at + 4] = box.high[0];
            data[at + 5] = box.high[1];
            data[at + 6] = box.high[2];
        }
        data[at + 7] = rule.node;
        data[at + 8] = rule.above;        // the LOW end of the accepted band
        data[at + 9] = rule.below;        // the HIGH end
        data[at + 10] = rule.facingAxis;
        data[at + 11] = rule.facingAt;
        data[at + 12] = rule.material;
        data[at + 13] = rule.flags;
    }
    return { data, count: use.length, boxed };
}

// One RGBA32F texture, `texelFetch` only, exactly like the cutter pool. There is always a texture
// even when there are no rules, because a sampler bound to nothing is undefined behaviour rather
// than a blank hole.
export function uploadRules(gl, texture, rules) {
    const packed = packRules(rules || []);
    const texels = Math.max(1, packed.count * RULE_TEXELS);
    // A multiple of RULE_TEXELS so a rule never straddles a row, which keeps the shader's index
    // arithmetic to one divide and one modulo. 1020 is 4 x 255.
    const width = Math.min(1020, texels);
    const height = Math.ceil(texels / width);
    const padded = new Float32Array(width * height * 4);
    padded.set(packed.data.subarray(0, Math.min(packed.data.length, padded.length)));
    gl.bindTexture(gl.TEXTURE_2D, texture);
    gl.pixelStorei(gl.UNPACK_ALIGNMENT, 4);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA32F, width, height, 0, gl.RGBA, gl.FLOAT, padded);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    return { width, height, count: packed.count, boxed: packed.boxed };
}

// ---------------------------------------------------------------------------------------------
// The control arm
// ---------------------------------------------------------------------------------------------

// `?paint=off` compiles the shapes view with no stack at all — not a stack with nothing in it, no
// stack: no texture fetches, no field walk, no branch. It is the other arm of the measurement and
// it exists because a counter taken from inside the change is not a control.
//
// `?paint=demo`  paints a clip that has no PANT chunk yet from the fixture below.
// `?paint=cap`   paints magenta wherever the walk hit its budget, so "the cap does not bite" is
//                something seen rather than assumed.
// `?paint=cover` paints every shaded fragment magenta, so the pixels the shapes view actually
//                costs anything for can be counted rather than estimated.
// `?paint=evals` writes the field walks this pixel paid for into red and marks it covered in
//                green, which is the one cost figure that does not move with the machine.
export function paintMode() {
    try {
        const value = new URLSearchParams(location.search).get('paint');
        if (value === 'off' || value === 'demo' || value === 'cap' || value === 'cover' ||
            value === 'evals') {
            return value;
        }
    } catch (error) {
        // No location (a test harness, a worker). The default is the right answer there.
    }
    return 'auto';
}

// ---------------------------------------------------------------------------------------------
// GLSL
// ---------------------------------------------------------------------------------------------

// A stand-in for `field_eval` until `web/js/features/field.js` lands, so the stack above is
// complete, compiled and testable before the evaluator it calls exists.
//
// It is deterministic and it is NOT the clip's field: smooth value noise in roughly [-1, 1],
// seeded and scaled by the node index, which is enough to make every rule in a stack fire
// somewhere and nowhere near enough to agree with the voxels. Replace the whole string with
// field.js's own source and nothing else in this file changes.
export const FIELD_EVAL_STUB_GLSL = `
// >>> paintstack: a stub field, to be replaced by web/js/features/field.js
float ws_stub_hash(vec3 c) {
    return fract(sin(dot(c, vec3(127.1, 311.7, 74.7))) * 43758.5453123);
}

float ws_stub_noise(vec3 p) {
    vec3 i = floor(p);
    vec3 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float n000 = ws_stub_hash(i + vec3(0.0, 0.0, 0.0));
    float n100 = ws_stub_hash(i + vec3(1.0, 0.0, 0.0));
    float n010 = ws_stub_hash(i + vec3(0.0, 1.0, 0.0));
    float n110 = ws_stub_hash(i + vec3(1.0, 1.0, 0.0));
    float n001 = ws_stub_hash(i + vec3(0.0, 0.0, 1.0));
    float n101 = ws_stub_hash(i + vec3(1.0, 0.0, 1.0));
    float n011 = ws_stub_hash(i + vec3(0.0, 1.0, 1.0));
    float n111 = ws_stub_hash(i + vec3(1.0, 1.0, 1.0));
    return mix(mix(mix(n000, n100, f.x), mix(n010, n110, f.x), f.y),
               mix(mix(n001, n101, f.x), mix(n011, n111, f.x), f.y), f.z);
}

float field_eval(uint node, vec3 p) {
    float seed = float(node);
    float scale = 1.4 + mod(seed, 4.0) * 1.7;
    return ws_stub_noise(p * scale + seed * 13.0) * 2.0 - 1.0;
}

// The real evaluator's second entry point: false means it refused — it ran out of depth at 64 —
// and the stack must read that as "this rule does not match here" rather than as a value. The stub
// never refuses, but it has to present the same two functions or swapping it out is not one line.
bool field_eval_ok(uint node, vec3 p, out float value) {
    value = field_eval(node, p);
    return true;
}
// <<< paintstack
`;

const PAINT_STACK_GLSL = `
// >>> paintstack: which material is this point, by the clip's own stack of rules
uniform highp sampler2D u_rules;   // ${RULE_TEXELS} RGBA32F texels a rule, texelFetch only
uniform int u_ruleWidth;           // texels across, so an index becomes a row and a column
uniform int u_ruleCount;
uniform int u_paintDebug;          // 1: show where the walk ran out of budget

const int PAINT_MAX_RULES = ${PAINT_MAX_RULES};
const int PAINT_TEXELS = ${RULE_TEXELS};
const int PAINT_MAX_EVALS = ${PAINT_MAX_EVALS};

bool g_paintCapped;
int g_paintEvals;

vec4 rule_texel(int index) {
    return texelFetch(u_rules, ivec2(index % u_ruleWidth, index / u_ruleWidth), 0);
}

// The stack, at the point the ray actually hit, with the analytic normal.
//
// Backwards, and the first match found is the answer — because the stack paints each rule over the
// last, so the last rule that matches is the one on top. Walking it forwards and keeping the last
// match gives the same answer and costs every rule at every pixel; walking it backwards and
// stopping costs one or two.
int paint_material(vec3 p, vec3 n) {
    g_paintCapped = false;
    g_paintEvals = 0;
    if (u_ruleCount <= 0) return -1;

    int start = min(u_ruleCount - 1, PAINT_MAX_RULES - 1);
    for (int k = 0; k < PAINT_MAX_RULES; ++k) {
        int i = start - k;
        // Rule 0 is the first coat and it is never evaluated: it is the fallback whether its own
        // test passes or not, so asking is a field walk that cannot change the answer.
        if (i < 1) break;

        int base = i * PAINT_TEXELS;
        vec4 t0 = rule_texel(base + 0);   // box low, and whether there is a box at all
        vec4 t1 = rule_texel(base + 1);   // box high, and the field node
        // Six floats and a compare. Most rules on a real clip die here, at every pixel, without
        // touching the field.
        if (t0.w > 0.5 && (any(lessThan(p, t0.xyz)) || any(greaterThan(p, t1.xyz)))) continue;

        // The budget. Everything up to here was free; from here a rule costs a field walk.
        if (g_paintEvals >= PAINT_MAX_EVALS) { g_paintCapped = true; break; }

        vec4 t2 = rule_texel(base + 2);   // above (low end), below (high end), facing axis, at
        float v;
        bool answered = field_eval_ok(uint(t1.w + 0.5), p, v);
        g_paintEvals += 1;
        // A refusal — the evaluator ran out of depth at 64 — is NO MATCH, and the walk carries on
        // to the rule underneath. Reading it as a match paints whatever the deepest graph in the
        // clip happens to be over everything below it.
        if (!answered) continue;
        if (v < t2.x || v > t2.y) continue;

        // floor, not a truncating cast: facingAxis is -1 when the rule does not ask for a normal,
        // and int(-1.0 + 0.5) truncates to 0, which is "ask about x" for every rule in the clip.
        int axis = int(floor(t2.z + 0.5));
        if (axis >= 0) {
            float c = (axis == 0) ? n.x : ((axis == 1) ? n.y : n.z);
            // The SIGN of the threshold is the direction of the test. An abs() here paints the
            // ceiling with the floor's moss.
            if (t2.w >= 0.0) {
                if (c < t2.w) continue;
            } else {
                if (c > t2.w) continue;
            }
        }
        return int(rule_texel(base + 3).x + 0.5);
    }
    return int(rule_texel(3).x + 0.5);    // rule 0, texel 3: the first coat
}
// <<< paintstack
`;

// The control arm: the same entry point, no stack behind it.
const PAINT_OFF_GLSL = `
// >>> paintstack: ?paint=off — the stack compiled out, for the other arm of the measurement
uniform highp sampler2D u_rules;
uniform int u_ruleWidth;
uniform int u_ruleCount;
uniform int u_paintDebug;
bool g_paintCapped;
int g_paintEvals;
int paint_material(vec3 p, vec3 n) {
    g_paintCapped = false;
    g_paintEvals = 0;
    return -1;
}
// <<< paintstack
`;

export const PAINT_GLSL = paintMode() === 'off' ? PAINT_OFF_GLSL : PAINT_STACK_GLSL;

// THE SEAM features/shapeshade.js WAS WRITTEN AROUND, and the one §3 of the integration plan says
// the ◉ view does not release without.
//
// shapeshade.js imports this module behind a caught dynamic import and asks for exactly this
// string. Without it, it falls back to a stub that hashes each shape's own placement into the
// material table -- so every shape gets A material rather than ITS material, and the sampler's
// wall panel is green because it is a hash and not because it is moss. "The colours are wrong"
// and "the colours are a hash" look identical in a screenshot, which is why the console says
// which of the two is live on every load.
//
// It is three lines because everything it needs is already above it: `PAINT_GLSL` is spliced into
// the shapes fragment before `SHAPE_SHADING_GLSL` is, so `paint_material` is declared by the time
// this calls it. -1 means the clip carries no stack at all, and material 0 -- the undercoat, the
// first rule any clip writes -- is a truer answer to that than any hash.
export const MATERIAL_AT_GLSL = `
uint material_at(vec3 p, vec3 n) {
    return uint(max(paint_material(p, n), 0));
}
`;

// ---------------------------------------------------------------------------------------------
// A fixture, for `?paint=demo`, until the exporter lands
// ---------------------------------------------------------------------------------------------

// `clips/sampler.clip`'s own stack, written out by hand in the wire format's own terms. It is the
// real stack — the real thresholds, the real facing test, the real material indices, in the real
// order — against a stubbed field, so what it demonstrates is the STACK and not the picture.
//
// It is reached only by `?paint=demo` and only for a clip that has no PANT chunk, so it can never
// quietly stand in for a real one. Delete it once the exporter ships PANT.
//
//     paint stone                                          <- the first coat
//     paint pale  where=grain_fine  above=0.15
//     paint moss  where=grain_ridge above=0.4  facing=y at=0.55
//     paint metal where=masonry     above=0.0
//
// The material indices are looked up by COLOUR and not assumed. The baker interns its palette by
// what a material looks like, in the order the mesher meets one, so the sampler comes out
// stone, metal, moss, pale — not the order the clip declares them in, and a fixture that assumed
// declaration order would paint the moss rule's material onto metal and read as a bug in the stack.
function samplerFixture(clip) {
    const count = clip ? clip.materialCount : 0;
    if (count < 4 || !clip.materials) return { rules: [], source: 'none' };
    const find = (r, g, b) => {
        for (let i = 0; i < count; ++i) {
            const at = i * 16;
            if (clip.materials[at] === r && clip.materials[at + 1] === g &&
                clip.materials[at + 2] === b) {
                return i;
            }
        }
        return -1;
    };
    const stone = find(124, 120, 112);
    const pale = find(196, 192, 182);
    const metal = find(170, 172, 178);
    const moss = find(64, 112, 54);
    if (stone < 0 || pale < 0 || metal < 0 || moss < 0) {
        return { rules: [], source: 'none' };   // not the sampler
    }
    const rules = [
        { node: 0, above: -1e30, below: 1e30, facingAxis: FACING_NONE, facingAt: 0.5, material: stone, flags: RULE_UNDERCOAT },
        { node: 1, above: 0.15, below: 1e30, facingAxis: FACING_NONE, facingAt: 0.5, material: pale, flags: 0 },
        { node: 2, above: 0.4, below: 1e30, facingAxis: 1, facingAt: 0.55, material: moss, flags: RULE_FACING },
        { node: 3, above: 0.0, below: 1e30, facingAxis: FACING_NONE, facingAt: 0.5, material: metal, flags: 0 },
    ];
    return { rules, source: 'fixture' };
}
