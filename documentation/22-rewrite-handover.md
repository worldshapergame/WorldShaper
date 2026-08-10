# 22 — Renderer rewrite: handover

*Written 2026-08-09, revised 2026-08-10, for somebody picking this up with no memory of the rewrite
**or of the game**. Everything needed to continue is here or named here. Read §0 for what the
project is, §1 for what was asked, §3 for what exists, §4 for the traps, and start work at §5.*

*The bug §4b used to open with — a deleted wall's shadow outliving it — is closed (D357–D361).
§4b now records how, because the shape of it is the useful part. **Start work at §5.***

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

`12-plain-english.md` is the user-facing explanation and now **covers the rewrite** — what a chunk
was and why it is going, what is done, and what the face pass is for. It is the only document
written for the person the work is for, so it is the one to keep current.

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

### How much is left, by the plan's own sizing

| Stage | Size | State |
|---|---|---|
| R0 instruments | S | a–c done. **R0d** outstanding — record the grid; now 6.6 s a run rather than 133 |
| R1 node pool | XL | a–d, f–i done. **R1e** outstanding, and it is most of what remains of R1 |
| R2 pixel residency | L | a–d done, plus the eviction churn and the edit cost. R2b landed with a stated limit |
| R3 the face pass | XL | **a, b done; c half** — the store, its mirror, the producer, the shading pass and the composite that reads it. Sun only; lamps, sky and bounce are the rest of R3c. **R3d not started** |
| R9 the off-screen set | L | **d done, early** (D308–D311: a face with no light of its own reads the coarse face standing over it — see below). The rest **planned, not started.** The face store holds what the camera can see, so light is a screen-space set in world-space clothing. A mirror facing a wall behind the camera reflects nothing, because the wall has no face. R9f–R9h extend it to light from regions that are not loaded at all: light folds up the tree as colour does and outlives its children, the emitter list persists per region and loads with the index rather than the voxels, and **no light path may cause streaming**. §8 R9 |
| R10 ambient occlusion | L | **planned, not started.** §8 R10. The composite applies an ambient term to every surface with **no occlusion in it at all** — `sees_sky` is decided from which way a face points and nothing else — so the interior is lit as though the building stood in the open. It is the same integral the sun already gets, over the hemisphere instead of the sun's disc, so it is the same machinery; it is sub-voxel because the face pass already jitters its sample across the face and throws the position away, and keeping the Legendre moments of that costs no rays and no solve; and it converges once and then stops tracing, because occlusion of static geometry is a constant |
| R4 directional faces | L | not started — **R9 first**, or a reflection is of an empty set |
| R5 face denoise, composite | M | not started |
| R6 post | M | not started |
| R7 the primary ray | L | not started |
| R8 infinite detail | XL | not started |

**Weighted by those sizes, roughly a fifth to a quarter of the plan is done**, and what is done is
the foundation rather than the feature: the marcher, its residency, and the instruments that make
either measurable. Against the three things the user actually asked for:

- **the path tracer, faster and cleaner** — R3 to R6. **Begun**: the real-time path now takes its
  sun from the face store rather than from the pixel, which is the first thing in the plan that
  actually moves light off the screen. The *reference* tracer (`--pathtrace`, F4) still shades per
  pixel over the old face cache and still includes `world.glsl`; that is R3d and R1e;
- **chunks removed** — the node pool is proven and is what the game launches with, but the chunk
  system is still in the build and still maintained every frame, at about 12 ms of CPU. R1e;
- **streaming and coarse resolution driven by pixels** — half. Feedback drives residency and a ray
  reports what it uses; the sub-pixel rule and proximity are R2b and R2c.

The unplanned work has been most of the elapsed time and is not visible in that table: nine faults
found and fixed, of which three were introduced by this rewrite and six predate it.

