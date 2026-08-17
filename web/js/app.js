// The viewer: what is on the page, what it loads, and how it knows to reload.
//
// The one behaviour worth reading the code for is the last of those. This site exists to watch
// clips change while somebody else is changing them, so it re-reads `data/index.json` every few
// seconds and compares the hash of the clip on screen with the hash in the index. When they differ
// it fetches the new one and swaps it in with the camera exactly where it was — so a fragment that
// has just been rebuilt appears from the same angle you were looking at the old one from, which is
// the only way to see what actually changed.

import { parseClip } from './format.js';
import { Renderer } from './gl.js';
import { Controls } from './controls.js';
import * as format from './format.js';

// >>> post
import { Quality, LADDER } from './features/budget.js';
// <<< post
// >>> paintcheck
// How much of the paint stack the ◉ view may walk. `paintcost.js` owns the ladder and this file
// picks the rung on it — from the clip, once, at load. See the note beside `paintRungFor` for why
// it is picked from the clip's own work and not from a frame time, and it is applied from here
// rather than from gl.js because gl.js has no view of a clip beyond the one on screen.
import * as paintcost from './features/paintcost.js';
// <<< paintcheck

// What the page was asked for on the way in. Three of these exist only so that a measurement can
// have two arms and a name:
//
//   ?post=0        no HDR target, no bloom, no composite — every pass tone maps in its own shader
//                  exactly as it did before features/post.js existed. This is the control.
//   ?q=N           pin a rung of the quality ladder instead of letting the frame time pick one
//   ?profile=X     'gpu' (the default where the browser has timer queries), 'cpu' (submission
//                  time only), or 'sync' (a gl.finish() per pass — serialises the pipeline, and
//                  is therefore the only honest per-pass number on a software rasteriser)
//   ?stats=1       show the breakdown on screen without opening the panel
const params = new URLSearchParams(location.search);
if (params.get('post') === '0') window.__wsNoPost = true;
// ?fat=1 keeps the FULL surface program on every rung, so the lean build can be measured against
// the same shader with its lobes merely branched around. That difference is the answer to "does a
// feature nobody switched on still cost something", and it is the reason the flag exists.
if (params.get('fat') === '1') window.__wsNoVariants = true;
// <<< post

window.__format = format;
const $ = (id) => document.getElementById(id);
const aspect = () => canvas.clientWidth / Math.max(1, canvas.clientHeight);

const canvas = $('view');
const state = {
    index: null,
    current: null,      // the entry from index.json
    clip: null,         // the parsed .wsc
    renderer: null,
    controls: new Controls(),
    sliceAxis: 2,
    sliceFlip: true,
    sliceValue: 1,
    shapes: false,
    // >>> paintcheck
    paintRung: 0,          // an index into features/paintcost.js's LADDER
    // <<< paintcheck
    resolution: Math.min(window.devicePixelRatio || 1, 2),
    frameMs: 16,
    lastFrame: 0,
    fps: 0,
    fpsCount: 0,
    fpsSince: 0,
    loading: false,
    // >>> post
    quality: new Quality(Math.min(window.devicePixelRatio || 1, 2)),
    budget: null,          // the renderer's, once it exists
    showStats: params.get('stats') === '1',
    // <<< post
};

// >>> paintcheck
// What one pixel of the ◉ view costs for this clip, out of the clip's own chunks.
//
// `PANT` is a four-byte count and 52 bytes a rule; `FLDG` is a four-byte count and 80 bytes a
// node. Both strides are derived from the chunk rather than assumed, for the same reason
// `paintcheck` derives them: a format revision should say so rather than be walked off the end of.
const PANT_RULE_BYTES = 52;
const FLDG_NODE_BYTES = 80;

