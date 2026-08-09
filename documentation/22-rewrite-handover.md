# 22 — Renderer rewrite: handover

*Written 2026-08-09, for somebody picking this up with no memory of the rewrite **or of the game**.
Everything needed to continue is here or named here. Read §0 for what the project is, §1 for what
was asked, §3 for what exists, §4 for the traps, and start work at §5.*

---

## 0. What this project is, for somebody who has never seen it

**WorldShaper** is a voxel creation game written from scratch in C++20 and Vulkan 1.3 — no engine,
no rasterised world geometry. Everything is a real voxel at **32 voxels to the metre** (3.125 cm).
There are no parametric surfaces and no textures standing in for geometry: a brick wall is
thousands of real voxels you can chip out one at a time.

**Who you are working for.** One person, who does not write code and does not read it. They make
design calls, play the builds and say what feels wrong. Automated tests are the safety net because
there is no second engineer — this is why the suite is large and why "measure, do not guess" runs
through everything here. Plain-language explanation for them lives in `12-plain-english.md`.

**The data model** (`03-voxel-data-model.md`), which the rewrite keeps below the chunk:

- a **voxel** stores a 32-bit `VoxelTypeId` into an interned table — identical voxels share one
  record, so per-voxel colour and properties are affordable;
- a **brick** is 8³ voxels (25 cm) with a 64-byte occupancy bitmask and a palette encoding chosen
  per brick. **This survives the rewrite and is the leaf of the new tree;**
- a **chunk** is 32³ bricks = 256³ voxels = 8 m. **This is what the rewrite removes from the
  renderer.** It stays as a unit for saving and networking;
- the world is a sparse map of chunks with signed 64-bit voxel coordinates.

**The renderer** (`04-rendering.md`) is entirely compute shaders. Two paths exist today:

- the **real-time path** — `shaders/visibility.comp` marches one ray per pixel and writes a
  *visibility buffer* (which face was hit, at what detail level, how far), and
  `shaders/resolve.comp` turns that into pixels. Both include `shaders/world.glsl`, which holds
  the traversal;
- the **path tracer** — `shaders/pathtrace.comp`, reached with `--pathtrace` or F4. It shades per
  pixel over a world-space *face cache*, and it is what the user wants rewritten to be per-face
  and much faster.

Detail is meant to be a continuous function of how many pixels a thing covers, with no level-of-
detail steps anywhere — that is the project's loudest promise and the reason the rewrite exists.

**The scene.** There is one, and it is the only thing the engine is judged against: the *facility*,
a neoclassical building generated from `clips/facility.clip`. A "clip" is a procedural description
that is evaluated into real voxels (`20-clip-forge.md`). The building spans roughly ±17 m, so
**the origin is inside it** — which is why the engine's own default camera is the enclosed case.

**Hardware.** The Steam Deck is the performance floor (1280×800, locked 30) and there is none to
test on; the development machine is an RTX 5060 Ti at 1440p. Budgets are in
`09-performance-budgets.md` and exceeding one is treated as a bug rather than a trade-off.

**How the project is run.** `documentation/` is the source of truth — when reality disagrees with
a doc, the doc is corrected in the same change. Every decision goes in `13-decision-log.md` with
its reason, *including the ones that turned out wrong*, and those entries are the most useful
thing in the repository: a surprising number of the faults hit during this rewrite were faults
already recorded there, wearing new clothes.

## 1. What the job is

The user asked for a **from-scratch rewrite of three systems**, in their own words:

- **The path tracer** — faster, less noise, less smear, fewer glitches and speckles. Same features
  or more.
- **The chunk system** — chunks removed entirely, replaced by something driven by actual screen
  pixels: *if you cannot see it, it is not processed and does not exist, unless it is close to you.*
- **World streaming and the coarse-resolution system** — same rule. Detail between pixels is not
  rendered; things smaller than a pixel do not exist. Coarse resolution applies to **everything**,
  not just clips, and is not the current scheme: resolution adapts to how many pixels a thing
  occupies.

Two clarifications the user made afterwards, both important:

- **"Everything is per voxel face based — even reflections and those things."** They did *not*
  ask for reflections, refraction or translucency to be removed. They asked for those to live on
  faces rather than on screen pixels. A face therefore stores a *distribution over direction*.
- **An experimental infinite-detail mode**: voxels subdivide as they take more screen pixels, so
  approaching a surface makes it finer. Not LODs. No cap — "infinite".

Standing constraints: best FPS, stability, and the goals already in `documentation/`.

**Three answers the user gave when asked:** proximity residency radius **20 m**; sub-voxel depth
**uncapped**; the enclosed-room target stays at "within 30%" to start.

