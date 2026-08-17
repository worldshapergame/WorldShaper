// What the frame costs, and what to switch off when it costs too much.
//
// Two things live here and they are two halves of one job. The first is an INSTRUMENT: a real
// per-pass frame breakdown, taken from `EXT_disjoint_timer_query_webgl2` where the browser has it
// and from the clock where it does not. The second is a LADDER: an ordered list of things to stop
// doing, walked down when the frame is over budget and back up when it is not.
//
// # Why an instrument at all
//
// The page already had one lever — drop to 55% of the pixels when the frame goes over 22 ms — and
// one number to pull it with, an exponential average of the wall clock. That is enough to know
// THAT a frame is slow and nothing at all about WHY, so every judgement about what to make cheaper
// was a guess. This file is the thing that makes the answer a measurement: `budget.passes()` says
// how many milliseconds went into the sky, the opaque mesh, the stencil parity pass, the cap, the
// glass, the bloom chain and the composite, separately.
//
// # The three timing sources, and they are NOT interchangeable
//
//   gpu    EXT_disjoint_timer_query_webgl2. What the card actually spent. One query may be open at
//          a time, so passes are timed in sequence and never nested, and the answers are read back
//          several frames later — asking for one in the frame that issued it is a stall, which is
//          the classic way a profiler becomes the thing it is measuring.
//   cpu    performance.now() around the same brackets. This is SUBMISSION time, not GPU time: on a
//          real driver it says how long the JavaScript took to describe the pass, which is a
//          genuine number about draw-call overhead and is NOT the pass's cost.
//   sync   performance.now() with a gl.finish() at the end of every pass. It serialises the whole
//          pipeline and therefore changes what it measures, and on a software rasteriser — where
//          the "card" is the same four cores the page is on — it is nonetheless the honest per-pass
//          cost, because there is no parallelism for the flush to destroy. It is off by default and
//          exists for taking a baseline.
//
// Whichever is in use is reported with the numbers, because a breakdown whose source is not stated
// is a set of milliseconds that could mean any of three things.
//
// # The ladder
//
// Levels, not knobs. A phone should land on a setting that RUNS rather than on one that stutters,
// and the way to get there is to have decided the order in advance: the cheapest-looking thing
// goes first and the picture's own subject goes last. Each level names what it stops doing, and
// `web/js/features/post.js` and `web/js/gl.js` read the flags rather than the level number, so
// adding a level is a line in one table.
//
// Resolution is INSIDE a level rather than beside it. Two controllers pulling on frame time
// oscillate against each other; this one cascades. Scale moves continuously between the level's
// own floor and ceiling, and only when it is pinned at the floor and still over does the level
// drop — and only when it is pinned at the ceiling with room to spare does the level rise.

// 22 ms is the page's own rule and it is the ceiling here too: over it, something gets cheaper.
export const CEILING_MS = 22;
// And the floor, under which the frame has room to buy something back. Well below 16.7 so that a
// scene sitting exactly on sixty does not walk up and down the ladder for ever.
export const FLOOR_MS = 13;

// Fall in half a second, climb after three. Asymmetric on purpose: a frame that is too slow is
// visible immediately and a frame that is too fast is not visible at all, so there is no reason to
// be slow about the first and every reason to be careful about the second.
const FALL_AFTER_MS = 500;
const RISE_AFTER_MS = 3000;