function paintWork(clip) {
    const pant = clip && clip.chunk ? clip.chunk('PANT') : null;
    const fldg = clip && clip.chunk ? clip.chunk('FLDG') : null;
    const rules = pant ? Math.max(0, Math.floor((pant.byteLength - 4) / PANT_RULE_BYTES)) : 0;
    const nodes = fldg ? Math.max(0, Math.floor((fldg.byteLength - 4) / FLDG_NODE_BYTES)) : 0;
    // Rules times the average graph depth, which over a whole field is nodes when every node
    // belongs to some rule and an over-estimate otherwise. Over-estimating degrades early rather
    // than late, which is the safe direction for a number that decides whether a tab answers.
    return { rules, nodes, perPixel: rules > 0 ? rules * nodes : 0 };
}

// The rung. The thresholds are the shape of the ladder in features/paintcost.js, in the units
// paintWork counts: `sampler` is 4 x 4 = 16 and paints in full; a facility fragment is
// 348 x 4,829 = 1.68 million and paints nothing but its undercoat, with the page saying so.
function paintRungFor(clip) {
    const work = paintWork(clip).perPixel;
    if (work <= 4096) return 0;          // full
    if (work <= 65536) return 1;         // beyond 48 m: flat
    if (work <= 262144) return 2;        // beyond 24 m: flat, 256 rules
    if (work <= 1048576) return 3;       // beyond 12 m: flat, 64 rules
    return 4;                            // off, and the page says why
}
// <<< paintcheck

// --- loading ---------------------------------------------------------------------------------

function toast(text, sticky) {
    const element = $('toast');
    element.textContent = text;
    element.classList.add('shown');
    clearTimeout(toast.timer);
    if (!sticky) toast.timer = setTimeout(() => element.classList.remove('shown'), 2600);
}

function setProgress(fraction, text) {
    $('loading').classList.toggle('hidden', fraction >= 1);
    $('loadingBar').style.width = Math.round(fraction * 100) + '%';
    $('loadingText').textContent = text;
}

// Gzip if the browser can do it, and the plain file if it cannot. The baked quads are a very
// regular structure and compress about four to one, which on a phone is the difference between a
// building appearing at once and appearing after a wait. Whether the compressed copies are there
// is asked once, by trying one, and then remembered — a bake published without them would
// otherwise cost every clip a 404 before its real request.
const canUnzip = typeof DecompressionStream !== 'undefined';
let gzipPresent = canUnzip ? null : false;

async function fetchClip(entry) {
    const attempts = (gzipPresent === false)
        ? [{ url: 'data/' + entry.id + '.wsc', gzip: false }]
        : [{ url: 'data/' + entry.id + '.wsc.gz', gzip: true },
           { url: 'data/' + entry.id + '.wsc', gzip: false }];

    let lastError = null;
    for (const attempt of attempts) {
        try {
            const response = await fetch(attempt.url + '?h=' + entry.hash);
            if (!response.ok) {
                if (attempt.gzip && response.status === 404) gzipPresent = false;
                throw new Error(response.status + ' on ' + attempt.url);
            }
            if (attempt.gzip) gzipPresent = true;
            const total = Number(response.headers.get('content-length')) || 0;
            let read = 0;
            const stream = new ReadableStream({
                start(controller) {
                    const reader = response.body.getReader();
                    const pump = () => reader.read().then(({ done, value }) => {
                        if (done) { controller.close(); return; }
                        read += value.byteLength;
                        if (total > 0) {
                            setProgress(Math.min(0.98, read / total),
                                        entry.id + '  ' + Math.round(read / 1024) + ' kB');
                        }
                        controller.enqueue(value);
                        pump();
                    }).catch((error) => controller.error(error));
                    pump();
                },
            });
            const body = attempt.gzip
                ? stream.pipeThrough(new DecompressionStream('gzip'))
                : stream;
            const buffer = await new Response(body).arrayBuffer();
            return parseClip(buffer);
        } catch (error) {
            lastError = error;
        }
    }
    throw lastError;
}