### R1 — the node pool (a–d and f–i done, e outstanding)

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
   region by region in the background, and a screenshot at frame 300 catches however much exists by
   then. **The bias runs against whatever you are testing**: a faster build gets there sooner with
   less world in front of it. Two runs of one binary differed on 52,292 pixels. Use `--settle`, and
   check the `scene:` line printed beside the pass table — including its **content hash**, which is
   the scene's identity rather than a description of it — before comparing two numbers. D229–D232,
   D243.

   The cache half of this is fixed (D241): it used to be written only when the *last* region landed,
   which never happens from a fixed camera, so the world was rebuilt on every launch. It is now
   written when refinement settles, with the list of which regions are sharp. **The consequence for
   old figures is that they are not comparable with new ones** — a run that loaded a finished world
   and a run that watched it sharpen draw different pictures from the same world, because the node
   pool evicts nothing (D244).
9. **Repeat a figure before calling it a regression.** The sky camera read 0.484 against 0.759
   between two builds and looked like a 57% regression on the floor view. Three runs of *one*
   build against *one* scene give 0.481, 0.763, 0.472. The empty-space views inherit the node
   pool's own irreproducibility (D233) through the ray clip, and a single sample on one of them is
   not evidence in either direction. D234.
10. **A debug view where two different answers are the same colour will produce a wrong number, and
    it will be the number you went there to get.** Trap 7, in the instrument rather than in the
    engine. Debug view 16 painted "no geometry" the same black a fully shadowed face reads as —
    which is nearly every face indoors — so the first histogram of the R9d change reported the
    enclosed room as 0.3% surface at frame 40 and only looked wrong because it was absurd. Sky is
    green there now. D310.
11. **`Copy-Item` keeps the source's timestamp, so restoring a file can leave a stale `.spv`.**
    Save-edit-measure-restore is the obvious way to run an A/B on a shader, and the restored file
    lands with an mtime *older* than the SPIR-V built from the edit, so ninja skips it and the
    "after" run measures the "before" build. It reads as five frames agreeing to the digit. Touch
    the file, or restore with `git stash pop`, which stamps it. Trap 2 with a different cause. D311.
13. **Two indexes answering one question will be wrong together, and fixing one of them measures
    nothing.** `world_has` answers below level 8 by walking bricks and at level 8 and above out of a
    cached set of chunks. Both were stale after an edit. Correcting the coarse half alone moved 42
    faces of 50,967 and was reverted as "a plausible latent fault, but not this one" (D345);
    correcting the fine half alone left the frame at one frame a second (D358). Together they took
    the fault to nothing. **A null result on half of a redundant pair is not evidence about that
    half** — it is evidence that the pair has to be tested as a pair. Trap 7 and the whole premise of
    `node_pool.hpp` say the same thing from the other end: the cure is one structure with one
    answer, and until R1e that is not what exists.
14. **When somebody says it is slow, look at their overlay before your grid.** Four exchanges went
    on quoting settled per-pass means at a report of stalls — and the settled grid discards
    transients *by construction*, since that is what `--settle` is for. The overlay's three numbers
    (GPU 0.92 ms, frame 247.51 ms, 99th 2,234 ms) named the culprit immediately, and it was not the
    pass under rewrite. D239, D240.

---

## 4b. The bug that was open here — closed, and what it teaches

A player deletes part of the building and its shadow stays; pull away and the deleted part fades
back in, black; standing still, bricks flicker to plain cubes. Three symptoms, one cause, and it is
**fixed** (D357–D361). What is left here is the shape of it, because the same shape will happen
again.

`NodePool::world_has` asks whether a brick is **allocated**, not whether it holds anything, and a
brick was not freed when its last voxel went. Every child mask in the render tree comes from that
answer, so an emptied region claimed matter for ever — the descent said unbuilt-but-occupied,
occlusion reads that as opaque, and the ancestor folded a colour from freed children and drew it
black.

**The reader never changed.** Making `world_has` test emptiness is correct and costs 726 ms of CPU
a frame, because `!= nullptr` stops at the first allocated brick and `!empty()` must scan past
every emptied one — that is D348/D349 and it is why the fix sat parked for a session. The world is
made honest instead, at the moment it changes:

