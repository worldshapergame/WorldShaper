// What painting the raw view costs, what to give up when it costs too much, and how to say so.
//
// The ◉ view marches the clip as it was written. Colouring it means evaluating the paint stack at
// the hit point — a walk of a field graph per rule, per pixel — and `tools/paintcheck.sh` counts
// what that is:
//
//     clips/sampler.clip            4 rules      238 node evaluations a pixel
//     a facility fragment         348 rules    thousands
//
// A hundred-plus rules each walking a field graph, at every pixel, on a phone, is not a thing that
// happens by being written carefully. So this file is the part that decides how much of it to do,
// and the part that makes the answer visible when it decides to do less.
//
// # Every number in here is SwiftShader on a shared box
//
// There is no phone in this loop and there is no GPU in this loop. The timings this module reads
// come from a software rasteriser sharing four cores with whatever else is running, so the ABSOLUTE
// milliseconds mean nothing at all and are never to be quoted as a phone figure. What survives the
// move to real hardware is the RATIO — twice the rules costs about twice the time, and the shape of
// that curve is a property of the work rather than of the machine. Everything below is written to
// use ratios and to measure the machine it is actually on rather than to carry a constant from one.
//
// # The cuts, in the order they are given up
//
// 1. **The per-rule box.** A rule written `weather ... on=terr_damp` can only fire inside a shape,
//    and a shape has a bounding box, so a pixel outside it skips the rule without evaluating
//    anything. This is the cut that matters and it is nearly free: `paintcheck` measures how many
//    rules survive it at a real surface point, and on a placed stack that is a small fraction of
//    the whole. It costs nothing in fidelity — a rule the box rejects could not have fired.
//
//    **The box must be the SHIFTED one.** `apply_origin` moves a rule's `test` and leaves its
//    `place` where it was, so a clip with an `origin` statement has every placed rule's box out of
//    position by the shift. The facility shifts by 3.50 m. A cull against that box is a coat that
//    silently never fires — see the PLACE CHECK section of `tools/paintcheck.cpp`.
//
// 2. **Back to front, stopping at the first match.** Last match wins, so walking the stack
//    backwards the first rule that fires IS the answer and everything before it is dead work. Exact
//    — it changes no pixel — and it turns "always the whole stack" into "usually the last coat that
//    covers this place". It only pays after the box cull, because a rule the box rejected costs
//    nothing to skip either way.
//
// 3. **A rule cap.** Past `maxRules` survivors the walk stops and takes what it has. This is the
//    first cut that can be WRONG: it drops the earliest rules, which are the undercoats, so a pixel
//    that hits the cap can come out the colour of a later coat with no ground under it.
//
// 4. **A distance fallback.** Beyond `far` metres the stack is not walked at all and the shape is
//    the flat grey it has always been. A building seen from across the site is a silhouette, and a
//    silhouette does not need to know where its moss is.
//
// 5. **Flat grey, everywhere.** The view the ◉ button drew before any of this existed. It is a
//    perfectly good picture and it is better than four frames a second.
//
// # The fallback says so
//
// A view that quietly stops doing something is a view nobody can trust, and this project has been
// caught by exactly that (a silent truncation "reads as it worked"). So whenever the policy is
// below full fidelity, `describe()` returns a line for the page to show, and the distance fallback
// is VISIBLE by construction: near geometry is coloured, far geometry is grey, and the boundary is
// a thing you can see and walk towards.

// The frame budget the viewer already scales its resolution against (documentation/24 §4). Painting
// is given the same one, so the two cuts compete on equal terms rather than one starving the other.
export const FRAME_BUDGET_MS = 22;

// The ladder, coarsest last. `far` is metres from the eye; `Infinity` means "everywhere".
export const LADDER = [
    { name: 'full',     far: Infinity, maxRules: 4096, note: '' },
    { name: 'near',     far: 48,       maxRules: 4096, note: 'paint beyond 48 m: flat' },
    { name: 'nearer',   far: 24,       maxRules: 256,  note: 'paint beyond 24 m: flat, 256 rules' },
    { name: 'close',    far: 12,       maxRules: 64,   note: 'paint beyond 12 m: flat, 64 rules' },
    { name: 'grey',     far: 0,        maxRules: 0,    note: 'paint off: too expensive here' },
];