async function load(entry, keepCamera) {
    if (state.loading) return;
    state.loading = true;
    setProgress(0, entry.id);
    try {
        const clip = await fetchClip(entry);
        state.clip = clip;
        // >>> paintcheck
        // THE PAINT RUNG, picked from what this clip actually asks for and from nothing else.
        //
        // §9 of 26-viewer-integration.md is why there has to be one: `facility/part_terrace` is
        // 348 rules and 4,829 field nodes, which is 338,060 node evaluations for ONE pixel after
        // the per-rule box reject, and a GPU a thousand times faster than this machine is still
        // 200 ms a frame for that one fragment. Left at full, the ◉ button on the building is a
        // tab that stops answering.
        //
        // It is picked from the CLIP'S OWN WORK and deliberately NOT from the frame time. The
        // only GL in this loop is SwiftShader on a shared box: every frame here is over the 22 ms
        // ceiling whatever is drawn, so a frame-time ladder drops `sampler` -- four rules and four
        // nodes -- to flat grey, which is a SwiftShader millisecond making a decision that belongs
        // to a phone. The rule count and the node count are properties of the clip and mean the
        // same thing on every machine. `paintcost.chooseRung` is still there, still tested, and is
        // what to wire the day there is a real mobile GPU to measure on.
        state.paintRung = paintRungFor(clip);
        state.renderer.paint = paintcost.uniformsFor(state.paintRung);
        {
            const note = paintcost.describe(state.paintRung, paintWork(clip));
            if (note) toast(note, true);
        }
        // <<< paintcheck
        state.current = entry;
        state.renderer.setClip(clip);
        if (!keepCamera) {
            state.controls.frame(clip, aspect());
            if (state.controls.mode === 'walk') state.controls.spawn(clip);
        }
        state.sliceValue = 1;
        $('slice').value = '1000';
        updateSliceLabel();
        describe();
        location.hash = entry.id;
        document.title = entry.id + ' — WorldShaper clips';
    } catch (error) {
        toast('could not load ' + entry.id + ': ' + error.message, true);
        console.error(error);
    } finally {
        setProgress(1, '');
        state.loading = false;
    }
}

// --- the index, and watching it ----------------------------------------------------------------

async function readIndex() {
    // Cache-busted on purpose: GitHub Pages puts ten minutes of cache on everything, and ten
    // minutes is long enough that the site would be showing yesterday's work while telling you it
    // was watching.
    const response = await fetch('data/index.json?t=' + Date.now(), { cache: 'no-store' });
    if (!response.ok) throw new Error('no index (' + response.status + ')');
    return response.json();
}

function buildList() {
    const list = $('listBody');
    const filter = $('filter').value.trim().toLowerCase();
    list.innerHTML = '';
    const groups = new Map();
    for (const entry of state.index.clips) {
        if (filter && !entry.id.includes(filter) && !entry.source.toLowerCase().includes(filter)) {
            continue;
        }
        if (!groups.has(entry.group)) groups.set(entry.group, []);
        groups.get(entry.group).push(entry);
    }
    // The building first, then its parts, then everything else — which is the order somebody
    // looking for a fragment expects, rather than alphabetical across forty files.
    const order = [...groups.keys()].sort((a, b) => {
        if (a === b) return 0;
        if (a === 'clips') return 1;
        if (b === 'clips') return -1;
        return a < b ? -1 : 1;
    });
    for (const group of order) {
        const heading = document.createElement('div');
        heading.className = 'group';
        heading.textContent = group === 'clips' ? 'other clips' : group;
        list.appendChild(heading);
        for (const entry of groups.get(group)) {
            const row = document.createElement('button');
            row.className = 'row' + (state.current && entry.id === state.current.id ? ' on' : '');
            const name = entry.source.replace(/\.clip$/, '').split('/').pop();
            row.innerHTML = '<span class="name"></span><span class="meta"></span>';
            row.querySelector('.name').textContent = name;
            row.querySelector('.meta').textContent =
                entry.quads.toLocaleString() + ' quads · ' + entry.metre + '/m · ' +
                (entry.bytes / (1024 * 1024)).toFixed(1) + ' MB';
            row.onclick = () => {
                $('list').classList.add('hidden');
                load(entry, false);
            };
            list.appendChild(row);
        }
    }
}