- `Chunk::set` unlinks a brick when the write clears its last voxel, and the ancestors that lose
  their last child go with it. One descent that was being walked anyway. `drop_brick_if_empty` is
  the same thing for the bulk writers in `op.cpp` that fill a whole brick and that the chunk cannot
  see for itself.
- `World::drop_chunk_if_empty`, called from `apply_op`, is the counterpart one level up — **and it
  is most of the fault**, not a tidy-up. `world_has` answers level 8 and above out of an index
  keyed by which chunks *exist*, so about thirty chunks the edit had emptied went on claiming
  occupancy at every level above the chunk. With only the brick half in, the edited camera still
  ran at **one frame a second**, all of it CPU. D345 had tested the chunk half alone years of
  reasoning ago and measured nothing, because the fine half was wrong at the same time. **Neither
  half is worth anything without the other**, and that is the lesson: an index that is derived from
  a second index is only as true as the worse of the two.
- The gate is headless and passing: `tests/test_node_pool.cpp`, *a region emptied by an edit stops
  being wanted*, plus three in `test_chunk.cpp` and three in `test_op.cpp`.
- Five earlier fixes were tried and measured; all are in `13-decision-log.md` under *one root cause
  behind three symptoms*. Do not repeat them. Four of the five assumed `world_has` was the
  authority and asked how its answer was *used*.

**Measured, close camera, against a same-commit control, after deleting 36 million voxels:** faces
shadowed by ignorance **62,756 → 0**, mean sun visibility **0.1222 → 0.6350**, node-pool CPU
**11.426 → 0.336 ms**, GPU **4.610 → 3.749 ms**, the run **120 s without finishing → 9.0 s**. On an
unedited world the seven-camera grid at three resolutions moves nothing over 3%.

**One thing did not survive the measurement.** The gathering-ray bound (D350, D356) was built again
on top of this and is still neutral, and now the reason is known: the **512-step cap binds long
before sixty metres does**, because a ray near the camera marches single voxels. A distance bound on
a step-bounded ray is not a bound. Not carried. D361.

## 5. What to do next

### Before anything else: the paste, which is what a player actually feels

**Not renderer work, and more important than any of it.** The scene sharpens region by region. The
sampling runs on a background thread; the **paste does not**, and a paste measures **twelve to
fourteen seconds** with the main thread blocked. That is what the in-game overlay reports as a
2,234 ms 99th percentile and a 6,282 ms worst frame, while the GPU sits at 0.92 ms.

Both marchers suffer it identically — swap with F6 and watch — and it predates the rewrite entirely.
It also happens on **every run**, because the finished-world cache is only written when the last
region lands and regions behind walls are never sharpened from a fixed camera, so it never
completes and never caches.

Two fixes, in the order they pay:

1. ~~**Write the cache when sharpening settles**, not only when it completes.~~ **Done** — D241–D246.
   The cache is written at the fixed point carrying a `CachedRegion` per box saying which are sharp,
   and a later run from another camera carries on from it. Default camera, `--settle`: first run
   133.3 s, every run after 6.6 s; two runs from different cameras finish the building and every run
   after that loads a complete world in five to seven seconds. **Read D243 and D244 before comparing
   any figure across this change** — the `scene:` line now carries the world's content hash, and
   figures taken before it are not comparable with figures taken after it.
2. **Slice the paste across frames.** D74 already names this and hands it to Stage 16: *"an edit
   runs in one frame and cannot be interrupted… raising the cap means slicing the work across
   frames"*. This is the one that makes editing feel right, and it is now the whole of what is left
   here: the first run on a cold cache still pastes for up to **17.4 s at a time** with the main
   thread blocked, and the fourteen seconds of stall in a first load is still fourteen seconds.

   **A large chisel is the same fault and is measurable on demand**, which the paste is not: the
   36-million-voxel delete used throughout §4b costs **one frame of 1,209 ms**, of which the op
   itself is 68 ms, the undo capture is 240 ms into 841,944 inverse ops, and the rest is everything
   downstream re-deriving itself. Whatever slices the paste should slice this, and this one can be
   reproduced with a flag rather than by watching a load.

Until the second lands, nobody can judge the renderer by playing a *first* load, because what they
are judging is the paste. Every load after the first is now the renderer.