// The ladder itself. Level 0 is everything; the flags say what is still on.
//
// The order is by what it costs against what it is worth to look at:
//   1  the bloom chain gets shorter and starts smaller — a flame still glows, less widely
//   2  the trims on the shading lobe: clearcoat, sheen, the brushed-metal anisotropy. Each is a
//      handful of ALU on every opaque pixel and each is a detail of one material rather than the
//      shape of the building
//   3  FXAA and the sky-reflection lobe. The reflection is two pow()s and a normalize per pixel
//      and it is what makes bronze read as metal, so it goes late
//   4  bloom entirely, the cap's voxel lattice, and the shapes march gets shorter
//   5  the light grid stops being filtered and the translucency term goes. This is the level a
//      phone that cannot do this at all lands on, and it still draws the clip
export const LADDER = [
    {
        name: 'everything',
        scale: [0.85, 1.00],
        flags: { bloom: true, bloomMips: 6, bloomBase: 2, fxaa: true, fog: true,
                 skyReflection: true, coat: true, brushed: true, capLattice: true,
                 lightFilter: true, translucency: true, shapeSteps: 96 },
    },
    {
        name: 'high',
        scale: [0.75, 0.90],
        flags: { bloom: true, bloomMips: 5, bloomBase: 4, fxaa: true, fog: true,
                 skyReflection: true, coat: true, brushed: true, capLattice: true,
                 lightFilter: true, translucency: true, shapeSteps: 96 },
    },
    {
        name: 'balanced',
        scale: [0.65, 0.80],
        flags: { bloom: true, bloomMips: 4, bloomBase: 4, fxaa: true, fog: true,
                 skyReflection: true, coat: false, brushed: false, capLattice: true,
                 lightFilter: true, translucency: true, shapeSteps: 80 },
    },
    {
        name: 'lean',
        scale: [0.55, 0.70],
        flags: { bloom: true, bloomMips: 3, bloomBase: 4, fxaa: false, fog: true,
                 skyReflection: false, coat: false, brushed: false, capLattice: true,
                 lightFilter: true, translucency: true, shapeSteps: 64 },
    },
    {
        name: 'low',
        // The first rung that runs the LEAN BUILD of the surface and cap programs rather than the
        // full one with its lobes branched around — see the note beside ws_has in web/js/gl.js.
        // Every optional lobe is off here, which is what lets the compiler delete them.
        scale: [0.45, 0.60],
        flags: { bloom: false, bloomMips: 0, bloomBase: 4, fxaa: false, fog: true,
                 skyReflection: false, coat: false, brushed: false, capLattice: false,
                 lightFilter: true, translucency: false, shapeSteps: 48 },
    },
    {
        name: 'minimum',
        scale: [0.40, 0.50],
        flags: { bloom: false, bloomMips: 0, bloomBase: 4, fxaa: false, fog: true,
                 skyReflection: false, coat: false, brushed: false, capLattice: false,
                 lightFilter: false, translucency: false, shapeSteps: 32 },
    },
];

// Fog is on at every level. It is an exp and a mix — five instructions against the sky's two
// pow()s — and it is the difference between air and a vacuum, so there is nothing to buy by
// turning it off and a picture to lose.

export class Budget {
    constructor(gl) {
        this.gl = gl;
        this.ext = gl ? gl.getExtension('EXT_disjoint_timer_query_webgl2') : null;
        this.mode = this.ext ? 'gpu' : 'cpu';
        this.wanted = this.mode;

        // One in six frames carries queries. A breakdown does not need to be taken every frame and
        // a query per pass every frame is itself a cost — six is often enough that the readout
        // moves with the scene and rare enough to disappear into it.
        this.cadence = 6;
        this.frameIndex = 0;

        this.open = null;               // { name, cpuStart, query }
        this.sampling = false;          // is THIS frame carrying queries
        this.slots = [];                // queries issued, waiting to be read
        this.current = [];              // the passes of the frame being built
        this.last = [];                 // the last complete breakdown
        this.lastSource = this.mode;
        this.disjoint = 0;              // how many readbacks the driver threw away

        this.frameMs = 16.7;            // smoothed wall clock
        this.cpuMs = 0;                 // the JavaScript half of the frame
        this.gpuMs = 0;                 // the sum of the breakdown
        this.history = [];              // the last 40 wall-clock frames, for a median
        this.frameStart = 0;
        this.lastRaf = 0;
    }

    get available() { return !!this.ext; }

    // 'gpu', 'cpu' or 'sync'. Asking for one the browser cannot give falls back and says so.
    setMode(mode) {
        this.wanted = mode;
        this.mode = (mode === 'gpu' && !this.ext) ? 'cpu' : mode;
        this.slots.length = 0;
        return this.mode;
    }