function describe() {
    const entry = state.current;
    const clip = state.clip;
    if (!entry || !clip) return;
    $('pick').textContent = entry.source.replace(/\.clip$/, '');
    const rows = [
        ['file', entry.source],
        ['size', (clip.high[0] - clip.low[0]).toFixed(2) + ' × ' +
                 (clip.high[1] - clip.low[1]).toFixed(2) + ' × ' +
                 (clip.high[2] - clip.low[2]).toFixed(2) + ' m'],
        ['voxels', clip.dims.join(' × ') + '  at ' + clip.metre + '/m (authored ' +
                   clip.authoredMetre + '/m)'],
        ['matter', clip.solid.toLocaleString() + ' voxels'],
        ['surface', clip.quads.toLocaleString() + ' quads, ' + clip.transparentQuads.toLocaleString() +
                    ' of them clear'],
        ['materials', String(clip.materialCount)],
        ['shapes', clip.shapeCount
            ? clip.shapeCount.toLocaleString() + ' as written, ' +
              (clip.cutterCount || 0).toLocaleString() + ' cut out of them'
            : 'not baked'],
        ['download', (entry.bytes / (1024 * 1024)).toFixed(2) + ' MB'],
        ['baked', state.index ? state.index.built : ''],
        // Which clips these are. The site follows whichever branch was pushed last, so without
        // this line "the facility" is ambiguous between two very different buildings.
        ['from', state.index && state.index.branch
            ? state.index.branch + (state.index.commit ? ' @ ' + state.index.commit : '')
            : 'unknown'],
    ];
    // Written as elements and filled with textContent rather than as one string of HTML: a clip's
    // own file name reaches this line, and a name is not markup.
    $('panelBody').innerHTML = rows.map(() => '<dt></dt><dd></dd>').join('');
    const dts = $('panelBody').querySelectorAll('dt');
    const dds = $('panelBody').querySelectorAll('dd');
    rows.forEach(([k, v], i) => { dts[i].textContent = k; dds[i].textContent = v; });
}

async function poll() {
    try {
        const next = await readIndex();
        const changed = !state.index || next.hash !== state.index.hash;
        state.index = next;
        if (changed) {
            const wasFrom = state.index ? state.index.branch : null;
            buildList();
            if (wasFrom && next.branch && next.branch !== wasFrom) {
                toast('now showing ' + next.branch);
            }
            if (state.current) {
                const fresh = next.clips.find((c) => c.id === state.current.id);
                if (fresh && fresh.hash !== state.current.hash) {
                    toast(state.current.id + ' changed — reloading');
                    await load(fresh, true);
                } else if (!fresh) {
                    toast(state.current.id + ' is no longer in the index');
                }
            }
        }
    } catch (error) {
        // A failed poll is a network blip, not something to shout about. The next one is in five
        // seconds and the clip on screen is still the clip on screen.
        console.warn(error);
    }
}

// --- the slice ---------------------------------------------------------------------------------

function slicePlane() {
    if (!state.clip || state.sliceValue >= 1) return null;
    const clip = state.clip;
    const axis = state.sliceAxis;
    const low = clip.low[axis] - 0.05;
    const high = clip.high[axis] + 0.05;
    const t = state.sliceValue;
    return {
        axis,
        sign: state.sliceFlip ? -1 : 1,
        at: state.sliceFlip ? high + (low - high) * t : low + (high - low) * t,
    };
}

function updateSliceLabel() {
    $('axis').textContent = 'XYZ'[state.sliceAxis];
    $('flip').classList.toggle('on', state.sliceFlip);
    const plane = slicePlane();
    $('sliceAt').textContent = plane ? plane.at.toFixed(2) + ' m' : 'whole';
    state.controls.plane = plane;
}

// --- input -------------------------------------------------------------------------------------