### Closed: undo restored the world and not its light

**Found by the instrument built for it** (D372), and the shortest path back to it is:

```powershell
.\build\bin\WorldShaper.exe --screenshot out.png --screenshot-frame 660 --settle `
  --width 1280 --height 800 --cam "0,2,-20,90,0" --quality 7 --no-vsync --no-update-check `
  --no-auto-quality --edit "-600,96,-600,600,640,600,0" --edit-frame 400 --undo-frame 500
```

The world came back exactly — same content hash as a run that never edited, to the digit — and the
picture did not: 400 frames later, geometry fully restored, **16.6% of pixels still differed at a
mean of 20.9/255**, all of it light. The player's words were "they do come back, just extremely
slowly", and that is what named it.

**It was neither the region nor the window.** `kEditShadowReach` is 512 voxels and covers the
terrace, and `kShadowRefreshFrames` re-measures every face in the box for 120 frames. What re-
measuring cannot fix is *what it accumulates into*: `face_accumulate` throws a history away only
when a sample contradicts a **unanimous** one (D319), and a face that was still mid-transition when
the second edit landed is unanimous about nothing. Delete the roof and a terrace face starts
climbing from black towards white; undo before it arrives and it has nothing to discard, so it
averages back down inside a 256-sample window. **Fully shadowed faces sat flat at ~42,000 for four
hundred frames against the 105,848 that camera has when it was never edited** — flat, not climbing,
which is the whole tell.

The fix is that the host **says so** instead of leaving it to be inferred: `edit_min.w` is 2 on the
one frame an edited region opens, and faces inside the box drop their history. Exact information
rather than a guess, and D319's rule is left alone for the case it was written for. **0.4427 at
undo+60 → 0.1278 at undo+20**, against a never-edited 0.1304; in pixels, undo+400 goes from 20.9/255
and 16.6% to **1.916 and 2.81%**, where two never-edited runs of this camera differ by 1.493 and
1.42%. D373, D374.

The general lesson is worth more than the fix: **an accumulator that infers "the world changed" from
its own samples cannot see a change that arrives before the last one finished.** Anything else in
this renderer that decides to forget based on agreement between samples has the same hole.

### R3 comes before R1e, deliberately

R1e's bulk is moving `pathtrace.comp` from `world.glsl` onto the node pool — and §9 of the plan
**deletes** `pathtrace.comp` at R3 and replaces it with the face pass. Doing R1e first is building
something to throw away one stage later, so R3 goes first and leaves R1e with nothing to port
(D278). What that costs is the chunk system staying in the build, which since it was resized to
what still reads it is 226 ms of load and about 12 ms of CPU a frame rather than 1.7 s and the
same 12.

### ~~Then, the thing R1e cannot be judged without~~ — solved

**The node pool converges.** D233 said it did not and carried it to R2; D234 blamed the bimodal
empty-space cameras on the same unknown. They were one bug: `last_wanted` is refreshed only by a
*request*, requests come from feedback, and feedback reports **misses** — so a finished tree stops
being asked for anything, every node goes cold on the same frame, and the pool threw away the whole
scene including what the rays were reading. Then it rebuilt it and did it again.

Eviction now happens only under memory pressure (D247). Measured on a static camera over a cached
world:

| | before | after |
|---|---|---|
| nodes at frame 400 / 4000 | 8,684 / 1,713 | **8,696 / 8,696** |
| two converged frames | 114,112 pixels apart | **bit-identical** |
| against the chunk marcher | 767,526 pixels (75%) | **176 pixels (0.017%)** |
| sky camera, three runs | 51% spread | **1% spread** |

**So image diffs gate again** (D249), and R1e's "the grid table does not move" is checkable for the
first time. What remains is a design question with a known answer rather than a blocker: a node
should be marked wanted when a ray *uses* it, which needs the marcher to report hits as well as
misses. That is R2's residency policy.

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

### R3 — the face pass (a and b done, c half) — and how it failed, which is the useful part