    beginFrame(now) {
        this.frameIndex += 1;
        const wall = this.lastRaf ? now - this.lastRaf : 16.7;
        this.lastRaf = now;
        // A frame over a second is a tab coming back or a clip being decoded, not a frame.
        //
        // This guard was a FIFTH of a second first, and that was wrong in the one direction a
        // guard must never be wrong in: on a machine slow enough to need the ladder, every frame
        // is over the guard, so nothing is ever recorded, the frame time sits at its initial value
        // for ever and the quality never drops. The thing that rejects a hitch is the median
        // below — this only has to reject the pathological.
        if (wall > 0 && wall < 5000) {
            this.history.push(wall);
            if (this.history.length > 40) this.history.shift();
            this.frameMs += (wall - this.frameMs) * 0.1;
        }
        this.frameStart = now;
        this.current = [];
        // Every frame on the clock, one in six on the card. A timer query per pass every frame is
        // itself a cost and a queue to drain; a performance.now() is neither, and on a scene slow
        // enough to matter a cadence of six can mean no sample at all inside a settle window.
        // ...and on a slow scene, every frame the queue has room for. A cadence of six is six
        // frames, and six frames of a clip that takes a third of a second each is two seconds
        // before the readout says anything — on exactly the scene somebody is watching it for.
        this.sampling = this.mode !== 'gpu'
            ? true
            : (this.slots.length < 4 &&
               ((this.frameIndex % this.cadence) === 0 || this.frameMs > 50));
        this.readback();
    }

    // Open a pass. Whatever was open is closed first: one GPU query at a time is the extension's
    // rule, and a nested breakdown would double-count anyway.
    begin(name) {
        if (this.open) this.end();
        if (!this.sampling) return;
        const gl = this.gl;
        let query = null;
        if (this.mode === 'gpu') {
            query = gl.createQuery();
            gl.beginQuery(this.ext.TIME_ELAPSED_EXT, query);
        }
        this.open = { name, query, cpuStart: performance.now() };
    }

    end() {
        if (!this.open) return;
        const gl = this.gl;
        const pass = this.open;
        this.open = null;
        if (this.mode === 'gpu') {
            gl.endQuery(this.ext.TIME_ELAPSED_EXT);
            this.current.push({ name: pass.name, query: pass.query, ms: 0 });
            return;
        }
        // The flush belongs INSIDE the bracket: it is what makes the number the pass's own cost
        // rather than the cost of describing it. It is also why this mode is not the default.
        //
        // AND gl.finish() ALONE IS NOT A FLUSH HERE. In a browser the WebGL calls are commands in
        // a buffer that another process drains, and finish() was measured returning in a tenth of
        // a millisecond while the frame it was inside took a hundred — it queues a fence and comes
        // back. A one-pixel readPixels cannot: the answer has to travel back, so the call does not
        // return until the pass that produced the pixel has actually happened. The format is asked
        // for rather than assumed, because the target may be RGBA8 or RGBA16F depending on whether
        // features/post.js is running.
        if (this.mode === 'sync') {
            gl.finish();
            this.drain();
        }
        this.current.push({ name: pass.name, ms: performance.now() - pass.cpuStart });
    }

    // One pixel, read back, which is the cheapest thing that cannot be deferred.
    drain() {
        const gl = this.gl;
        // Asked once a frame and not once a pass: a getParameter is itself a round trip in a
        // browser, so asking three times a frame doubles what the probe costs.
        if (this.readFormat === undefined || this.readFrame !== this.frameIndex) {
            this.readFormat = gl.getParameter(gl.IMPLEMENTATION_COLOR_READ_FORMAT);
            this.readType = gl.getParameter(gl.IMPLEMENTATION_COLOR_READ_TYPE);
            this.readFrame = this.frameIndex;
        }
        const format = this.readFormat;
        const type = this.readType;
        const wide = type !== gl.UNSIGNED_BYTE;
        if (!this.pixel || (this.pixelWide !== wide)) {
            this.pixel = wide ? new Float32Array(4) : new Uint8Array(4);
            this.pixelWide = wide;
        }
        try {
            gl.readPixels(0, 0, 1, 1, format, type, this.pixel);
        } catch (error) {
            // A target this browser will not read back from. The mode is still useful — finish()
            // is at least a flush — and saying so beats throwing inside a profiler.
            this.mode = 'cpu';
        }
    }

    endFrame() {
        if (this.open) this.end();
        this.cpuMs = performance.now() - this.frameStart;
        if (!this.sampling) return;
        if (this.mode === 'gpu') {
            if (this.current.length) this.slots.push({ passes: this.current });
        } else {
            this.finish(this.current);
        }
    }