function setupInput() {
    const controls = state.controls;
    const pointers = new Map();
    let pinch = 0;

    canvas.addEventListener('pointerdown', (event) => {
        canvas.setPointerCapture(event.pointerId);
        pointers.set(event.pointerId, { x: event.clientX, y: event.clientY });
        if (pointers.size === 2) {
            const [a, b] = [...pointers.values()];
            pinch = Math.hypot(a.x - b.x, a.y - b.y);
        }
    });

    canvas.addEventListener('pointermove', (event) => {
        const last = pointers.get(event.pointerId);
        if (!last) return;
        const dx = event.clientX - last.x;
        const dy = event.clientY - last.y;
        last.x = event.clientX;
        last.y = event.clientY;

        if (pointers.size === 2 && controls.mode === 'orbit') {
            const [a, b] = [...pointers.values()];
            const spread = Math.hypot(a.x - b.x, a.y - b.y);
            if (pinch > 0) controls.distance *= pinch / Math.max(1, spread);
            pinch = spread;
            // Two fingers also pan, in the plane the camera is looking through, and the clip goes
            // the way the fingers go. Same screen-right as the walker uses, for the same reason.
            const speed = controls.distance * 0.0008;
            const forward = [Math.sin(controls.yaw), 0, Math.cos(controls.yaw)];
            const right = [-forward[2], 0, forward[0]];
            controls.target[0] -= right[0] * dx * speed;
            controls.target[2] -= right[2] * dx * speed;
            controls.target[1] += dy * speed;
            return;
        }

        // The same sign in both modes, and it was not: orbit had a `+` that tipped the building
        // the opposite way to the finger. Dragging down looks down and tips the top of a clip away
        // from you, which is the one reading that means the same thing in both.
        const sensitivity = 0.005;
        controls.yaw -= dx * sensitivity;
        controls.pitch -= dy * sensitivity;
    });

    const release = (event) => {
        pointers.delete(event.pointerId);
        if (pointers.size < 2) pinch = 0;
    };
    canvas.addEventListener('pointerup', release);
    canvas.addEventListener('pointercancel', release);

    canvas.addEventListener('wheel', (event) => {
        event.preventDefault();
        if (controls.mode === 'orbit') {
            controls.distance *= Math.exp(event.deltaY * 0.0012);
        }
    }, { passive: false });

    // The joystick: a pad in the corner that follows the thumb, and it works anywhere inside its
    // own generous area rather than only on the little circle that is drawn.
    const pad = $('pad');
    const knob = $('knob');
    let padPointer = null;
    let padOrigin = [0, 0];
    pad.addEventListener('pointerdown', (event) => {
        pad.setPointerCapture(event.pointerId);
        padPointer = event.pointerId;
        padOrigin = [event.clientX, event.clientY];
        knob.style.display = 'block';
        knob.style.left = event.clientX + 'px';
        knob.style.top = event.clientY + 'px';
        knob.firstElementChild.style.transform = 'translate(-50%, -50%)';
    });
    pad.addEventListener('pointermove', (event) => {
        if (padPointer !== event.pointerId) return;
        const dx = event.clientX - padOrigin[0];
        const dy = event.clientY - padOrigin[1];
        const reach = 56;
        const length = Math.hypot(dx, dy);
        const scale = length > reach ? reach / length : 1;
        controls.move.x = (dx * scale) / reach;
        controls.move.y = -(dy * scale) / reach;
        knob.firstElementChild.style.transform =
            'translate(calc(-50% + ' + dx * scale + 'px), calc(-50% + ' + dy * scale + 'px))';
    });
    const padUp = (event) => {
        if (padPointer !== event.pointerId) return;
        padPointer = null;
        controls.move.x = 0;
        controls.move.y = 0;
        knob.style.display = 'none';
    };
    pad.addEventListener('pointerup', padUp);
    pad.addEventListener('pointercancel', padUp);

    // Up and down. Tap up to jump; tap it twice to fly, which is the gesture the request asked
    // for and is worth the 320 ms it costs the first tap to be sure of.
    let lastUpTap = 0;
    const up = $('btnUp');
    const down = $('btnDown');
    up.addEventListener('pointerdown', (event) => {
        event.preventDefault();
        up.setPointerCapture(event.pointerId);
        controls.up = true;
        const now = performance.now();
        if (now - lastUpTap < 320) {
            controls.flying = !controls.flying;
            toast(controls.flying ? 'flying — no clipping' : 'walking');
            lastUpTap = 0;
        } else {
            lastUpTap = now;
            if (!controls.flying) controls.jumpQueued = true;
        }
        setFlyLook();
    });
    const upOff = () => { controls.up = false; };
    up.addEventListener('pointerup', upOff);
    up.addEventListener('pointercancel', upOff);
    up.addEventListener('pointerleave', upOff);

    down.addEventListener('pointerdown', (event) => {
        event.preventDefault();
        down.setPointerCapture(event.pointerId);
        controls.down = true;
        if (!controls.flying) controls.toggleCrouch(state.clip, true);
    });
    const downOff = () => {
        controls.down = false;
        if (!controls.flying) controls.toggleCrouch(state.clip, false);
    };
    down.addEventListener('pointerup', downOff);
    down.addEventListener('pointercancel', downOff);
    down.addEventListener('pointerleave', downOff);

    window.addEventListener('keydown', (event) => {
        if (event.target.tagName === 'INPUT') return;
        controls.keys.add(event.code);
        if (event.code === 'Space') {
            event.preventDefault();
            controls.up = true;
            if (controls.flying) return;
            controls.jumpQueued = true;
        }
        if (event.code === 'ControlLeft' || event.code === 'KeyC') {
            controls.down = true;
            if (!controls.flying) controls.toggleCrouch(state.clip, true);
        }
        if (event.code === 'KeyF') {
            controls.flying = !controls.flying;
            toast(controls.flying ? 'flying — no clipping' : 'walking');
            setFlyLook();
        }
        if (event.code === 'KeyV') $('mode').click();
        // >>> post
        // G for the frame breakdown, and [ ] to walk the quality ladder by hand — which is the
        // only way to see what a rung actually costs without waiting for a phone to find it.
        if (event.code === 'KeyG') {
            state.showStats = !state.showStats;
            const box = $('stats');
            if (box) box.remove();
            if (state.showStats) updateStats();
        }
        if (event.code === 'BracketRight' || event.code === 'BracketLeft') {
            const step = event.code === 'BracketRight' ? 1 : -1;
            const level = Math.max(0, Math.min(LADDER.length - 1,
                (state.quality.manual === null ? state.quality.level : state.quality.manual) + step));
            state.quality.setManual(level);
            state.renderer.setQuality(state.quality.flags);
            toast('quality pinned: ' + state.quality.name);
        }
        // <<< post
    });
    window.addEventListener('keyup', (event) => {
        controls.keys.delete(event.code);
        if (event.code === 'Space') controls.up = false;
        if (event.code === 'ControlLeft' || event.code === 'KeyC') {
            controls.down = false;
            if (!controls.flying) controls.toggleCrouch(state.clip, false);
        }
    });
}