The chain, end to end: the marcher stops on a face → it reports the face down the feedback buffer
and *also* writes the face's slot into an R32_UINT image → the CPU claims a slot for each reported
face → `shade_faces.comp` runs one invocation per face and traces one jittered shadow ray at the
sun → `resolve.comp` reads the slot image and multiplies its Lambert term by that face's stored
visibility. Cost: 3–11% on the grid, for a shadow the real-time path has never had.

Between "the face pass computes visibility" and "the picture has a shadow in it" were **five
separate bugs, four of which produced a plausible picture and no error anywhere**. In the order
they were found — this is the shape of the work, not a list of mistakes:

1. **Nothing read it.** The face pass had been shading for two stages and `resolve.comp` still lit
   every pixel itself. Check who *consumes* a pass before optimising it.
2. **The request lattice never moved**, so the same 1/64 of the screen was sampled every frame and
   the store settled at 630 faces against thirty thousand on screen. A fixed sparse grid is not a
   sample; it is a permanent choice of which pixels may speak.
3. **A shadow ray stood in for unbuilt geometry.** R2d has a primary ray draw the parent when the
   pool has no detail; a shadow ray doing that treats "I do not know" as "opaque", and the tree is
   only refined where the *camera* looks. Not one face in the scene was lit.
4. **An eight-bit running mean cannot converge.** A face lit by all 494 of its rays read 245/255
   and would never read more. It is two counts now, and the fraction is worked out on read.
5. **The upload wiped the light.** The record has two owners, and the uploader coalesced dirty runs
   across up to 63 clean records, sending the CPU's zeroed counters over what the card wrote.

Four of those five were found by **one log line** — `faces sun on the card: …`, which reports what
the card actually wrote, split by whether a face can see the sun at all. It is in the audit and it
should stay: a picture cannot tell "every face is shadowed" from "every face is unlit", because both
are a dark building and they have nothing in common to fix.

Then faces became **voxels** rather than bricks (D298), which is what the plan's arithmetic always
assumed and what the one rule says on its face, and which brought three more of the same kind:
`NodeHit` used level 0 to mean "no face" while level 0 had just become the commonest face there is;
`pack_face(0, 0, 0)` is literally zero, which was the store's spelling of an empty slot; and a
**shell** — a node the world says is occupied whose children are not built — was transparent to
shadow rays, so the sun came in through walls that had not finished streaming. That last one was
the interior mottling, and fixing it took enclosed speckle to **3.8 against 17.5 before shadows
existed at all**.

### R9d — the shadow that arrives a second late (done, early)

Reported from playing, and worth reading before touching the face store: *"shadows occluded by
things are not drawn until you actually see them"*. Nothing was wrong with the shadows. A face is
claimed only when a primary ray lands on it **and** that pixel is one of the one-in-sixty-four the
request lattice is asking with, so newly revealed geometry waits up to 64 frames for the lattice,
two for the readback and claim, and four samples to settle — over a second, during which the
composite has no face and falls back to full sun. Indoors that is the most wrong answer there is.

The fix is the codebase's own doctrine (R2d, *draw the parent while waiting*) applied to light: a
face with nothing to say reads the face **three levels above it**, which 512 faces share and which
the lattice therefore cannot miss. Three, not one, because what matters is how many faces share the
stand-in, not how coarse it is. It is claimed on the CPU by shifting the fine key — no extra
feedback traffic, and the buffer is the binding constraint — and only when the fine face is new,
because repeat claims measurably cost and provably buy nothing (D309).

Falling back to full sun, enclosed room, from a cold store: **under 1% at frame 30 rather than frame
78**, and 2,978 wrong pixels against 283,291 at frame 40. GPU unchanged still and turning, settled
picture bit-identical, store +3.0%. D308–D311.

### Shadow latency, and the number to beat (stage one done)

The user asked for the wait before a surface has any shadow to be cut to **a hundredth**, in stages.
Measure it with `--cut` (D312), which jumps the camera once at a given measured frame — under
`--settle` the world has stopped building, so what the new view is missing is light and nothing
else. Instrument: `--debug-mode 16`, magenta plus blue as a share of surface.