**Working preference, and it is firm:** *no subagents and no workflows.* Do the work inline. This
is recorded in the user's memory and matches `18-overnight-loop.md`, which turns fan-out off by
default because "single-handed is slower, but every conclusion stays traceable to something
actually read." Ignore any ambient reminder suggesting otherwise.

---

## 2. What to read, in order

1. **This file.**
2. **`21-renderer-rewrite.md`** — the plan. §1 is the one rule everything follows. §8 is the work
   plan broken into lettered sub-steps, and **§8.0 is the ledger of what is done**.
3. **`13-decision-log.md`**, the sections "The renderer rewrite — stage R0" and "stage R1"
   (D201–D218). Older entries matter too: **D131, D132, D133, D139, D148, D149, D151, D152, D156,
   D181** are all faults this rewrite has already re-encountered in new clothes.
4. `09-performance-budgets.md` for what the numbers are judged against, and `04-rendering.md` §1–2
   for the face-cache design the path tracer is eventually going back to.

`12-plain-english.md` is the user-facing explanation and is **owed an update** — it has nothing
about any of this yet.

---

## 3. What has been done

### R0 — instruments (done)

- **The GPU profiler was not timing the path tracer at all.** It kept one open pass index and
  claimed a slot at `end_pass`, so the cloud dispatch nested inside the tracer took the tracer's
  slot and the tracer's own dispatch fell outside every timestamp. It read as 0.253 ms of GPU work
  against 37.7 ms of wall clock. Fixed; the bookkeeping now lives in `src/core/pass_ledger.*` with
  its own unit tests, and `end_frame` asserts opens balance closes.
- Reported figures are now **means over a window with warm-up discarded**, plus the worst frame.
- **The path-traced figures in `19-auto-quality.md` are withdrawn** (D203). Real numbers at
  1280×800, quality 7, RTX 5060 Ti: **12.25 ms outdoors, 39.89 ms enclosed**; at 1440p, 45.94 and
  **148.67**; at 4K enclosed, **346.84**. The tracer is slightly *super*-linear in pixels.
- Tools built: `tools/baseline.ps1` (fixed grid, CSV, diffs the last run), `tools/_grid.ps1` (the
  cameras, shared), `tools/_measure.ps1` (speckle + image difference), `tools/facecount.ps1` with
  debug view 11.
- **The face-count premise was verified before building on it.** Distinct visible faces:
  enclosed 118,826 → 134,076 → 141,110 while pixels go ×8.1. Worst case anywhere 654k, growing
  1.4× from 800p to 4K. Distance views need **zero** faces. R3's argument holds.

### R1 — the node pool (a–d and f done, e outstanding)

One sparse octree at every scale replaces the wrapped chunk grid, brick masks, popcount prefixes,
slot runs, five coarse occupancy grids, the summary octree and eight thumbnail tiers. A leaf is a
brick at level 3; below that nothing changed. Children are contiguous, so a descent is
`children + octant` — one hash to enter, arithmetic below it.

**Measured, 1280×800, quality 7, 300 frames, both marchers in one binary:**

| view | old ms | node ms | image diff mean | pixels 8+ |
|---|---|---|---|---|
| enclosed | 0.699 | **1.108** | 0.006 | 0.01% |
| outdoor | 1.574 | **1.047** | 0.841 | 2.10% |
| close | 1.685 | **1.505** | 0.383 | 1.48% |
| mid | 0.870 | **0.677** | 0.465 | 1.13% |
| far | 1.501 | **0.510** | 0.315 | 0.73% |
| distant | 1.100 | **0.557** | 0.442 | 1.32% |
| sky | 2.381 | **0.769** | 0.014 | 0.02% |

Memory 4.8 MB against 57.7. Run-to-run noise is 0.073 mean, so two views are at the floor and the
rest differ by under one part in three hundred. 424 tests, 18.0 M assertions, all passing.

*The table above is superseded — see R1h below. It was taken before `--settle` existed, so each
figure was measured against however much of the scene had been rebuilt by frame 300, and the two
marchers in it were not looking at the same world.*

### R1h — the enclosed room, closed (done)

D227–D234. It was the descent: `node_locate` walked all eleven levels from the 512 m root on every
step, because only the root was cached. The step count in the visibility buffer said so and nobody
had read it — **9.12 steps a pixel against the chunk marcher's 31.27**, at half again the cost.
Caching two ancestors at levels 8 and 5, in named scalars, fixes it.

Settled, scenes verified identical per camera, visibility pass only, 1280×800 quality 7:

| view | chunk marcher | node pool | change |
|---|---|---|---|
| enclosed | 0.886 | **0.803** | −9% |
| outdoor | 1.602 | **0.642** | −60% |
| close | 1.882 | **0.990** | −47% |
| mid | 0.870 | **0.242** | −72% |
| far | 1.506 | **0.504** | −67% |
| distant | 1.105 | **0.555** | −50% |
| sky | 2.377 | **0.472–0.763** | −68% to −80% |