function setFlyLook() {
    document.body.classList.toggle('flying', state.controls.flying);
    $('btnUp').classList.toggle('on', state.controls.flying);
}

function setMode(mode) {
    state.controls.mode = mode;
    document.body.classList.toggle('walking', mode === 'walk');
    $('mode').textContent = mode === 'walk' ? 'Orbit' : 'Walk';
    if (mode === 'walk' && state.clip) state.controls.spawn(state.clip);
    if (mode === 'orbit' && state.clip) state.controls.frame(state.clip, aspect());
    setFlyLook();
}

// --- the frame ---------------------------------------------------------------------------------

function resize() {
    const scale = state.resolution;
    // >>> post
    // Quantised to eight pixels, and this is not tidiness.
    //
    // The scale creeps by a hundredth every frame while the frame time is under the floor, so an
    // unquantised canvas is a NEW SIZE EVERY FRAME — and every size change now throws away and
    // reallocates the HDR target, the emissive target, the depth-stencil buffer, the eight-bit
    // copy and every mip of the bloom chain. That is a dozen textures a frame to gain four pixels
    // of width. Rounding to a multiple of eight makes the reallocation happen when the resolution
    // has actually moved, which is a few times a second at worst.
    const grain = 8;
    const width = Math.max(1, Math.round(canvas.clientWidth * scale / grain) * grain);
    const height = Math.max(1, Math.round(canvas.clientHeight * scale / grain) * grain);
    // <<< post
    if (canvas.width !== width || canvas.height !== height) {
        canvas.width = width;
        canvas.height = height;
    }
}

