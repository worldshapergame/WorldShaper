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
    resolution: Math.min(window.devicePixelRatio || 1, 2),
    frameMs: 16,
    lastFrame: 0,
    fps: 0,
    fpsCount: 0,
    fpsSince: 0,
    loading: false,
};

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
    const width = Math.max(1, Math.round(canvas.clientWidth * scale));
    const height = Math.max(1, Math.round(canvas.clientHeight * scale));
    if (canvas.width !== width || canvas.height !== height) {
        canvas.width = width;
        canvas.height = height;
    }
}

function frame(now) {
    requestAnimationFrame(frame);
    const dt = state.lastFrame ? (now - state.lastFrame) / 1000 : 0;
    state.lastFrame = now;

    state.controls.plane = slicePlane();
    state.controls.update(state.clip, dt);
    resize();
    if (state.clip) state.renderer.render(state.controls, slicePlane());

    // Resolution follows the frame time. A phone that cannot hold sixty at its own pixel ratio
    // gets fewer pixels rather than fewer frames, because a viewer you are steering has to answer
    // the thumb and a slightly softer image is not something anybody notices while it is moving.
    state.frameMs += ((dt * 1000) - state.frameMs) * 0.05;
    const ceiling = Math.min(window.devicePixelRatio || 1, 2);
    if (state.frameMs > 22 && state.resolution > 0.55) {
        state.resolution = Math.max(0.55, state.resolution - 0.02);
    } else if (state.frameMs < 13 && state.resolution < ceiling) {
        state.resolution = Math.min(ceiling, state.resolution + 0.01);
    }

    state.fpsCount += 1;
    if (now - state.fpsSince > 500) {
        state.fps = Math.round(state.fpsCount * 1000 / (now - state.fpsSince));
        state.fpsSince = now;
        state.fpsCount = 0;
        $('fps').textContent = state.fps + ' fps · ' + Math.round(state.resolution * 100) + '%';
    }
}

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

    setupInput();

    $('pick').onclick = () => {
        $('list').classList.toggle('hidden');
        if (!$('list').classList.contains('hidden')) buildList();
    };
    $('listClose').onclick = () => $('list').classList.add('hidden');
    $('filter').oninput = buildList;
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

main();