Enclosed room, 180° cut, share of surface on the full-sun fallback:

| | cut+1 | cut+2 | cut+3 | cut+5 | cut+8 | cut+15 |
|---|---|---|---|---|---|---|
| before stage one | 100% | 100% | 100% | 100% | 6.8% | 0.3% |
| after stage one | 100% | 100% | 73.9% | 2.6% | 0.6% | 0.2% |
| **after stage two** | **0%** | **0%** | **0%** | **0%** | **0%** | **0%** |

Stage one was two pieces of waste, both free to remove: the face mirror was uploaded *above* the
line that claims faces, and the composite refused to read a face until it had four samples while
showing full sun in the meantime (D313, D314). The two frames left at 100% were the feedback round
trip — report on frame N, host reads it on N+2 because that is when N retires — and no arrangement
of host code shortens that.

Stage two (D316–D318) is the card claiming a stand-in itself, in the pass that discovers it. Three
things to know before touching it:

- **The slot is the bucket.** There is no allocator, so a claim is one `atomicCompSwap`, and the
  pixel that loses the race is handed the winner's slot as the return value — in the same
  instruction, with no ordering between workgroups to depend on. That single property is what
  removed the second pass the plan had budgeted for.
- **The card's records live in the tail of the faces buffer**, above `max_faces`, and its buckets
  are a separate buffer. Not sharing the store's bucket array is the point: that array is uploaded
  from the host every frame and would overwrite anything the card wrote. D295 is the fault this
  arrangement makes unrepresentable.
- **The visibility pass writes faces now**, so there is a barrier before the shading pass that was
  not there before, and the shading pass gates provisional slots on the mark's frame stamp. Without
  the second half, every bucket ever used is re-traced every frame for the rest of the run.

**Stage three is the edit** (D319–D321). A revealed face has no answer; an edited one has a wrong
answer it is confident about, which is the harder case — a slab placed over sunlit roof had reached
only 52% of its shadow after three hundred frames, while its own faces were lit correctly at once.
Two rules fix it: a sample that contradicts a *unanimous* history is treated as the world having
changed, so the face keeps its ratio and drops to two samples; and faces inside the edited region
skip the shading stride, using the box the path tracer has had in the parameter block all along.
Detection in one frame, 93% of the converged shadow by edit+30.

**The carved-skylight case that was written up here was not real** — a control with no edit
reproduces the number to three decimal places (D322). Three hypotheses died on the way; the useful
one is what the control exposed instead, and it is the biggest open fault in the renderer right now:

> **A sealed room fills with sunlight as the node pool sheds. Static camera, no edit.** Frame 500,
> 442,968 nodes: visibility 0.0000. Frame 700, 6,972 nodes: 0.0458. Frame 900: 0.0596, still
> climbing. The shedding is R2 working correctly — pixel-driven residency keeps what the screen
> needs — and a shadow ray needs the roof and the outer walls, which the screen does not. It
> predates this session (0.0266 at `f902a00`) and D314/D319 amplify it, because a leaked ray now
> moves a wall to a third instead of to 1/257.

Do not respond by reverting D319. **Planned as R9i** in `21-renderer-rewrite.md` §8, which has the
two candidate shapes and says which to do first: make an evicted subtree read as WANTED to an
occlusion ray (a rule, cheap, fails towards dark), then make a shadow ray's occluder count as use (a
mechanism, correct, and the one D292 has to be narrowed for). D322, D323.

**The first of the two is done and the leak is stopped (D324).** It turned out not to need a shader
rule at all: the shader was already right and the pool was not leaving it a shell to be right about.
A cold root freed its subtree *and* cleared its node, its entry and its `live_` record, so a 512 m
block that had gone cold answered "nothing is here" instead of "something is here I have not built",
and occlusion reads the first as open sky (D302). A cold root now sheds only its children and stands.
Enclosed camera, frame 900: **1,163 faces in full sun → 0**, mean 0.02 → **0.0000**, four of eight
roots evicted → **all eight standing**, and it holds to frame 5,000 at a steady 7,168 nodes. The
memory still goes — the subtree is where the memory was. It costs nothing: the 42-run grid was run on
this build and on a same-commit control with the change stashed out, and the total moved **+0.46%**
with speckle identical to two decimals in all 42 cells.