    // Queries issued a few frames ago, read only once every one of them has an answer. Asking
    // early is a stall; asking about a disjoint interval is asking about a number the driver has
    // already said is wrong.
    readback() {
        if (this.mode !== 'gpu' || !this.slots.length) return;
        const gl = this.gl;
        const disjoint = gl.getParameter(this.ext.GPU_DISJOINT_EXT);
        if (disjoint) {
            this.disjoint += 1;
            for (const slot of this.slots) {
                for (const pass of slot.passes) gl.deleteQuery(pass.query);
            }
            this.slots.length = 0;
            return;
        }
        const slot = this.slots[0];
        for (const pass of slot.passes) {
            if (!gl.getQueryParameter(pass.query, gl.QUERY_RESULT_AVAILABLE)) return;
        }
        this.slots.shift();
        for (const pass of slot.passes) {
            pass.ms = gl.getQueryParameter(pass.query, gl.QUERY_RESULT) / 1e6;
            gl.deleteQuery(pass.query);
        }
        this.finish(slot.passes);
    }

    finish(passes) {
        this.last = passes.map((p) => ({ name: p.name, ms: p.ms }));
        this.lastSource = this.mode;
        this.gpuMs = this.last.reduce((sum, p) => sum + p.ms, 0);
    }

    // The median of the last forty frames rather than the average of all of them. One hitch — a
    // clip swapping in, a collection — is not a regression and must not cost a quality level, and
    // a median is the cheapest thing that ignores one.
    median() {
        if (this.history.length < 4) return this.frameMs;
        const sorted = this.history.slice().sort((a, b) => a - b);
        return sorted[sorted.length >> 1];
    }

    passes() { return this.last; }

    // One line, for the readout. Named passes in the order they ran, biggest first would hide the
    // shape of the frame so they stay in order.
    summary() {
        if (!this.last.length) return '';
        const parts = this.last.map((p) => p.name + ' ' + p.ms.toFixed(1));
        return this.lastSource + '  ' + parts.join(' · ') + '  = ' + this.gpuMs.toFixed(1) + ' ms';
    }
}

export class Quality {
    constructor(ceiling) {
        this.level = 0;
        this.manual = null;             // a level the user pinned, or null for automatic
        this.ceiling = ceiling || 1;    // the device pixel ratio cap the page allows
        this.scale = LADDER[0].scale[1];
        this.since = 0;                 // when the current verdict started
        this.verdict = 0;               // -1 wants cheaper, +1 wants richer, 0 content
        this.changedAt = 0;
    }

    get name() { return LADDER[this.level].name; }
    get flags() { return LADDER[this.level].flags; }
    // What the canvas should actually be, in device pixels per CSS pixel.
    get pixelScale() { return this.scale * this.ceiling; }

    setCeiling(ceiling) { this.ceiling = ceiling; }

    // Pin a level, or pass null to hand it back to the frame time.
    setManual(level) {
        this.manual = level;
        if (level !== null) {
            this.level = Math.max(0, Math.min(LADDER.length - 1, level));
            this.scale = LADDER[this.level].scale[1];
        }
    }

    // `frameMs` is a median, not one window. Returns true when the level changed, which is the
    // only event anything else needs to hear about — the scale moves every frame by design.
    update(frameMs, now) {
        if (this.manual !== null) return false;
        const rung = LADDER[this.level];
        const [floor, roof] = rung.scale;

        // The fine lever first. Over the ceiling, render fewer pixels; under the floor, more.
        let pinned = 0;
        if (frameMs > CEILING_MS) {
            const next = Math.max(floor, this.scale - 0.02);
            if (next === this.scale) pinned = -1;
            this.scale = next;
        } else if (frameMs < FLOOR_MS) {
            const next = Math.min(roof, this.scale + 0.01);
            if (next === this.scale) pinned = +1;
            this.scale = next;
        }

        // ...and the coarse one only when the fine one has run out of travel.
        if (pinned !== this.verdict) { this.verdict = pinned; this.since = now; }
        if (pinned === 0) return false;
        const held = now - this.since;
        if (pinned < 0 && held > FALL_AFTER_MS && this.level < LADDER.length - 1) {
            this.level += 1;
            this.scale = LADDER[this.level].scale[1];
            this.since = now;
            this.changedAt = now;
            return true;
        }
        if (pinned > 0 && held > RISE_AFTER_MS && this.level > 0) {
            this.level -= 1;
            // Back on at the new level's FLOOR rather than its ceiling, so climbing a rung does
            // not immediately hand back everything the rung below had just paid for.
            this.scale = LADDER[this.level].scale[0];
            this.since = now;
            this.changedAt = now;
            return true;
        }
        return false;
    }
}