function frame(now) {
    requestAnimationFrame(frame);
    const dt = state.lastFrame ? (now - state.lastFrame) / 1000 : 0;
    state.lastFrame = now;

    state.budget.beginFrame(now);   // >>> post <<< post

    state.controls.plane = slicePlane();
    state.controls.update(state.clip, dt);
    resize();
    if (state.clip) state.renderer.render(state.controls, slicePlane(), state.shapes);

    // >>> post
    // Resolution follows the frame time, and now so does everything else.
    //
    // This used to be one lever: over 22 ms, render fewer pixels, down to 55%. That is the crudest
    // of the levers available and it was the only one, so a phone that could not hold the frame at
    // 55% simply stuttered at 55%. It is now the FINE lever inside a rung of a ladder — see
    // features/budget.js for the rungs and for why the two are cascaded rather than run side by
    // side. The 22 ms rule is unchanged and is where it has always been: `CEILING_MS`.
    //
    // The number driving it is a MEDIAN of the last forty frames rather than an exponential
    // average. A clip swapping in, or a collection, is one slow frame and not a regression, and it
    // must not cost a quality level; the average lets it, the median does not.
    state.frameMs = state.budget.frameMs;

    state.quality.setCeiling(Math.min(window.devicePixelRatio || 1, 2));
    if (state.quality.update(state.budget.median(), now)) {
        state.renderer.setQuality(state.quality.flags);
        if (!state.quality.manual) toast('quality: ' + state.quality.name);
    }
    state.resolution = state.quality.pixelScale;
    state.budget.endFrame();
    // <<< post

    state.fpsCount += 1;
    if (now - state.fpsSince > 500) {
        state.fps = Math.round(state.fpsCount * 1000 / (now - state.fpsSince));
        state.fpsSince = now;
        state.fpsCount = 0;
        // >>> post
        $('fps').textContent = state.fps + ' fps · ' + Math.round(state.resolution * 100) + '% · ' +
                               state.quality.name;
        if (state.showStats) updateStats();
        // <<< post
    }
}

// >>> post
// The breakdown, on screen. Nobody else in this viewer is measuring the whole frame, so this is
// the one place that says where the time went — per pass, with the source of the numbers named,
// because a GPU millisecond and a submission millisecond are not the same quantity.
function updateStats() {
    let box = $('stats');
    if (!box) {
        box = document.createElement('div');
        box.id = 'stats';
        box.style.cssText =
            'position:fixed;left:8px;bottom:8px;z-index:40;font:11px/1.5 ui-monospace,monospace;' +
            'background:rgba(10,10,10,0.72);color:#d8d8d8;padding:6px 9px;border-radius:6px;' +
            'pointer-events:none;white-space:pre;max-width:60vw';
        document.body.appendChild(box);
    }
    const budget = state.budget;
    const passes = budget.passes();
    const lines = [
        Math.round(state.fps) + ' fps · ' + budget.frameMs.toFixed(1) + ' ms wall · ' +
        budget.cpuMs.toFixed(1) + ' ms js',
        state.quality.name + ' (' + state.quality.level + '/' + (LADDER.length - 1) + ') · ' +
        Math.round(state.resolution * 100) + '% · ' + canvas.width + '×' + canvas.height,
        state.renderer.stats.draws + ' draws · ' +
        state.renderer.stats.quads.toLocaleString() + ' quads',
    ];
    if (passes.length) {
        lines.push('— ' + budget.lastSource + ' —');
        for (const pass of passes) lines.push(pass.name.padEnd(14) + pass.ms.toFixed(2) + ' ms');
        lines.push('total'.padEnd(14) + budget.gpuMs.toFixed(2) + ' ms');
    } else {
        lines.push('(no breakdown yet)');
    }
    box.textContent = lines.join('\n');
}
// <<< post