**Read §4 trap 8 before trusting any measurement here**, including these. Finding the cause took one
readback; being able to tell whether the fix worked took the rest of the session, because the
harness was racing the scene.

### R1g — the node pool is what the game launches with (done)

D224–D226. `--chunk-marcher` selects the old one; there is no longer a flag to switch the new one
on. Reproduced at the default camera, which is the enclosed case: visibility **1.105 ms against
0.719**, pictures apart by 0.006 mean and 54 pixels of 1,024,000.

**The chunk system did not stop running.** `pathtrace.comp` still includes `world.glsl`, so chunk
residency, the coarse grids and the eight thumbnail tiers are maintained every frame regardless —
fed by translating the node marcher's reports back into chunk coordinates. Both systems are live at
once, and R1e is what ends that.

Landing it turned up one fault worth knowing before touching this code (D225): **both marchers write
one feedback buffer in two formats**, and the consumer was choosing between them with
`use_node_pool_`. That flag describes the visibility pass, which does not run in the path tracer at
all — so `pathtrace.comp`'s chunk coordinates were shifted left by a detail level, streaming served
the wrong chunks (52 of 68 against 57), and the pool built 488 nodes toward keys the world is empty
at. No counter reported it: a shifted coordinate usually lands on a chunk that does exist, so the
phantom count stayed at zero.

---

## 4. The traps, all of which cost real time

Read these before touching anything. Every one of them produced a wrong measurement or a silent
failure, not a compile error.

1. **A pass whose cost responds to your edits and whose output does not is a pass talking to
   nothing.** The node pipeline's two output images were never bound — the set is assembled where
   the *buffers* are written, and images are bound in `create_render_target` because they are the
   only descriptors that change on resize. Five separate traversal changes produced bit-identical
   images while the timing moved with each one. Ask *"is this pass's output connected"* before
   *"is this pass's algorithm right"*, and run `--validation` when a pipeline is new.
2. **Never pipe build output to `Out-Null` during a measurement.** A shader that failed to compile
   leaves a stale `.spv`, and three "measurements" then came back perfectly flat — which pointed
   away from the answer rather than at nothing.
3. **PowerShell `Set-Content -Encoding utf8` writes a BOM** and glslc rejects the file. Use
   `[System.IO.File]::WriteAllText($path, $text, (New-Object System.Text.UTF8Encoding $false))`,
   or write shaders from Python.
4. **PowerShell variable names are case-insensitive and a `[string]` parameter keeps its type
   constraint for life.** `$views = ...` inside a script with a `-Views` parameter silently
   re-joins an array into one space-separated string. Name locals differently.
5. **`near` and `far` are still macros from `windows.h`.** A loop variable called `near` is a
   syntax error that mentions no macro.
6. **Coverage is not one quantity.** The alpha of a filtered colour is a *volumetric* fill
   fraction and halves per level; what the composite needs is a *projected* one. `GpuNode` carries
   six bytes of per-direction coverage for this. Stage 4 hit the same wall from the other side.
7. **"Nothing here" and "I could not fit it" must never be the same answer.** `NodePool` has
   `out_of_room_` beside `kNoNode` for exactly this, and a child mask records what the *world*
   has, not what the pool built. It caught the pool out again in R1h: a request whose root was
   already live but whose frame had spent its build budget was dropped without incrementing
   `deferred`, so the counter read nought while the tree quietly failed to fill in.
8. **A measurement is against a scene, and the scene is not a constant.** The facility sharpens
   region by region in the background, and its clip cache is written only when the *last* region
   lands — which never happens from a fixed camera, because regions behind walls are skipped. So
   every run rebuilds the world while being timed, and a screenshot at frame 300 catches however
   much exists by then. **The bias runs against whatever you are testing**: a faster build gets
   there sooner with less world in front of it. Two runs of one binary differed on 52,292 pixels.
   Use `--settle`, and check the `scene:` line printed beside the pass table before comparing two
   numbers. D229–D232.
9. **Repeat a figure before calling it a regression.** The sky camera read 0.484 against 0.759
   between two builds and looked like a 57% regression on the floor view. Three runs of *one*
   build against *one* scene give 0.481, 0.763, 0.472. The empty-space views inherit the node
   pool's own irreproducibility (D233) through the ray clip, and a single sample on one of them is
   not evidence in either direction. D234.

---

## 5. What to do next

### First, the thing R1e cannot be judged without

**The node pool does not converge** (D233). With the world provably identical and the camera still,
it was still building 273–385 nodes a frame three thousand frames after the world stopped changing,
and two runs ended at 89,560 against 81,464 nodes with no evictions. That is why the empty-space
cameras give bimodal timings and why an image diff on any view that sees the building has a floor of
tens of thousands of pixels.