// What one pixel of this clip costs, in the units paintcheck counts.
//
// Read out of the baked file's own chunks rather than taken as an argument, so the page cannot be
// looking at one clip and costing another. `PANT` is 4 + 52 a rule and `FLDG` is 4 + 80 a node; the
// stride is derived from the chunk size for the same reason paintcheck derives it — a format
// revision should say so rather than be walked off the end of.
export function perPixelWork(clip) {
    const rules = clip && clip.paintRules ? clip.paintRules.length : 0;
    const nodes = clip && clip.fieldNodes ? clip.fieldNodes.length : 0;
    // A rule's own graph is not knowable without walking it, and the walk belongs in the shader, not
    // here. What IS knowable is the shape of the stack: rules times the average graph depth, which
    // over a whole field is nodes/rules when every node belongs to some rule and an over-estimate
    // otherwise. Over-estimating is the safe direction: it degrades early rather than late.
    return { rules, nodes, perPixel: rules > 0 ? nodes : 0 };
}

// Pick a rung. `msPerFrame` is what the last few frames actually cost, measured on THIS machine —
// never a constant, because the constant would be a SwiftShader number pretending to be a phone.
//
// Hysteresis, deliberately: it drops a rung at the budget and only climbs back at 70% of it. Without
// that, a scene sitting exactly on the budget flickers between two rungs every frame, and a view
// whose fidelity blinks is worse than one that is simply coarse.
export function chooseRung(msPerFrame, current, budgetMs = FRAME_BUDGET_MS) {
    let index = Math.max(0, Math.min(LADDER.length - 1, current | 0));
    if (msPerFrame > budgetMs && index < LADDER.length - 1) index += 1;
    else if (msPerFrame < budgetMs * 0.7 && index > 0) index -= 1;
    return index;
}

// The line the page shows. Empty at full fidelity — a view doing everything asked of it has nothing
// to announce, and a permanent badge saying "full" trains people to stop reading it.
export function describe(rungIndex, work) {
    const rung = LADDER[Math.max(0, Math.min(LADDER.length - 1, rungIndex | 0))];
    if (!rung.note) return '';
    const scale = work && work.rules ? ` (${work.rules} paint rules)` : '';
    return rung.note + scale;
}

// The uniforms the shape shader wants for a rung. Kept here rather than in gl.js so the policy is
// one thing in one place: gl.js applies what this returns and decides nothing.
export function uniformsFor(rungIndex) {
    const rung = LADDER[Math.max(0, Math.min(LADDER.length - 1, rungIndex | 0))];
    return {
        u_paintFar: rung.far === Infinity ? 1e9 : rung.far,
        u_paintMaxRules: rung.maxRules,
    };
}

// ------------------------------------------------------------------------------------------
// Measuring, rather than guessing
// ------------------------------------------------------------------------------------------
//
// Two arms of one flag, the same scene, the same camera, in one call — which is the thing CLAUDE.md
// asks for and the thing a benchmark written as two separate runs cannot give you, because the
// machine is shared and the two runs are not the same machine.
//
// `apply` is handed the rung to set; `frame` draws once and returns nothing. The caller owns both,
// so this works against the real renderer and against a stub.
export async function compare(apply, frame, { warmup = 20, frames = 60 } = {}) {
    const arm = async (rung) => {
        apply(rung);
        for (let i = 0; i < warmup; ++i) frame();
        // A median rather than a mean: one scheduling hiccup on a shared box moves a mean by
        // milliseconds and a median by nothing, and this project's rule is that one window is not a
        // measurement.
        const samples = [];
        for (let i = 0; i < frames; ++i) {
            const began = performance.now();
            frame();
            samples.push(performance.now() - began);
        }
        samples.sort((a, b) => a - b);
        return {
            median: samples[samples.length >> 1],
            p5: samples[Math.floor(samples.length * 0.05)],
            p95: samples[Math.floor(samples.length * 0.95)],
        };
    };
    const off = await arm(LADDER.length - 1);   // the flat grey the view had before any of this
    const on = await arm(0);                    // every rule, everywhere
    return {
        off,
        on,
        // The number that survives the move to real hardware. Absolute milliseconds do not.
        ratio: off.median > 0 ? on.median / off.median : 0,
        note: 'SwiftShader on a shared box — the ratio is meaningful, the milliseconds are not',
    };
}