// --- start ---------------------------------------------------------------------------------------

async function main() {
    try {
        state.renderer = new Renderer(canvas);
    } catch (error) {
        setProgress(1, '');
        document.body.innerHTML =
            '<div class="fatal">This browser has no WebGL 2, which is what the viewer draws ' +
            'with.<br>' + error.message + '</div>';
        return;
    }

    // >>> post
    state.budget = state.renderer.budget;
    const profile = params.get('profile');
    if (profile) state.budget.setMode(profile);
    const pinned = params.get('q');
    if (pinned !== null) state.quality.setManual(Number(pinned));
    state.renderer.setQuality(state.quality.flags);
    state.resolution = state.quality.pixelScale;
    // <<< post

    setupInput();

    $('pick').onclick = () => {
        $('list').classList.toggle('hidden');
        if (!$('list').classList.contains('hidden')) buildList();
    };
    $('listClose').onclick = () => $('list').classList.add('hidden');
    $('filter').oninput = buildList;
    $('shapes').onclick = () => {
        if (!state.clip || !state.clip.shapeCount) {
            toast('this clip was baked before the shapes view existed');
            return;
        }
        state.shapes = !state.shapes;
        $('shapes').classList.toggle('on', state.shapes);
        // >>> paintcheck
        // ...and what the paint rung gave up, said HERE rather than only at load. A view that has
        // quietly stopped painting is a view nobody can trust, and on a facility fragment the whole
        // answer is flat grey -- so the line saying why has to be the line on screen when the
        // button is pressed, not one the clip's own toast replaced four seconds earlier.
        const note = state.shapes ? paintcost.describe(state.paintRung, paintWork(state.clip)) : '';
        // <<< paintcheck
        toast(state.shapes
            ? state.clip.shapeCount.toLocaleString() + ' shapes, as written — cut as the clip cuts ' +
              'them' + (note ? ' · ' + note : '')
            : 'voxels', !!note);
    };
    $('info').onclick = () => $('panel').classList.toggle('hidden');
    $('panelClose').onclick = () => $('panel').classList.add('hidden');
    $('mode').onclick = () => setMode(state.controls.mode === 'walk' ? 'orbit' : 'walk');
    $('axis').onclick = () => {
        state.sliceAxis = (state.sliceAxis + 1) % 3;
        updateSliceLabel();
    };
    $('flip').onclick = () => {
        state.sliceFlip = !state.sliceFlip;
        updateSliceLabel();
    };
    $('slice').oninput = (event) => {
        state.sliceValue = Number(event.target.value) / 1000;
        updateSliceLabel();
    };

    try {
        state.index = await readIndex();
    } catch (error) {
        setProgress(1, '');
        toast('no baked clips yet: ' + error.message, true);
        return;
    }
    buildList();

    // A pasted link with a clip in it should show that clip, whether the page was open already or
    // not. `load` writes the hash itself, so this only acts on a fragment that is not what is on
    // screen — otherwise selecting a clip would load it twice.
    window.addEventListener('hashchange', () => {
        const asked = location.hash.replace('#', '');
        if (!asked || !state.index || (state.current && state.current.id === asked)) return;
        const found = state.index.clips.find((c) => c.id === asked);
        if (found) load(found, false);
    });

    const wanted = location.hash.replace('#', '');
    const entry = state.index.clips.find((c) => c.id === wanted) ||
                  state.index.clips.find((c) => c.id === 'facility') ||
                  state.index.clips[0];
    if (entry) await load(entry, false);
    updateSliceLabel();

    setInterval(poll, 5000);
    requestAnimationFrame(frame);
}

// Handy from a console, and it is what the browser tests drive.
window.__state = state;
// >>> post: and what the frame-cost harness drives. Both are read from a rAF callback registered
// after this one, so what they report is the frame that has just been drawn.
window.__budget = () => state.budget;
window.__quality = () => state.quality;
// <<< post

main();