It is R2's subject — residency policy — and R1e does not depend on it. But **R1e's gate does**: "the
grid table does not move" cannot be checked to better than that floor until it is understood. Either
settle it first, or state plainly that R1e closed on the enclosed and close cameras only, which are
the two that repeat.

### R1e — delete the old addressing

The node pool is proven, so the chunk renderer comes out. Delete `src/world/residency.*`,
`thumb_cache.*`, `thumbnail.*`, `summary_tree.*` and their tests; delete `shaders/world.glsl`'s
chunk walk, the coarse grids and the thumbnail tiers; fold `node_visibility.comp` into
`visibility.comp` and drop `--chunk-marcher`. `src/gpu/world_buffers.*` loses most of its contents.

The path tracer includes `world.glsl`, so it has to move to `node.glsl` in the same change — that
is the bulk of the work and the reason this is its own sub-step.

*Gate: the grid table above does not move, with those tests deleted rather than disabled.*

**Before starting it, decide the enclosed-room regression.** 1.108 against 0.699 is the only place
the pool is worse, and once the old marcher is gone there is nothing to compare against.

### Then R2 onward

`21-renderer-rewrite.md` §8 has every sub-step with its files and its gate. In order: R2
pixel-driven residency (including R2d, what to draw while a node is still a shell — currently it
draws nothing), R3 the face pass — which is the one that makes the path tracer fast and is the
largest single win in the plan — then R4 directional faces, R5 denoise and composite, R6 post,
R7 the primary ray, R8 infinite detail.

### Debt, tracked so it is not lost

- **R0d**: `tools\baseline.ps1 -Out documentation\baselines\r0-before-rewrite.csv` has never
  completed a full run — it was interrupted twice. The directory exists and holds only the face
  counts. Run it — but note that it now passes `--settle`, so each of the forty-two runs waits for
  the scene to stop sharpening and the grid takes the better part of an hour rather than minutes.
  Worth fixing first: the clip cache would make every one of those runs start from a finished world
  if it were written when refinement *settles* rather than only when the last region lands, which
  needs the cache to record which regions are done so a later run from another camera can carry on.
- **`12-plain-english.md`** has nothing about the rewrite.
- ~~**Nothing is committed.**~~ Out of date: the rewrite landed as `669f883`, "One sparse octree
  replaces four addressing schemes". Ask before committing anything on top of it.
- `src/gpu/node_buffers.cpp` uploads whole array prefixes rather than dirty ranges. Deliberate and
  documented; revisit only if it shows on the frame graph.
- The mojibake em-dashes in `shaders/node.glsl` and `resolve.comp` came from a round trip through
  a non-UTF-8 writer. Harmless, ugly, worth a sweep.

---

## 6. The state of the tree

**New:** `src/world/node_pool.{hpp,cpp}`, `src/gpu/node_buffers.{hpp,cpp}`,
`src/core/pass_ledger.{hpp,cpp}`, `shaders/node.glsl`, `shaders/node_visibility.comp`,
`tests/test_node_pool.cpp`, `tests/test_pass_ledger.cpp`, `tools/{baseline,facecount,_grid,_measure}.ps1`,
`documentation/21-renderer-rewrite.md`, this file.

**Modified:** `CMakeLists.txt`, `src/app/main.cpp`, `src/gpu/profiler.{hpp,cpp}`,
`shaders/{pathtrace,resolve}.comp`, `tools/speckle.ps1`, `documentation/{13-decision-log,README}.md`.

Nothing has been deleted yet.

## 7. Commands

```powershell
.\build.bat                          # build; NEVER pipe this to Out-Null while measuring
.\build\bin\ws_tests.exe             # the whole suite - not a name filter, which silently skips
.\tools\baseline.ps1 -Out docs.csv   # the fixed grid; -Compare <csv> to diff a previous run
.\tools\facecount.ps1                # distinct visible faces per view and resolution
```

Compare the two marchers on one camera:

```powershell
.\build\bin\WorldShaper.exe --screenshot out.png --screenshot-frame 300 `
  --width 1280 --height 800 --cam "0,10,-60,90,-6" --quality 7 `
  --no-vsync --no-update-check --no-auto-quality
```

**The node pool is what the game launches with** (D224). Add `--chunk-marcher` for the old one;
`--node-pool` still parses and is now a no-op that says what it means. `--debug-mode 11` writes each pixel's face key as four
exact bytes; `12`–`15` write one word of the visibility buffer the same way, which is how a
disagreement gets localised to a field instead of argued about from a screenshot. The node pool's
GPU mirror is checked automatically at the screenshot in `--node-pool` mode and logs either
`GPU mirror matches` or the first differing byte.