**It is not finished.** At frame 500, on full residency, 9 faces still read fully lit and the mean is
0.0002; the control reads the same, so that residual has nothing to do with eviction and is the next
thing to find. And the second shape — a shadow ray's occluders counting as use — is still open and
is still what makes the room *right* rather than merely dark.

The answer is correct from the first frame, not merely present: at cut+1 the enclosed room reads
identically to the same camera 120 frames later. Where the answer is not uniformly black, the
stand-in is about a tenth too bright and sharpens — against a fallback that was twenty times too
bright.

**What is left of R3c**: sky, lamps and bounce, all on the same one-invocation-per-face footing.
**R3d** deletes the per-pixel light path, and carries one debt from here — split `GpuFace` so the
CPU's half and the card's half are never in one copy, which is what makes bug 5 impossible rather
than merely absent.

### R10 — ambient occlusion, and why it is planned next to R3c rather than as a feature

Asked for by the user, planned in `21-renderer-rewrite.md` §8 R10 and not started. Read that section
before the rest of this list, because it changes what R3c's sky sub-step is: **the composite already
applies an ambient term to every surface in the frame and there is nothing anywhere that occludes
it**, so a corridor and an open field receive the same dome. AO is not an effect to add on top; it
is the missing visibility on a term already being applied, and it is the same integral the sun gets
over a different set of directions. Building the sky term and AO as two things guarantees they will
disagree, and the failure — every crease double-darkened — reads as taste rather than as a bug.

Three claims in it that are worth knowing before touching the face pass: the sub-voxel half is free
because `shade_faces.comp` already jitters its sample across the face and throws the position away;
the fit needs no solve because the jitter is uniform, so the Legendre basis is already orthogonal
under it and every coefficient is a running mean; and the steady-state cost is **zero rays**,
because occlusion of geometry that is not being edited is a constant and the sun is the only thing
that has to be re-traced for ever.

### Then R2 onward

`21-renderer-rewrite.md` §8 has every sub-step with its files and its gate. In order: R2
pixel-driven residency (including R2d, what to draw while a node is still a shell — currently it
draws nothing), R3 the face pass — which is the one that makes the path tracer fast and is the
largest single win in the plan — then R4 directional faces, R5 denoise and composite, R6 post,
R7 the primary ray, R8 infinite detail.

### Debt, tracked so it is not lost

- **R0d**: `tools\baseline.ps1 -Out documentation\baselines\r0-before-rewrite.csv` has never
  completed a full run — it was interrupted twice. The directory exists and holds only the face
  counts. **The reason it was unaffordable is gone** (D241): a settled run was 133 s and is 6.6 s,
  and after two runs from different cameras the world is complete and every run measures the same
  scene. The grid is now minutes. Run it, check the `scene:` content hashes agree across the runs
  being compared, and commit the csv. Note that the first grid run still pays one cold build.
- **`12-plain-english.md`** has nothing about the rewrite.
- ~~**Nothing is committed.**~~ Out of date: the rewrite landed as `669f883`, "One sparse octree
  replaces four addressing schemes". Ask before committing anything on top of it.
- `src/gpu/node_buffers.cpp` uploads whole array prefixes rather than dirty ranges. Deliberate and
  documented; revisit only if it shows on the frame graph.
- The mojibake em-dashes in `shaders/node.glsl` and `resolve.comp` came from a round trip through
  a non-UTF-8 writer. Harmless, ugly, worth a sweep.

---

## 6. The state of the tree

**New:** `src/world/node_pool.{hpp,cpp}`, `src/world/face_store.{hpp,cpp}`,
`src/gpu/node_buffers.{hpp,cpp}`, `src/gpu/face_buffers.{hpp,cpp}`, `src/core/pass_ledger.{hpp,cpp}`,
`src/core/dirty_set.hpp`, `shaders/node.glsl`, `shaders/node_visibility.comp`,
`shaders/shade_faces.comp`, `tests/test_node_pool.cpp`, `tests/test_face_store.cpp`,
`tests/test_pass_ledger.cpp`, `tests/test_world_cache.cpp`,
`tools/{baseline,facecount,_grid,_measure}.ps1`, `documentation/21-renderer-rewrite.md`, this file.

**Modified:** `CMakeLists.txt`, `src/app/main.cpp`, `src/core/hash.hpp`, `src/gpu/image.hpp`,
`src/world/world_cache.{hpp,cpp}`, `src/gpu/profiler.{hpp,cpp}`, `shaders/{pathtrace,resolve}.comp`,
`tools/speckle.ps1`, `documentation/{13-decision-log,README}.md`.

Nothing has been deleted yet.

## 7. Commands

```powershell
.\build.bat                          # build; NEVER pipe this to Out-Null while measuring
.\build\bin\ws_tests.exe             # the whole suite - not a name filter, which silently skips
.\tools\baseline.ps1 -Out docs.csv   # the fixed grid; -Compare <csv> to diff a previous run
.\tools\facecount.ps1                # distinct visible faces per view and resolution
```

**Every scripted run ends on the clock, at 180 s, and says so at startup** (D362). A frame count
cannot bound a run whose frames are the thing that got slow, and it is the slow build that most
needs to report. The shot is still taken and the pass table still printed, with a warning saying
the frame it was asked for was not reached — which is itself the result. `--max-seconds N` moves
the deadline; `--max-seconds 0` removes it, and that has to be said out loud. Note that a **cold**
clip cache is about 133 s of sampling before any frame runs, so the first run against a new clip
may want more than the default.

The edited case, which is what the shadow work is judged on:

```powershell
.\build\bin\WorldShaper.exe --screenshot out.png --screenshot-frame 660 --settle `
  --width 1280 --height 800 --cam "0,2,-20,90,0" --quality 7 --no-vsync --no-update-check `
  --no-auto-quality --edit "-600,96,-600,600,640,600,0" --edit-frame 400
```

`--edit-frame` counts RAW frames and `--screenshot-frame` counts MEASURED ones, so under `--settle`
the edit frame has to be past where the world settles — it prints `world settled at frame N`.

The tool previews and the history are scriptable too, and both hooks exist because the thing they
drive could not otherwise be photographed (D368, D372):

```powershell
--preview x0,y0,z0,x1,y1,z1,s   # s: 1 carve, 2 place, 3 refused, 6 the cursor marker
--preview-mark x,y,z            # a constraint cross, repeatable
--undo-frame N  --redo-frame N  # press undo or redo once, on the same path the key takes
```

**Serialise measurement runs.** Trap 9's corollary, learned the hard way in D367: a previous run
still shutting down holds the GPU, and the next run reads 1.4 ms where it should read 0.70. Wait for
the process to exit between runs — three configurations were misattributed before a control that
should have matched a known baseline gave it away.

Compare the two marchers on one camera:

```powershell
.\build\bin\WorldShaper.exe --screenshot out.png --screenshot-frame 300 `
  --width 1280 --height 800 --cam "0,10,-60,90,-6" --quality 7 `
  --no-vsync --no-update-check --no-auto-quality
```

**The node pool is what the game launches with** (D224). Add `--chunk-marcher` for the old one;
`--node-pool` still parses and is now a no-op that says what it means. `--debug-mode 11` writes each pixel's face key as four
exact bytes; `12`–`15` write one word of the visibility buffer the same way, which is how a
disagreement gets localised to a field instead of argued about from a screenshot. **`16` is the sun
term on its own** and is the instrument for anything about shadows: grey is the visibility fraction,
**magenta** a face the composite could not find, **blue** one it will not believe yet, **green** no
geometry. Magenta and blue are the pixels being lit by the fallback, so their share of the surface
is a number rather than an impression — that is what R9d was measured with. The node pool's
GPU mirror is checked automatically at the screenshot in `--node-pool` mode and logs either
`GPU mirror matches` or the first differing byte.
