# 22 — Renderer rewrite: handover

*Written 2026-08-09, revised 2026-08-10, for somebody picking this up with no memory of the rewrite
**or of the game**. Everything needed to continue is here or named here. Read §0 for what the
project is, §1 for what was asked, §3 for what exists, §4 for the traps, and start work at §5.*

*The bug §4b used to open with — a deleted wall's shadow outliving it — is closed (D357–D361).
§4b now records how, because the shape of it is the useful part. **Start work at §5, at the block
headed "THE ORDER" — the user chose it on 2026-08-13 and it is three numbered steps.***

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
- the **path tracer** — `shaders/pathtrace.comp`, reached with `--pathtrace` or F4. **Deleted by
  R3d**; this paragraph describes what the rewrite started from. It shaded per
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

### How to work with the person this is for — asked for directly, and it is firm

Three rules, and they are about the shape of the exchange rather than about the code. The person
this is for does not read code and does not run the measurements; what they have is the build and
what they can tell you is what it looks like. All three exist to keep that loop working.

1. **Before starting, say what you are about to do and HOW THEY WILL SEE IT IN GAME.** Not the stage
   number, not the file list — what to do with the mouse and the keyboard, what should look
   different afterwards, and what would mean it went wrong. A stage of this rewrite that cannot be
   described that way is one nobody can accept or reject, and this project's acceptance test is a
   player going to look for a bug and not finding it (see the blocky-flicker section in §5, which is
   the one that was closed that way).
2. **Every time you report back, COMMIT first.** Not at the end of a session and not when a stage
   finishes — with the report. A report describes a build; if the build is not in the history, the
   report is about something nobody can go back to. It also means the ledger in
   `21-renderer-rewrite.md` §8.0 and the entries in `13-decision-log.md` land at the same time as
   the code they describe, which is what makes the log usable at all.
3. **The record carries what did not work; the REPORT does not end on it.** Asked for directly on
   2026-08-14 and it revises what this rule used to say. Which half of a sub-step landed, what was
   measured and came out neutral, what was built and reverted, and every wrong turn — all of that
   still goes in `13-decision-log.md`, and it is half of what makes that file worth keeping. What
   changed is where it goes in a message to the person this is for.

   **A report leads with what was built and ends with the next step.** It does not close with a
   list of what was not done, what went wrong, or what was got wrong on the way — the user's own
   words: *"this may cause you to fail further by manifestation."* Their build, their call.

   This is not licence to hide anything. A fact that changes what they would decide is stated
   plainly, in place, as part of the work it belongs to — a measurement that came out neutral, a
   stage that is blocked, a thing that is still open. **State it once, in the flow, in the terms
   the work is in, and move on.** What is being dropped is the closing inventory of failures, the
   self-criticism, and the tallying of mistakes; what is being kept is every fact.
4. **End every report with the NEXT step and what it will look like in game.** The same test as rule
   1, applied one step ahead: name what is next and say what they should expect to see when it
   lands, in the same terms — what to do, what should change, what would mean it failed. That is
   what makes it possible to say *"do that one first"* or *"that is not what bothers me"* before a
   session is spent on it, and this rewrite has more than one stage that was built in the right
   order by the plan and the wrong order by what a player actually feels (§5's opening two sections
   are both that mistake). If the next step genuinely cannot be seen — an instrument, a refactor,
   a measurement — say that plainly instead of inventing a symptom for it; "you will see nothing,
   and here is what it buys the step after" is an answer they can act on.

5. **After every change: build, test, COMMIT, and merge it to `main` on GitHub.** Not at the end of a
   session and not when a stage finishes — with the change. Rule 2 says a report describes a build
   nobody can go back to unless it is in the history; this is the other half of that sentence, and
   the half that was being skipped. A commit sitting on a local branch is not in the history the
   person this is for can reach, and `main` was **58 commits behind `origin/main`** when somebody
   finally looked.

   The whole loop, and none of it is optional:

   ```bash
   build.bat  then  build\bin\ws_tests.exe
   git commit -a
   git checkout main && git merge --ff-only <branch> && git push origin main
   ```

   Three things that make this a rule rather than a habit. **Build before you commit** — a working
   tree that has not been compiled since the last edit is not a change, it is a draft, and one was
   nearly merged that way. **Kill any stale `WorldShaper.exe` first**: the linker fails with
   `LNK1168: cannot open bin\WorldShaper.exe for writing` and it reads like a code fault. And
   **`--ff-only`**, because a rewrite branch that has diverged from `main` is something to find out
   about deliberately rather than by watching git invent a merge commit.

   **A release is the same loop with a tag on the end, and it is hand-built.** `.github/workflows/
   release.yml` has never once succeeded on this repository — the runner crashes its own compiler
   with an access violation, at v0.6.0, v0.6.1 and v0.7.0 alike. So `tools/package.ps1` is the path,
   its own header says so, and the release notes have to say the download carries **no provenance
   attestation**. `package.ps1` cannot currently invoke `build.bat` either (`vswhere` does not
   resolve through it), so its steps are run against an already-gated build: stage, zip, **unpack to
   a clean directory and run it there**, hash. That last one is not a formality — it is the gate that
   caught v0.6.0 shipping with a shader path hard-coded to the build machine, which passed every
   other check and then opened a black window on every computer but one.

**All five are one rule wearing five hats:** the person this is for can only judge the build, so
every exchange has to be anchored to what the build will do — before, during, after, next, and in a
place they can actually get at it.

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
| R1 node pool | XL | **done, all of it** — R1e's fifth slice took the rest: `residency.*`, `world_buffers.*`, both orphaned descriptor sets, the tracer's 256 MB face cache and `rebuild_coarse_grids`. Device memory 970 MB → 112, warm start 505 → 340 ms, and an edit stops paying 3.86 ms for grids nothing reads. D521–D525 |
| R2 pixel residency | L | a–d done, plus the eviction churn and the edit cost. R2b landed with a stated limit. **A ray now reports what it READS and not only where it stopped** (D427), which is what "wanted" was supposed to mean since D247 |
| R3 the face pass | XL | **R3d done** — the per-pixel light path is deleted. | **a, b, c done** — the store, its mirror, the producer, the shading pass and the composite that reads it. Sun (D290–D303), sky and ambient occlusion (R10, D325–D400), and now **lamps** (D401–D409): a fitting is aimed at from the face, one per face per frame, and it converges and stops. Bounce is R9's. **R3d not started** |
| R9 the off-screen set | L | **a, b, e done and f half done** (D526–D532, D554–D560, D569–D572): a light ray names the one face it landed on, the store holds those in a class whose cap is the table's SPARE room rather than a fixed quarter — which was worth **150.1 → 157.4** of 255 in the enclosed room on its own, and needed the store's eviction order fixed beside it or the coarse pyramid paid for it — both classes are counted, and the coarse pyramid now outlives the fine faces under it — which it did not, at all: the control arm holds **0 stand-ins of 711,000 faces**. Bounce reads them (D533–D538), and a ray that still finds nothing walks up. The probe says **a third of what the bounce integrates is still black**, which is what R9c and R9g–R9h are worth. **d done, early** (D308–D311: a face with no light of its own reads the coarse face standing over it — see below). R9c and R9f–R9h **planned, not started.** The face store holds what the camera can see, so light is a screen-space set in world-space clothing. A mirror facing a wall behind the camera reflects nothing, because the wall has no face. R9f–R9h extend it to light from regions that are not loaded at all: light folds up the tree as colour does and outlives its children, the emitter list persists per region and loads with the index rather than the voxels, and **no light path may cause streaming**. §8 R9 |
| R10 ambient occlusion | L | **done** (D325–D337, D381–D396). The far field (sky visibility, R10a), the near field (first-hit distance through a falloff over a metre, R10b — the term that actually carries shape, because indoors every ray hits something and the far field saturates) and the linear gradient across each face (R10c, from moments the samples already carry: no rays, no passes, no least squares). The quadratic terms §8 calls for were **built, measured and reverted** — they moved the picture by less than the renderer's own run-to-run noise, because a face is a voxel now and a voxel has no curvature inside it (D336, D337). **R10d, convergence, is done too** (D388–D396): the term now measures itself hard and stops, instead of trickling one ray a visit for ever. See §5 |
| R4 directional faces | L | **started, and it is what the user chose over R9c** — R4a is done, both halves (D582, D583, D591), **R4c** is in (D591, D592) and so is **R4b's ray** (D594). A face resolves what it is made of once; the composite then splits what leaves it by metalness, so the metals stop being Lambertian — bronze, gilt, lead and copper read as metal rather than chalk. The sun comes back through a GGX lobe with no storage; the environment out of **sixteen outgoing bins** in a pool of 65,536 blocks (8.7 MB) that faces HOLD, filled by the gathering ray they were already casting. A face that holds a lobe then casts its own ray, aimed into the cone each bin gathers from, which is what fills the grazing bins a reflection is read out of — bronze reads as deep metal with panel structure where it was a flat wash, and the glass gains a sky-coloured sheen. Costs **nothing measurable settled** and **1.5 ms flying**. **What is still owed**: the bin count does not follow pixel coverage, and the lobe is visibly mottled face to face at 24 samples a bin, which is R5's `face_denoise` and is the next thing this stage wants. **R4d is HALF in — transmission, not refraction** (D601–D604): a face resolves what it lets past and stores it, the sun and sky rays stop being blocked by a pane and are tinted per METRE rather than per voxel, and the primary ray marches on behind the glass so a window is fifteen glazed lights with the bars across them instead of one milky panel. **+0.246 ms (+4.8%) at a camera facing a window**, nothing measurable outdoors or enclosed. **Refraction itself is not started**: `ior` is carried and read by no ray, nothing is displaced, there is no Beer-Lambert over the true path and no dispersion |
| R5 face denoise, composite | M | **a done** (D573–D576) — the first thing here that filters ACROSS faces. `open_sky`, the bounce and the lamps blended with a face's coplanar neighbours' in a 3×3 tent, with no edge-stopping term at all because the face key already answers that question. Roughness **4.35 → 2.97** at the steps and **3.01 → 1.72** enclosed, speckle 35.20 → 27.53 and **12.11 → 7.99**, mean pixel unmoved, flying inside its own spread. Costs 29.6 MB and takes the settled close camera to 4.06 ms of a 4.40 budget. **b, c, d not started** |
| R6 post | M | **the light meter is done** (D577, D578) — it was not a sub-step in the plan because the tracer had one when the plan was written, and R3d and R1e between them left `kPreviewExposure` a constant of 3.2 with **no writer at all**. Two clips written to test exposure could not be used because of it: `many_lamps.clip` read **248.9 of 255** and `exposure_range.clip` **35.8**; they read **150.6** and **149.3** now. The facility moves 2–6%, because `kExposureBias` is a separate constant from `kMiddleGrey`. **a, b, c not started** |
| R7 the primary ray | L | not started |
| R8 infinite detail | XL | not started. **Re-sized to L**: R8c and R8d moved into R11 (D612) |
| R11 the world source | XL | **a done** (D613) — the instrument, the mapping from a node to a box and a resolution, and an agreement check that failed and found a fifteen-month-old fault in the sampler's bulk settle. One node at the leaf is **1.389 ms** against 0.213 for an empty one; the fixed cost is the paint rules and not the box. **b–h not started, and b is next.** This is the stage the loading bar is in |
| R12 the field on the card | L | not started. R11's successor |

**Weighted by those sizes, roughly a fifth to a quarter of the plan is done**, and what is done is
the foundation rather than the feature: the marcher, its residency, and the instruments that make
either measurable. Against the three things the user actually asked for:

- **the path tracer, faster and cleaner** — R3 to R6. **Well begun**: the real-time path now takes
  its **sun**, its **ambient occlusion** and its **lamps** from the face store rather than from the
  pixel, and every one of the three converges and then costs nothing. That is three of the four
  terms a picture is made of moved off the screen; the fourth is bounce, which is R9's. The
  *reference* tracer (`--pathtrace`, F4) still shades per pixel over the old face cache and still
  includes `world.glsl`; that was R3d and R1e, and both are done;
- **chunks removed** — **done**. Nothing in the renderer addresses a chunk: one sparse octree
  marches, `node_buffers` mirrors it, and the composite reads a face store and two interned
  tables. Chunks remain what `03-voxel-data-model.md` says they are, a storage and networking
  unit, and `World`, `Chunk`, `serialize` and `world_cache` never changed;
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
15. **A clean measurement and a measurement that never ran look identical, so check it against
    something that should NOT be clean.** The consecutive-frame pair for D427 was written
    `@($Frame, $Frame + 1)`, and PowerShell binds the comma tighter than the plus: that is
    `(2400, 2400) + 1`, three shots whose first two are the *same frame*. Both arms reported nought
    pixels of difference, which is exactly what a working fix looks like. What gave it away was the
    eviction counter printed beside it also reading nought, on a run that had certainly evicted a
    quarter of a million nodes. Traps 2 and 4 and D420 are the same shape three more times; the
    portable half is not about PowerShell. D428.
16. **When a signal is suspect, do not measure it against itself — count the events that produced
    it.** Four fixes were reasoned about for the flicker before an instrument existed and three were
    wrong, because every one of them asked how a report was *used*. The question that answered it in
    one line was whether a report had ever been *made*: 99.9% of evictions were of nodes no ray had
    ever mentioned. D426, and D345 says the same thing from the other end.
17. **A cost that tracks nothing about its own output is not a cost, it is a wait — and the
    question to ask of a wait is who else is running.** The region paste was quoted at 1.4 to 14.1
    seconds in this file for two sessions and every figure was true. None of them was about the
    paste. The tell was there the whole time and needed no profiler: the same 991-brick region
    pasted in **146 ms and in 7,076 ms**, and 5,359 bricks went in 80 ms while 4,258 took 1,453.
    Nothing about the work explains 47× on identical output. Printing the suspect's cost **beside
    the cost of what was running at the same time** answered it in one column — it tracked the
    background sample, row for row, and the one region with no sample beside it cost 75 ms.
    Two things follow that are worth more than the fix. A timing figure that covers more than one
    thing hides exactly this, so split it before theorising (the replay and the announcement inside
    that same number were **0 ms**). And **foreground work and background work must never share a
    job pool**: `parallel_for` queues a take-*loop* over a whole range, so the second submitter gets
    no workers until the first has finished and `wait()` runs the first one's jobs on the second
    one's thread. `JobSystem::submitter_collisions()` now counts it. D511–D514.

18. **An accessor whose comment says "read once at an audit" will end up in the frame, and the
    frame will not say so.** `NodePool::stats()` sweeps every node and popcounts every resident
    leaf — 1.5 million popcounts on the facility. R1e moved the overlay's report and the crash
    context off chunk residency's counters and onto it, and a change that only DELETED work
    measured **7.27 ms a frame against a control's 5.19**. Every host cost this file already timed
    came out equal between the two, because none of them covered that line. Splitting the frame
    into head, recording and present named it in one run: head **1.911 ms against 0.154**. The
    counters live in `live_stats()` now and the walk keeps the name that sounds expensive. Trap 17
    from the other end — there, one number covered three things; here, three numbers covered
    everything except the one that mattered. D525.
19. **A harness that has stopped reading the log looks exactly like a clean run.** `baseline.ps1`
    refuses to compare two rows measured against different worlds, and read the scene out of the
    `scene:` line with a regex that wanted the clip ladder's *"N of M regions"* — which a settled
    world does not print. Twenty of twenty-one rows of the last baseline therefore recorded a scene
    of **nought voxels**, and every comparison against them passed the gate by comparing nought
    with nought. It parses the **content hash** now and refuses a row that has none. D524, and it
    is trap 10 living in the instrument rather than in the engine.
20. **A change that makes a pass cheaper by giving it less to do is a regression wearing an
    improvement's clothes.** The sun's ray budget is divided among the faces that want one, and it
    was divided by the store's WATERMARK — so R9a claiming 262,144 faces nobody is looking at
    refreshed every face on screen less often, and the faces pass measured **0.96 ms against a
    control's 1.16**. The cost was real and was in a number no pass table carries: **72 sun samples a
    face against 84**. A timing figure alone cannot tell a pass that got faster from a pass that
    stopped doing its job, and the only defence is to print a convergence number beside every time —
    which is what `sun samples each`, `still bursting` and `cast no more rays at all` are for. D527.
21. **An instrument's own bookkeeping must never live in a field the card is writing to.** `GpuFace`
    has two owners: the host owns the key and the flags, the card owns the light. `FaceBuffers::upload`
    sends whole records for every slot the store marks dirty, so a host-side flag change on a LIVE
    record sends the host's zeroed counters over what the card accumulated. Putting R9's face class in
    the flags byte would have cost **29,882 faces a flight** their light, silently: right picture,
    matching mirror, clean audits, and only the cost moving. D295 is the same fault through a
    different door, and the standing cure is R3b's owed `GpuFace` split. D528.
22. **An audit that cannot run looks exactly like an audit with nothing to report.** The face audit
    returned at its first line whenever an upload was pending — correct for the mirror comparison and
    wrong for the dozen card-only statistics printed under it. A moving camera always has a backlog,
    so those numbers were missing from the one case where this pass costs anything, and two arms of an
    A/B differed by a factor of two with nothing in either log to say why. Split the check that needs
    both sides to agree from the reading that needs only one. D529, and it is trap 15's shape one
    level along.
23. **A convergence figure that is a step function of a budget must be measured at two frame counts,
    not one.** R9f took the sun's stride from 5 to 6, and the far ray needs `kBounceMin` = 512
    samples at one per stride frames: 512×5 = 2,560 lands before the frame the shot was taken at and
    512×6 = 3,072 does not. So a 20% change in a rate read as **107,582 faces converged against
    475,632** — fourfold, and the size of it was entirely an artefact of where the shot was. The
    fault was real and worth fixing; the magnitude was not, and both mistakes are available here: a
    threshold just crossed in one arm reads as a catastrophe, and one just crossed in both reads as
    nothing at all. D557.
24. **A stand-in is the coldest record in the store, by construction, and it is the one everything
    else is rebuilt from.** `last_read_` is stamped by a CLAIM, and a coarse face is claimed only
    when a fine face under it is new — so the moment a camera stops discovering geometry, the one
    record that has to survive the camera leaving is the first one given up. Measured: **0 of
    711,000 faces above level 1** on a settled close camera. The general shape is that a recency
    clock is only as good as what stamps it, and a record whose value is *for later* will always look
    idle to a clock stamped by use. D554, and it is D508's lesson (`last_read_` stamped by the
    lattice rather than by a read) arriving one class along.

25. **A suite that only ever asks one resolution says nothing about the others, and it will look
    like fifteen months of agreement.** `tests/test_sample.cpp` holds a brute-force reference — ask
    every voxel at its centre, keep a cell a feature thinner than a voxel passes through — and every
    one of its eleven subcases compares against it **at thirty-two voxels a metre**. R11 asks for
    eight resolutions, and the first one it asked at outside 32 failed in one line: the sampler
    settled a box empty over cells the per-voxel rule would have kept, because the box test never
    allowed for the rescue's own reach. The parameter that was never varied was not obscure — it is
    the first field of `SampleSettings` — and the whole of the coarse ladder the game already ships
    (`--clip-coarse 4`, metre 8) ran through the untested half. Ask what your tests hold FIXED, and
    whether the thing you are about to build varies it. D613.

26. **Every audit agreeing is not evidence when they all read the same source.** Three checks stand
    over the render tree: `NodeBuffers::audit` compares the card with the pool, and `stale_leaves`
    and `stale_masks` compare the pool with `world_has`. All three printed *"agrees, leaf for leaf"*
    and *"mask for mask"* on a settled camera with **304 lumps standing in front of it**, because
    `world_has` was the thing that was wrong and every one of them is downstream of it. The
    instrument that found it had to ask the world a question no check asked — how many allocated
    bricks hold nothing — and that number could not have been derived from anything the engine
    already printed. Traps 7, 10 and 13 are this from other sides; the addition is that a *redundant
    set of checks* is only as good as its deepest shared reader, and the way to find that reader is
    to ask which one of them could be wrong and leave all the others content. D621.

27. **A counter incremented on one of several failure paths reads as success on the rest.**
    `NodeUploadBatch::out_of_memory` was set where the entry-level shell failed and nowhere else, so
    a pool jammed at its leaf ceiling for the whole of a load printed `deferred 0` — and `refine`'s
    caller incremented `built` whatever `refine` had actually managed, so 252 refusals a frame read
    as 252 successes. The cure is what `note_no_room` now is: every path that gives up funnels
    through one line, so the count cannot be right for some callers and silent for others. Trap 7 in
    the instrument rather than in the structure, and trap 20's twin — there a pass got cheaper by
    doing less, here a pass reported success by failing. D621.

28. **A grid commensurate with the thing it measures measures the grid.** A census asking how close
    the facility's surfaces pass to its sample points reported **44 points within 1e-7 m and the
    same 44 within 1e-5** — a plateau, where a distance field has to thin out as the band does. It
    was not a field property and it was not a bug in the field: a clip is authored in round metres,
    its box is round metres, and a grid dividing that box into forty equal parts lands exactly on
    the building's own faces. All 44 were one column's vertical arris at x = ±11.475, hit dead on,
    answering 1.1e-15. Offsetting every sample by an amount sharing no factor with the building
    reads 469, 0, 0 — the distribution a locally linear field must have. **The wrong version was the
    alarming one**, and it would have argued against an f32 GPU path on the strength of a sampling
    artefact. The general form: whenever a measurement's sample positions are chosen by dividing
    something the subject was also authored against, they are not independent of it. D639.

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

#### CLEARED FIRST: the ladder now stands down (D627)

D626 left 4,788 ms of a cached load doing ladder work that delivered nothing, and that is gone. The
ladder stands down when a sweep finds nothing and is re-armed by the camera moving or the world
changing. **Cached launch: 270 wakes → 33, pick 584 → 147 ms, 99,600 occlusion rays → 11,832, and
the cache written once instead of 28 times — 504 MB → 18 MB.** The cold load is byte-identical
(`789c8a80f40323a1`) and cheaper anyway, 486 wakes → 249.

**Two versions of it were wrong and both are worth knowing.** Standing down on the FIRST empty sweep
loses the tail — an occlusion refusal is retried every `kRefuseFor` wakes and a sleeping ladder never
wakes, so the cold facility settled 32 nodes short and its hash moved. And "stand down once the clock
passes the furthest-out memo" can never be satisfied, because every sweep rewrites the memos it
refuses. The window is measured from the last sweep that **delivered**.

#### R11d IS BUILT AND OPT-IN — `--no-coarse-paste`. Read D630 to D635 before touching it.

The up-front sample is **taken and not pasted**: the ladder builds the world from nothing, seeded at
eight metres. Five gates were run and it passes all five.

| what was asked of it | result |
|---|---|
| does the world grow when the camera moves? | 19,751,324 → **29,622,982** voxels walking the same scene (D632) |
| is a camera-dependent world reproducible? | same camera twice, **`70b51a3f94dc44ba` to the digit**; a second camera differs (D633) |
| does `baseline.ps1`'s gate cope? | **it needed no change** — it pairs rows by view and never compares across cameras (D633) |
| does a partial world resume elsewhere? | B loads A's world in **52 ms** and ends holding both; returning to A is **9.1 s against 18.6** (D634) |
| does a chisel cut with nothing under it? | **yes** — clean faces, and the portico it reveals is built at full detail (D635) |

**Three things keep it opt-in, and none of them is the ladder.**

1. **The loading bar does NOT go with this flag.** The up-front *sample* still runs — 2,760 ms — and
   must, until the stipple verdict has another source. Not pasting saves 959 ms and removes the
   blocky first pass. **D629 and D630 are why the verdict cannot move**, and D630's option 2 is built
   behind `--stipple-from-world` and measured at **+19 s and a verdict that protects nothing from a
   real camera**.
2. **The far chisel is unmeasured** — sixty metres into a surface never approached, where the
   proximity radius has to hold *sampling* rather than *residency* (R2c, D199). That is R11h's
   remaining half.
3. **Flipping the default changes every baseline in the repository**, because the shipped world
   becomes camera-dependent.

**And two mistakes in here are worth more than the feature.** D631 called the smaller world *"a
sixth of a building"* and D632 had to correct it — it is the design, and *"stopped early"* and
*"finished what it could see"* produce **identical counters**; only movement separates them. D635
nearly condemned a working carve on a `0 leaves rebuilt` line that is about what the POOL held, not
about what the edit did.

#### ASKED FOR NEXT: noise and speckle. Start at R5b, and start with these numbers.

The user asked for speckle down 100x. **R5's own gate is 4x** (§8 R5d: *"speckle down 4x against the
R0 baseline on the enclosed-room camera"*), and that is the honest target -- speckle is a variance
measure with a floor set by how many samples a face has had and by real detail in the scene, so a
filter that took it 100x down would have taken the mouldings with it. Say that in those terms.

Measured now, settled, 1280x800 quality 7, facility:

| camera | speckle | fireflies | faces settled |
|---|---|---|---|
| enclosed `0,0,0,-90,0` | **8.93** | **0** | 114,491 of 245,511 |
| close `0,2,-20,90,0` | **28.98** | **54** | 454,799 of 589,870 |

**The close camera is 3.2x the room and carries every firefly, so it is the one to work against** --
and the number that points at why is the last column: **135,071 of its faces, 23%, have not
settled.** The grain is faces that are still measuring, not converged faces disagreeing.

##### STARTED, AND THE FIRST IDEA IS MEASURED AND REFUSED (D644)

**What claim time actually does, which §5 said to check first: it seeds three of the four terms.**
A face claimed fresh inherits the near field, the sky and bounce, and the lamps from its nearest
useful ancestor. **The sun inherits nothing**, and `kShadowSettled` is 1, so the composite reads a
face's own shadow from its very first ray — and one ray is 0% or 100%.

Seeding it the same way is built, behind `--sun-seed N`, and **measured 19% the wrong way**: 108.65
against a control repeating at 91.62 and 91.38 (close camera, 320×200, frame 40, overlay excluded).
It ships at 0. An ancestor's fraction is an average over four times the area, so seeding every child
with it is structured error at the parent's scale — the same argument `face_light_seed` already
refuses the parent's *gradient* on.

**That arm is run too, and it is also worse (D645).** `--shadow-settled 4` holds the composite off a
face's own shadow until it has four rays: **107.74 speckle and 131 FIREFLIES against one**, because
an unsettled face reads as *fully lit* and the steps and lower facade come out white-flecked. The
objection in `kShadowSettled`'s own comment — *"wrong on every face, in the same direction, for four
frames"* — was right, and now has a number.

**The pair is what matters, and neither half says it alone:**

| arm | speckle | fireflies |
|---|---|---|
| what ships — believe the first ray | **91.62**, repeated **91.38** | 1, 2 |
| seed from the ancestor | 108.65 | 1 |
| believe nothing until four rays | 107.74 | **131** |

**The shipped choice is the best of three, and not because a first ray is good.** Both substitutes
are worse than a coin toss — a parent's average is wrong in a *correlated* way, full sun is wrong in
a *bright* way. **So the grain is not a face believing itself too early; it is that there is nothing
better to show.**

**BOTH OF THOSE ARE NOW MEASURED TOO, AND SO ARE TWO MORE (D646, D647). Seven arms, and the
shipped build wins every one of them:**

| what was changed | speckle |
|---|---|
| **nothing — what ships** | **91.62**, repeated **91.38** |
| a young face starts from its ancestor's shadow (`--sun-seed 3`) | 108.65 |
| its shadow is withheld until four rays (`--shadow-settled 4`) | 107.74, **131 fireflies** |
| it shows its coarse stand-in until four rays (`--face-answerable 4`) | 107.61 |
| it casts four sun rays a visit instead of one (`--sun-burst 4`) | 108.70 |
| the agreement filter off (`--denoise-edge 0`) | 102.22 |
| the agreement filter tightened (`--denoise-edge 6`) | 112.54 |

Every world within 0.3% of the same leaf count, every arm a flag on one build.

**The four young-face arms land within 1% of each other although one seeds, one withholds, one
substitutes and one ACCELERATES** — a burst makes a face settle four times faster and costs exactly
what the three that slow it down cost. No account in terms of convergence speed survives that. What
they share is that each creates a **second population of faces**, and the boundary between
populations is what the metric sees. **So the grain is disagreement BETWEEN neighbouring faces, not
noise within one** — which is why a coin toss everybody shares beats a better answer only some hold.

**That predicted where the gain is, and the prediction held**: the lever that makes neighbours agree
is R5a's filter, it is the only intervention that moves the number the right way, and its shipped
sharpness is the minimum of its own sweep. **Nothing reachable by a flag is mistuned.**

**So the next gain needs a mechanism that does not exist yet**, and the diagnosis names its shape:
make neighbours agree more without blending across a real edge. What ships is one ring of taps
applied once, and `face_lobe_denoise`'s `kLobeFiltered` bit is explicit that a filter reading the
array it writes may run once and must not run twice. A second ring, or a second pass over a copy, is
the untried version — a design question rather than a constant.

**And all of it is the TRANSIENT**: frame 40 of a building still arriving, which is what a player
turning their head sees, and not the settled case. `--settle` is unreachable in the container.

~~Which leaves exactly one candidate, and it is now justified by measurement rather than by
argument: show the coarse STAND-IN.~~ **Measured and refused — and D646 corrects the claim under it:
the marcher ALREADY substitutes a stand-in** (`visibility_face_slot`, at `kFaceSettled`), so this
needed a flag rather than the new binding D645 costed it at.

**The old note, kept because the reasoning is still the reasoning:** The store already keeps one over that face (R9f), it is a real
measurement of the same place at the right brightness, and it is what the composite draws before the
first ray anyway. The cost is that **`resolve.comp` has no face lookup at all** — it does not include
`node.glsl` — so this needs a binding and the host plumbing under it. That is the next piece of work,
and the two cheap arms above are what pay for deciding it is worth doing.

**And the metric is computable where the picture is taken**: `tools/speckle.py` is a transliteration
of `_measure.ps1`'s `Measure-Speckle`, with a `--top N` band skip, because **the developer overlay is
in every screenshot, defaults on, has no flag, and its digits differ between two arms of an A/B** —
text is the worst thing a speckle metric can be shown. Whole-frame, the two arms above read 86.66
and 102.71; the picture alone is what the table quotes.

**Half of R5b may already exist.** `face_light_seed` seeds a new face from the coarse stand-in over
it (R9d, D308-D311) and `face_reseed` scales a pair of counts down keeping the ratio, with the
comment at `node.glsl:1350` saying in as many words *"the same arithmetic face_light_seed already
does to a stand-in's history, applied to a face's own"*. So check what claim time actually does
before writing a seeding path; what is more likely missing is the **temporal** half -- the sun is a
running mean that converges and stops (R10d), which is right for a static face and is exactly what
leaves a newly claimed one noisy for its first frames.

**The gate is not the speckle number alone.** R5d's own wording: *two identical frames are
bit-identical, and a slow dolly-out shows no transition.* Temporal averaging fails by SMEARING, and a
smear is invisible in a speckle metric -- it is what `--fly` and the consecutive-frame pair are for
(trap 15).

### THE ORDER, chosen by the user on 2026-08-13: do these three, in this order

The user was shown the measured breakdown of a 17.1 s cold load and asked for the order. They chose
**1, then 2, then 3**. Everything after this block is the detail behind them and the history that
produced them; **start here and read the numbered step you are on.**

The 17.1 s these are against, measured (D622, D623), cold facility, enclosed camera, no cache:

| | | |
|---|---|---|
| startup, device, shaders | ~1.1 s | not addressed by any of the three |
| **the up-front coarse build** | **3.7 s** | sample 2,754 + paste 257 + compact 702 ms. **Step 1 removes all of it** |
| **the ladder** | **12.4 s** | of which **sampling 6,322 ms**, picking 809, pasting 140, and ~5 s of frame time |

**Nothing below is a scheduling change.** D622 took the waiting out — 83.6 s → 17.1 s, 4.9× — and
D623 established that the last 9% of waiting cannot be taken without losing voxels. What is left is
work, and these three are the only three places it lives.

---

#### 0. Since the order was chosen: a bug is closed, a fourth place was found, and step 2 shrank

**Step 2 is measured, both halves, it turned up a 6% that was nobody's stage, and what is left of
it is one number to change (D636, D637, D638).**
Its first half — "bound the 923 nodes that have no box" — does not survive its own histogram: only
177 of them are under the solid at all, and every one of those is either a deliberate refusal or a
shape with no finite extent, so no bounds were written. Its second half, `kAccelerateFrom`, was
measured instead: **twelve is too low and the sweep's minimum is 16–64**, worth 4–13% of the shape
evaluation that is 76% of sampling. The default has NOT been moved, because the sweep was taken on a
container rather than on the development machine; **repeating it there is five minutes and is the
whole of what step 2 has left.** The section itself carries both tables.

**And then the instrument that should have existed before any of it (D638).** `--clip-field` now
counts what one evaluation actually WALKS: **131 nodes in open air, 165 near a face, 150 inside
matter, against the 2,505 the shape can reach** — so the cull is doing its job at 5–7% and there is
no second D637 hiding in it. What the walk is made of is the useful half: **63% of it is structure
rather than distance** — union 24%, difference 9%, translate 8%, mirror 7.5%, intersection 6%, with
`box` at 21% and every other primitive under 4%. **That is the table R12 should be sized from.** On
its first run it also found a bug worth **−6% of shape evaluation, bit-identical**: a union computed
each child's box distance to sort by and then computed the identical number again to cull by, and
the BVH did the same one level up. Now carried rather than recomputed, gated against a stashed
control build whose whole report differs on two lines, both clocks.

**The two together, which is the figure to hold in mind:** with the reuse in AND the threshold at
48, one box of the facility goes **6.68 → 5.71 µs a shape evaluation and the other 6.27 → 4.85,
−15% and −23%**. The first half is already in. The second is the one number waiting on a five-minute
repeat on the development machine, and D637 has the loop to paste.


**Closed (D625).** A cached load — the path every launch after the first takes — was running with
the despeckler off. The stipple verdict is taken once over the whole clip in the up-front coarse
build, `resume_refinement` never took one, and `forge::despeckle` reads an empty verdict as *"leave
every speck alone, everywhere"*. It now travels in the world cache (format 3 → 4, so the first run
after any build of this code is necessarily cold), and the cached world's content hash moved
`007113c0915ed6b1 → 789c8a80f40323a1` — **the cold build's hash**, stable over two runs. Note for
step 1: `refine_stipple_` is now populated on both paths, so R11d does not have to keep the coarse
build alive merely to keep the verdict.

**Closed (D626), and it was two faults rather than the one that was written down here.** The cache
now records the ladder's **whole leaf set** — key, level, corners, `applied_per_metre`, `done` —
and `resume_refinement` rebuilds `refine_regions_` from those keys through `refine_node_of` instead
of seeding at level 8 and asking which saved box contains a seed. Format **4 → 5**.

The fault this section described is real: `already_sharp` tested **seed** nodes for containment in a
saved box, and since R11c a saved box is *smaller* than a seed, so no seed was ever marked done and
`cached world has 0 of 120 nodes sharpened from 19680 saved boxes`. But it only bites when some leaf
reached the authored resolution. **The commoner one is worse.** `save_refined_world` wrote only
leaves with `applied_per_metre >= refine_authored_`, and from the default camera **no leaf ever
reaches that**, so the file was written with an **empty** region list — which `world_cache.hpp`
documents as *"this world was not built through the ladder"*, i.e. a one-pass authored world with
nothing to do. The loader duly did nothing, for ever. Measured on the default camera, the old build's
cached load is **3.83 s** and prints `no ladder, the world is at the detail the clip asked for` over
a building permanently stuck at sixteen voxels a metre. It was not slow. It was finished and wrong.

**Do not quote the 11.7 s figure above for this camera** — it was measured on a camera that does
reach authored detail, and repeating it here sent one session looking for time that was not being
spent. See D626 for the eight-arm table.

**What it cost, honestly.** The steady-state cached load goes **3.83 s → 5.81 s** on this camera, and
in exchange the fixed point is the authored 32 voxels a metre (62,752 level-3 leaves against none)
and reproduces byte-identically run over run. **The 2 s is recoverable and is the next small thing**:
of the new 5.81 s, **4,788 ms is ladder cost delivering 0 nodes** — 13,409 leaves are permanently
occluded from this camera so `done == refine_regions_.size()` never becomes true, and the ladder
sweeps 149 times over 22.4 M entries, 619 ms of it on the **main thread**, discovering it has nothing
to pick. A *"a whole sweep found no candidate → stand down until the camera moves"* teardown gives
that back and turns D626 into a strict win.

---

#### 1. R11d — nothing is sampled up front

**What it is.** The up-front build samples the *whole building* at metre 8 and inflates it 4× on
paste, before the first frame is drawn. It is the last thing in the chain that is not pixel-driven,
and it is 3.7 s of a 17.1 s load — 22% — plus the compact sweep that follows it.

**Read before starting**: the four things R11c leaves it, in *"R11c is done as well"* below. The one
that decides the shape of the work is the first: every node currently knows what the world already
holds where it is (`applied_per_metre`, seeded from `--clip-coarse`), and **removing the up-front
build makes that floor nothing** — so the first frame has no world at all until the first nodes
land. **R11d is therefore not "delete the coarse build"; it is about what is drawn in the meantime.**
D621's stand-in path (`node.glsl`, the `kFoundWanted` branch) is what draws it and is the thing to
reason about, and trap 7 is the rule it must not break.

**Also read** the fourth of those four — though **D626 has since done what it asked for**:
`CachedRegion` now carries `key`, `level` and `applied_per_metre`, and the cache holds the ladder's
whole leaf set rather than only the boxes at authored detail, so coarse work IS remembered across
runs and a resuming node comes back knowing the detail it actually holds. That matters to R11d
directly: with the up-front build gone, a cached load's only floor is what the file says each leaf
holds, and before D626 that was reset to `--clip-coarse` for every node.

**What a player sees.** No loading bar at all. The world builds around them from nothing rather than
starting as a blocky whole building. **What would mean it failed:** spawning into an empty room that
fills with big blocky boxes — which is worse than what is being fixed, and is exactly why b and c
had to land first.

**The gate.** `clips/sampler.clip --refine-all --no-despeckle` must still return
**`a1f8bc6c656343b7`, 1,430,104 voxels**. The facility should lose 3.7 s of wall clock and must keep
its content hash for a given camera, or `baseline.ps1` stops working (R11g).

##### AND THE BLOCKER, found by reading before building: where does the STIPPLE VERDICT come from?

**R11d cannot start until this is answered, and it is not a detail.** The up-front sample is the only
place in the engine that ever looks at the whole building at once, and one thing is taken from it
that nothing else can produce: `refine_stipple_`, the verdict on which materials are a deliberate
dither and must never be despeckled. Every node the ladder refines is despeckled against it. Delete
the up-front sample and there is no verdict, and `forge::despeckle` reads an absent verdict as
**"leave every speck alone, everywhere"** — which is D625, the fault that shipped silently on every
cached load for the life of the feature.

The measurements that already exist and bound the problem:

- **A coarser sample is not a cheaper verdict, it is a different one.** D625 tried exactly this and
  refused it on measurement: at sample metres 8, 4, 2 and 1 the verdict sees **35, 31, 26 and 19**
  materials, and metre 4 shares only two of metre 8's six protected ones — material 27 is spared at
  metre 8 and repainted at metre 4.
- **The verdict today is the metre-8 one**, not the authored one, because `--clip-coarse 4` is what
  the up-front build samples at. So the answer being preserved is not sacred; it is whatever metre 8
  says, and that is the arm any replacement must be compared against.
- **The cache already carries it** (D625, format 4). So this only blocks a COLD load. Every launch
  after the first already has the verdict without a whole-clip sample.
- **The removable part without solving it is only 959 ms of the 3.7 s** — the paste (257 ms) and the
  compact (702 ms). The sample itself is 2,754 ms and is what produces the verdict. Skipping the
  paste alone gives a player 2.75 s of loading bar and then an EMPTY world, which is worse than what
  is being fixed.

**Three candidate answers, in the order they look promising, and each is a measurement rather than
an argument:**

1. ~~**Accumulate it across the ladder's own nodes.**~~ **BUILT, MEASURED, REFUTED — D628.** The
   counts are additive and the idea is sound; the boundary is not. `paint_specks` reads outside the
   clip it is given as AIR, so every voxel on a node's own face counts as surface and as a speck —
   and **a leaf node is 512 cells of which 296, 57.8%, are its own boundary**. Summed over 470,142
   nodes on the facility: **20 materials agree, 11 DIFFER**, and two of the six deliberate dithers
   (358 and 509) would be **cleaned away**. `sampler.clip` agrees perfectly, 0 differ, which is
   exactly why it is not the gate for this.
   **The surviving version is 1b: count against the WORLD after the paste**, where the neighbours
   outside the node are no longer unknown — the same walk, with the edge reading the world instead
   of returning air. Two things to know first: the world holds a mixture of resolutions at that
   moment, and the count moves to the main thread at paste time.
   **The instrument is built and kept**, and it is the gate: `forge::StippleCounts`,
   `stipple_verdict(counts, share)`, and a settle-line comparison naming every material that moves
   and which way. Any route is judged by that line reading **0 DIFFER on the facility**.

   **1b IS BUILT AND IT WORKS — and it settled the whole question. Read D629.**
   `stipple_counts_from_world` captures each chunk with a one-voxel skirt and counts only the
   interior, so the interiors tile the world once and no box has a boundary to lie about. It took
   the facility from 11 materials differing to **5, all in one direction** — and those five are
   `358 392 455 509 554`.
   Then the arm that settles it: `--clip-coarse 1` takes the whole-clip verdict at the AUTHORED
   metre 32 instead of metre 8. **Metre 8 protects six materials; metre 32 protects one — `{27}`.
   Route 1b's world verdict protects exactly `{27}`.** The method is right; the RESOLUTION is the
   difference, and D625's 35/31/26/19 at metres 8/4/2/1 was the same fact without the conclusion.
   **So there is no cheaper source for the metre-8 verdict, because it is not a property of the
   building** — it is whatever `--clip-coarse` happens to sample at, and nothing in the world is
   ever built at metre 8. The specks being judged are metre-32 voxels and the judge is a metre-8
   verdict, which has been true since D610.
   **The next move is a decision rather than a measurement**, and D629 lists the three: keep a
   metre-8 whole-clip sample for the verdict alone (R11d then only saves 959 ms and leaves the
   player an empty world); accept the authored-resolution verdict (free, R11d unblocked, five
   weathering coats stop being protected — has to be LOOKED at); or re-tune `stipple_share` /
   `kStippleFloor`, which were set from metre-8 numbers at D609/D610, until the metre-32 verdict
   protects the same six. The target for the third is the settle line reading **0 DIFFER**.
2. **Derive it from the paint rules** rather than from any sample. A dither is authored as a rule
   keyed on noise; whether that is recoverable statically is unknown and nobody has looked.
3. **Keep one whole-clip sample at the coarsest resolution whose verdict still agrees with metre 8.**
   D625's ladder says metre 4 already disagrees, so this is probably dead — but it has only been
   measured at four resolutions and by material COUNT rather than by which materials.

**Do not start by deleting the coarse build.** Start by answering this, because the answer decides
whether R11d is a scheduling change or a new pass.

---

#### 2. The 923 field nodes that carry no box

**What it is, and it is a measurement rather than a proposal.** `--clip-file clips/facility.clip`
prints, today:

```
field   3744 nodes, 923 with no box (25%), 19 hierarchies over 479 leaves, 190 wide unions
where   shape 485600 core-ms (76%), paint 154790 core-ms (24%), 2.59 us per shape eval
```

Sampling is **76% shape evaluation** and one shape evaluation is **2.59 µs** — roughly seven
thousand cycles — on a field of 3,744 nodes. The reason it is that expensive is in the same line:
**a quarter of the field has no bounding box**, and `Field::build_bounds` gives a node
`everywhere()` when its op is not one it can bound. An unbounded node makes every ancestor
unbounded too, so `Field::eval`'s union sort and `eval_accelerated`'s BVH rejection — both of which
work entirely on boxes — cannot throw that branch away for any point. **190 wide unions have no
hierarchy at all** against 19 that do (`kAccelerateFrom = 12`).

##### THE HISTOGRAM IS TAKEN, AND IT REFUTES THE FIRST HALF OF THIS STEP (D636)

`.\build\bin\WorldShaper.exe --clip-field --clip-file clips/facility.clip` prints it in under a
second — the boxes are decided by the parse, so it does not sample anything. **Read D636 before
doing anything here.** Two lines of it:

```
the shape     2505 of the 3744 nodes, 177 with no box (7%)
of those      scale 87 own, plane 59 own, 12 more values, 19 inherited
```

- **Of the 923, only 177 are under the solid.** A shape evaluation walks the solid's subtree and
  nothing else; the other 746 are patterns and arithmetic that only a paint rule or a named part
  reads. The 25% headline was counting nodes no shape cull ever looks at.
- **Of those 177, 158 are the node's own doing and not one of them is a missing case.** 87 are
  non-uniform `scale`, refused deliberately because such a node under-reports its distance and a
  cull that believed its box would drop a child that was the nearest thing. 59 are `plane`, which
  is a half space and genuinely infinite. The remaining twelve are constants, patterns and
  weathering terms — values, with no extent to bound.
- **The upward poisoning is already fixed.** 54 nodes of the 923 are unbounded *by inheritance*,
  against the 38% the rotate case was written to cure. An intersection keeping a bounded sibling's
  box is doing most of that work.

**So do not write bounds here.** What remained of this step was the second half only —
`kAccelerateFrom = 12`, never measured — and **that is now measured too (D637)**. It is a flag,
`--accelerate-from N`, because two arms are two flags and never two builds (D407).

**Twelve is too low, and both ends of the sweep are worse than the middle.** On one box of the
facility, against a ±1.5% noise band that the sweep establishes on itself (48 and 64 build the
identical accelerators): 4 is +7%, **12 is the baseline**, 16 is −4%, **48 is −6%**, and turning
hierarchies off entirely is +9%. A second box says −13% for 48. So the biggest unions are worth a
hierarchy and the small ones lose to the sorted linear scan the plain path already does — the best
setting builds **two** hierarchies on the facility instead of nineteen.

**The default is still 12, and changing it is the one thing left in step 2.** Every figure above was
taken on a four-core Linux container at 2.6× the development machine's cost per evaluation, and a
cache moves a crossover like this. D637 has the five-minute repeat as one PowerShell loop; **if it
agrees, `kAccelerateFromDefault` in `field.hpp` becomes the measured minimum and step 2 is done.**
It changes no answer — `clips/sampler.clip` measures 1,430,104 voxels at 12 and at 48 alike, and the
headless gate demands every distance to the bit across three thresholds.

D636 also records one live idea and the reason it is not being built yet: a non-uniform scale can be
culled soundly against `box distance × least/most`, which never drops a possible winner and would
recover all 87. It is worth nothing if these unions are not where the time goes, which is what the
`kAccelerateFrom` arm answers first.

**The trap this stage is made of.** A bound that is too small is not slow, it is **wrong** — it
deletes geometry, silently, and the symptom is voxels going missing rather than anything crashing.
Every bound must be conservative in the same sense `box_may_hold_matter` is: `false` is a promise
and `true` is "look and see". D613 is the standing example of what an under-tight box costs, and it
was invisible at 32 voxels a metre and constant at 1.

**What a player sees.** Nothing directly — the world sharpens faster and is identical. **This is an
instrument-and-gate stage**: the acceptance test is the byte-identical gate plus the `us per shape
eval` figure, and if the figure does not move, say so and stop.

**Size it before building it.** ~~Unknown until the histogram exists.~~ **Sized, and it came out at
the small end**: the 923 are almost all ops that genuinely cannot be bounded, so the bounds half is
worth nothing and is not being done. What is left is one measurement — `kAccelerateFrom` — and its
own honest range is *"a hierarchy over four children may buy nothing"*.

---

#### 3. R12 — the field on the card

**What it is.** The only route to a sub-second load, and the plan already names it as R11's
successor (`21-renderer-rewrite.md` §8 R12, and `20-clip-forge.md` §4 for why the field
transliterates). After R11 the CPU round trip — miss, report, sample, paste, upload — is the last
thing between a ray and its geometry.

##### SIZED, before anybody starts it (D639) — and one number contradicts the plan

`--clip-field` ends with the two numbers this stage has never had, and they are CPU-answerable:

- **The facility's field is 41 deep.** `field.hpp` has claimed since R0 that evaluation is *"a
  switch with an explicit stack of at most a handful of entries"* that *"transliterates to a compute
  shader without changing"*. The array does; the evaluation does not. It is recursive, a shader
  cannot recurse, and each stack entry needs a **point** as well as a node — `revolve`, `twist`,
  `bend` and `displace` all ask a child somewhere else. **~656 bytes an invocation** before the
  hierarchy's own stack. The header is corrected.
- **The whole field is 351 KB.** The upload is nothing.
- **An f32 card build cannot be byte-identical — and it is MEASURED now, not estimated (D640).**
  `--eval-f32` builds the clip twice in one run, every node's point and answer rounded to f32, and
  compares it cell by cell. **The geometry survives entirely**: across three arms not one cell
  gained or lost matter. What moves is paint, and only on the clip authored to be a knife edge —
  **15 of `sampler.clip`'s 9,437,184 cells, all material changes**, against **0 of 12,582,912 on
  each of two facility boxes**. D639's estimate of "a few hundred voxels of the facility" was
  pessimistic by the width of the building. Read it as a lower bound: this rounds between nodes
  where a card also rounds inside them. **The equality gate would still fail**, so the choice is
  still the user's — but it is a choice between f64's rate and fifteen cells of colour on a test
  clip, not between that and a different building.

**Why it is third and not first.** It is stage-sized (L), it is the highest-risk piece left, and
steps 1 and 2 both change the number it would be measured against. It also unblocks R2b's unfinished
half, stuck since D259.

**The arithmetic that makes it necessary, so nobody re-derives it.** A hundredfold on the 83.6 s
this all started from is **0.84 s**, which is *less than the part of today's load that is not
sampling at all* (~1.1 s of startup plus the frame time the ladder spans). Sampling itself is 3.8 M
voxels asked against 139 paint rules and a 3,744-node shape field, about 32 core-seconds, so five
workers are a **6.3 s floor**. Steps 1 and 2 attack the 3.7 s and the 2.59 µs respectively; only
moving the evaluation off the CPU attacks the floor. **Say this to the user in these terms rather
than promising a multiple** — they asked for 100× twice, and the honest answer is that steps 1 and 2
are worth roughly 3.7 s and an unknown share of 6.3 s, and step 3 is the rest.

---

### OPEN, and it is the largest thing left: the WORLD SOURCE was never rewritten

**Reported, and every word of it is reproducible:** *"the entire rewritten renderer was meant to also
make the game have no loading time, instead i got a very short 9 million voxel loading time, and when
it loads, it loads with very low detail and i gotta wait for it to load and it loads in chunks, this
contradicts the entire point of pixel screen based loading."* D611.

**The renderer is pixel-driven and the thing that makes its voxels is not.** That sentence is the
whole diagnosis and it is worth reading twice, because four sessions have been spent answering the
marching half of a report about the *making* half — this file's own trap 14, and the
[cold-load memory](../documentation/13-decision-log.md) records two more.

The chain a clip takes to the screen is:

```
clips/*.clip  →  forge::sample(box, voxels_per_metre)  →  voxels  →  World  →  NodePool  →  rays
                 ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^                          ^^^^^^^^^^^^^^^^^
                 fixed box list, two fixed resolutions                     pixel-driven, works
```

Everything right of `World` was rewritten and does what was asked. Everything left of it is what it
was before the rewrite started: **a whole-building sample at a fixed coarse resolution, then the same
building sampled again at the authored resolution in a fixed list of eighteen boxes.** Neither
number has anything to do with pixels, and the box list is planned before the first frame is drawn.

**Reproduced, shelf world `facility.wsworld`, warm shaders, 1280×800, no usable cache** — which is
the state a player is in, because the cache is only written at the ladder's fixed point:

| what the player sees | what it is | measured |
|---|---|---|
| a loading bar counting voxels | the whole building sampled at **metre 8** and inflated 4× on paste | `sample 2381 ms`, 2,363,352 asked + 8,596,115 settled in bulk; the bar's own total is the coarse grid |
| ...and then it is playable | | **ready at t+3,615 ms** |
| the building is blocky | `--clip-coarse 4` is a quarter of the authored 32 voxels/m | `coarse build: sampling at metre 8 and scaling 4x on paste` |
| detail arrives in chunks | **eighteen** pre-planned boxes, each sampled whole at metre 32 and pasted in one go | `18 regions to sharpen, biggest on screen first`; per region 624–7,406 ms of sampling, **8 of 18 done by frame 900** |
| ...and each is a hard step | two rungs, 8 → 32, with nothing between | — |

**Three things that are NOT the cause, each already measured, so nobody spends a session on them
again.** The marcher, residency and the face store are all pixel-driven and working (R1h, R2, D427).
The hiccups are closed (D511–D514): a region paste is 24–92 ms and blocks nothing. And the
*cached* path proves the ladder is the whole of the symptom — `clips/facility.clip` with its
608 MB cache complete loads in **804 ms** to `no ladder, the world is at the detail the clip asked
for`. Which is also the second half of the fault: that path is a **608 MB eager load of the whole
world**, so even when nothing pops in, nothing about it is pixel-driven either.

**The plan already named the answer and its blocker is gone.** §8.0 of `21-renderer-rewrite.md`, in
the `the cold load, measured` row, written before R1e: *"half a second with a sharp first frame means
nothing is sampled up front at all, which is **R8c** (`forge/field.cpp` already answers at any
resolution) with R1e removing the addressing that keeps a chunk world necessary."* **R1e landed on
2026-08-11** (D521–D525). The prerequisite is paid; R8c is now the open work, and it is what the
third of the user's three original asks actually requires.

**What R8c is, concretely, and why every piece of it already exists.** `forge::sample` takes a box in
metres and a `voxels_per_metre` and answers — that is its whole signature, and the ladder already
calls it per box off the main thread and pastes the result with `paste_clip(..., coarse)`, which
inflates a coarse sample into real voxels. The node pool already knows which node a ray wanted and at
which level, and `request(key, source)` is the one route by which anything is ever built. So the
change is not new machinery, it is **who chooses the box and the resolution**:

- today: a list of eighteen boxes planned at load, each at the authored resolution;
- R8c: the box is **a node the marcher asked for** and the resolution is **the one that node's level
  implies** — 256/2^level voxels per metre, which is 32 at the level-3 leaf and 4 at a two-metre
  node. A node's level is already a function of its pixel footprint, so resolution becomes a function
  of pixel coverage by construction rather than by a second rule that could disagree.

**The plan now has a stage for it. `21-renderer-rewrite.md` §8 R11 — the world source, driven by
pixels · XL — is the next stage, with R12 (the field on the card) after it.** Read the stage there
rather than re-deriving it here; what follows is only the shape of it and the two traps that decide
the order:

| | what it is | what a player sees |
|---|---|---|
| **R11a** | ~~one node, sampled, **timed**~~ — **DONE** (D613). `--sample-cost` and `tools/samplecost.ps1`; the numbers are below | nothing. It is a measurement |
| **R11b** | ~~the unit of refinement is a node, not a region~~ -- **DONE** (D615): seeded at four metres, split to one as you get near | detail stops arriving as slabs |
| **R11c** | ~~resolution is `256 / 2^level`~~ -- **DONE** (D616): and the split threshold became eight voxels at a pixel each | the 8 -> 32 jump becomes 8 -> 16 -> 32, following you |
| **R11d** | nothing is sampled up front. **The headline** | no loading bar at all |
| **R11e** | a light path may not cause sampling — R9h one level down | nothing, until it is missing |
| **R11f** | a world is a clip plus its edits. **R8d, and the only sub-step that can lose data** | a `.world` stops being 608 MB |
| **R11g** | `--settle` and the harness mean what they say | nothing. It protects every figure in this file |
| **R11h** | an edit is served at full detail whatever has been looked at | a chisel at sixty metres cuts properly |

**Do a before b, and b and c before d.** a is the instrument three later trades are against. d without
b and c under it is spawning into an empty room that fills with big blocky boxes, which is worse than
what is being fixed. f is last because it is the one that can lose somebody's building.

#### R11a is done, and here is what it says — start at R11b

`tools\samplecost.ps1` runs it in **18 seconds** and writes `documentation/baselines/r11a-sample-cost.csv`.
Facility, eight workers, RelWithDebInfo. D613.

The figures below are **after D614's plan split**, which is what R11b will be building on. The
`empty node` column is the one that moved: it was 0.213 ms at the leaf and 0.418 at level 4.

| level | node | voxels/m | nodes with matter | one node | median | empty node | asking one at a time costs |
|---|---|---|---|---|---|---|---|
| 3 | 0.25 m | 32 | 269,337 | **1.216 ms** | 0.513 | **0.017 ms** | **7.8×** |
| 4 | 0.50 m | 16 | 42,062 | 2.016 | 1.029 | 0.260 | 6.1× |
| 5 | 1.00 m | 8 | 7,558 | 2.841 | 2.910 | 0.446 | 4.6× |
| 6 | 2.00 m | 4 | 1,464 | 2.203 | 1.161 | 0.153 | 2.4× |
| 7 | 4.00 m | 2 | 330 | 1.964 | 0.997 | 0.386 | 0.9× |
| 8 | 8.00 m | 1 | 72 | 2.396 | 1.283 | 0.041 | 1.1× |

**Compare like with like when you re-run it.** Two runs of one build over one building came out
13% and 25% apart when one asked for 8 nodes a box and the other 24: the worst node at every level
is ten times the median, so the mean follows which nodes the stride landed on. Same `-Nodes`, same
`-Boxes`, or the comparison is about the sampling rather than about the build.

**Three things R11b had to know, and what R11b did with them (D615).**

1. ~~**The fixed cost of a sample is the PAINT RULES, not the box.**~~ **Found, and FIXED — D614.**
   It was 0.213 ms an empty node on the facility's 139 rules against 0.012 on a clip with four,
   because `sample()` re-derived every rule's slack, box and pieces on every call. It is now
   `forge::plan_sample` once and `sample(plan, settings)` many times: **an empty node at the leaf
   is 0.014 ms against 0.211, a node with matter 1.19 against 1.43, and asking one node at a time
   costs 7.5× a single call rather than 21.9×.** `--sample-cost-replan` is the control arm. What
   is left of a node's price is the descent from the root of a field describing a whole building;
   batching siblings is the next lever and it has not been measured.
2. **The job pool is not worth waking for one node**: 1.389 ms threaded against 1.391 serial, at
   every level, to three digits. Eight voxels a side is eight z slabs.
3. **Despeckle is a whole-clip judgement and will not survive being asked per node.** 29 of 297
   nodes come out different when the pass runs on the node rather than on the box around it,
   because it decides what is a deliberate stipple from a material's share of the clip's whole
   surface, and a node is 512 cells. Measured and deliberately not fixed — it is R11b's.

#### R11b is done too -- start at R11c

The eighteen boxes are gone. The unit is a node of the render tree, seeded at four metres, and it
**splits into eight when it is more than a quarter of its own distance across** -- the projected
size the ladder already ordered its work by, so grain and order are one rule rather than two.
Inside eight metres of the camera the world arrives a metre at a time.

**What is proved, and where.** On `clips/sampler.clip` with `--refine-all`, the node ladder and the
eighteen-box ladder build **byte-identical worlds** with despeckling off (`a1f8bc6c656343b7`,
1,430,104 solid voxels in both) and identical geometry with it on. **The facility A/B was not run**
-- it is about ten minutes a side -- so the gate is met on a small clip and the facility is owed.

**Three things R11c inherits.**

1. **The four-metre ceiling is memory and R11c removes it.** A sample allocates five bytes a cell
   up front: four metres at the authored resolution is 2 M cells and 10 MB, eight metres would be
   84 MB, and the old twelve-metre unit was 283 MB every time. Once the resolution follows the
   level a node is 512 cells however many metres it spans, and the ladder can seed at level 8 or
   above.
2. **A node is skipped only when the world AND the field agree nothing would change**
   (`any_matter_in` and `forge::box_may_hold_matter`). Asking the world alone lost 4,923 voxels of
   1.43 million on `sampler.clip`, because a feature the coarse pass was too blunt to see is a
   feature the skip makes permanent. Keep both halves.
3. **Despeckling is still the residual, and the skirt that would fix it is reverted.** Sampling a
   node with a one-voxel margin, despeckling, and cropping the margin off is exactly right in
   principle -- and it lost **240 voxels of 1.43 million**, because a box one voxel larger on every
   side is not the same question as an aligned one. `--sample-cost` reproduces it: **2 of 96 nodes
   differ on `sampler.clip`, 0 of 297 on the facility**. D613's class one step along. What is left
   without the skirt is 46 cells of 152,064 on the facility wearing a neighbour's colour.

#### THE LOAD IS 4.8× SHORTER, and the instrument that did it is the thing to keep (D622)

**Read D622 before touching the ladder.** Nothing here had ever measured where a load's seconds go —
the batch line times one batch, and a batch is tens of milliseconds. A run-total line beside the
settle line, split by which half of the ladder spent the time, named three faults in its first run.
All three were **waiting**, not cost:

1. **the batch was sampled one node at a time on one core** — the loop was serial and each
   `forge::sample` was handed the pool to split *itself* across, which R11a had already measured as
   worth nothing (1.389 ms threaded against 1.391 serial: a node is eight z slabs). One node per
   worker instead: **33,912 → 7,691 ms**;
2. **7.1 million occlusion raycasts to choose sixteen nodes** — the picker made two full sweeps of a
   40,436-entry list, and the first cast a ray for every node beating the front runner. A node the
   ray *refuses* does not become the front runner, so `keenest` never rose and every one of the 6,042
   permanently occluded nodes fired a ray on nearly every wake. One cheap sweep, no ray in it, the
   expensive tests paid only by the shortlist: **25,996 → 1,736 ms, rays 7,103,492 → 149,512**;
3. **the ladder slept four fifths of the load** — batch 16 was sized in D617 for a serial sampler, so
   a wake cost 4 ms of a 22 ms frame and `pump_refinement` runs once a frame: 1,730 wakes over 1,716
   frames. Batch **16 → 128**.

**Wall clock, cold facility, enclosed camera: 83.6 s → 17.3 s.** Frames to settle 3,268 → 701, and
the frame *during* the load 24.1 → 17.7 ms, so it is smoother as well as shorter. The acceptance
test is not the timing: `clips/sampler.clip --refine-all --no-despeckle` returns
**`a1f8bc6c656343b7`, 1,430,104 voxels**, byte for byte D615/D616's reference. All three changes are
scheduling.

**Not carried, and measured:** more than half the machine for the sampler. At 12 workers of 10,
sampling drops 6,361 → 4,509 ms and **the paste goes 140 → 1,607 ms** — D511–D514 from the other
side, the background pool starving the foreground one.

**The double buffer was then built and REVERTED — read D623 before proposing it again.** It works
and is worth 9% (ladder 12,421 → 10,961 ms, wall 17.3 → 15.7 s), and it **loses 606 voxels**: the
pick asks `refine_node_is_a_no_op`, which asks the WORLD whether there is anything to replace, and a
node picked while a batch is in flight is judged against a world that batch has not been pasted into
yet — so a node whose matter is about to arrive reads as empty and is marked `done` for good. R11b's
gate caught it (`1,429,498` against `1,430,104`). **The world is only an honest witness when nothing
is in flight over it**, and making the skip test consult the queue as well as the world is the price
of that 9%. Delivering every landed batch rather than one a frame was worth a further 4% and made
the settled world depend on frame timing (two runs, two hashes) — refused for trap 8/19.

**The half that WAS kept lost the same 606 voxels too, and shipped — D624.** `enlist` marks a node
`done` when it is **picked**, not when it lands (it must, or the next pick samples it twice), so
`left == 0` in `deliver_refinement` reads true with a batch still out at the sampler — and the pick
sits directly above that count, so an outstanding batch there is the common case. D622 got away with
it because its teardown never stopped the worker, so the dropped batch landed anyway into a reset
plan; that accident is why the old build printed *"fully sharpened"* twice. Fixed by testing
`left == 0 && !refine_busy()`. **D623 checked the FACILITY hash and called the gate restored — the
facility never reaches the fixed point, so that hash could not have moved whatever the change did.
Check the arm that can fail: the gate is the clip small enough to finish.**

**So the honest ceiling, and the arithmetic of why 100× is not on this road.** The load is 17.1 s:
about **1.1 s** of startup, **3.7 s** of up-front coarse build (sample 2,754 + paste 257 + compact
702 — **R11d** removes all of it), and **12.4 s** of ladder, of which **6.3 s is sampling**, 0.8 s
picking, 0.14 s pasting and ~5 s frame time. A hundredfold would be 0.84 s, which is less than the
part that is not sampling at all.

And the sampling itself is now a measured constant rather than a mystery. `--clip-file
clips/facility.clip` prints it: **2.59 µs per shape evaluation** over a **3,744-node field**, with
**923 nodes (25%) carrying no box** — so no ancestor of one can be culled — and **190 wide unions
with no hierarchy** against 19 that have one, over 479 leaves. Shape is **76%** of sampling and paint
24%. That 2.59 µs, and the 25% of the field that cannot be bounded, is where the next large multiple
lives; it is a field problem, not a ladder problem, and **R12 (the field on the card)** is the stage
for it. This is the first hard number the plan has for why R12 exists.

#### HALF CLOSED: "huge brick blocks on top of things" — D620 bounded it, D621 closed one half

**Read D620 then D621.** D620 was right about the shape and it came from one sentence the player
said in passing: the chisel's aim cursor does not detect the lumps. That cursor is a CPU raycast
against `World`, so **the lumps are not voxels** — they are the render tree drawing a stand-in for
a node whose leaf it has not got. §4b's own words for this shape are *"the descent said
unbuilt-but-occupied, occlusion reads that as opaque"*, and trap 7 is the rule it breaks.

There are **two populations**, and they had been read as one for four entries.

**The ones that survive a settle: CLOSED (D621).** `paste_clip` writes whole bricks through
`brick_for_write().assign()`, a refinement paste REPLACES, and a Replace write that takes the last
voxel out of a brick left it allocated. `world_has` asks whether a brick is *allocated*, so the
render tree was told the world holds matter it does not; `build_leaf` correctly refuses to build a
node with nothing in it, so the cell stayed unbuilt-but-occupied for ever. `drop_brick_if_empty`'s
own header has said since D357 that a bulk writer through `brick_for_write` has to say so, and
`op.cpp` was taught to; the paste never was. Measured at the fixed point, enclosed camera: **304
allocated bricks holding nothing → 0**, sun faces shadowed by a cell the pool has not built
**12,517 → 113**, gathering rays stopped by one **8.2% → 0.0%**, fireflies **108 → 0**, 20% of
pixels changed, and the world byte-identical (same content hash, same 125,419,666 voxels).
`--no-paste-drop` is the control arm.

**And the reason no audit caught it, which is the reusable half**: `NodeBuffers::audit` compares the
card with the pool, and `stale_leaves`/`stale_masks` compare the pool with `world_has`. **`world_has`
was the liar**, so all three agreed perfectly with 304 lumps on screen. See trap 26.

**The ones a player watches during a load: STILL OPEN, and this is where to start.** Both arms are
the same picture at frame 600 and the control holds *nought* empty bricks there, so it is not the
above. D620 said to read `deferred` and `out_of_room_` during a load and nobody had; read now, with
`no_room` counted at all four of the pool's allocation choke points rather than at one:

| frame | leaves held | NO ROOM this frame | deferred | evicted |
|---|---|---|---|---|
| 300 | **262,144 — the ceiling** | **252** | 0 | 0 |
| 600 | **262,144 — the ceiling** | **338** | 0 | 0 |
| 1200 | 33,282 | 0 | 0 | 0 |

The pool sits pinned at `max_occupancy_leaves` for the whole load, refuses hundreds of builds a
frame, evicts nothing, and reported `deferred 0` — with every refusal counted as `built`, because
`refine`'s caller increments `built` whatever `refine` managed and `out_of_memory` was set in one
place only. **The load's peak leaf demand is eight times the settled demand and why is not
diagnosed.** That is the next question: not "raise the ceiling" but *what is holding a quarter of a
million leaves during a load that thirty-three thousand serve afterwards*, and why the erode sweep
gives up nothing while it happens. Trap 20 applies to any fix here — a pass that gets cheaper by
building less is a regression in improvement's clothes.

Everything below this line, and D617 through D619, is work on the thing that MAKES voxels. None of
it could have fixed either population, and D619's starvation fix was a real fault that is not this
one.

#### Was closed and was not: "huge brick blocks on top of things" -- the starvation half (D619)

Reported with a photograph of an urn standing as a slab of coarse cubes in a niche whose walls were
already sharp, and reported again after D617. **Read D619**, and read D617 and D618 only for how the
diagnosis went wrong, because it did.

**This section used to say the blocks were the up-front coarse build and that R11d removed the
cause. That was wrong.** The blocks were the refinement ladder never getting to those nodes.

The evidence is a **census** printed beside the settle line: a level histogram, plus which of the
picker's three tests refuses each node left coarse. The column that matters is **"neither"** -- a
node that passes every visibility test and simply never got into a batch. At a fixed point there is
no honest reason for it to be above zero. It was **721**.

The cause is that the picker's three tests have different tenures. A node with nothing in it, or
already at its finest, is marked `done` for good. A node behind the camera is demoted. But a node
**behind something** is refused by an occlusion ray and **not marked at all** -- correctly, because
the camera will move. So it comes back every frame, and the nodes behind a wall are big and near,
which is exactly what the rank rewards, so they sat permanently at the head of a shortlist of
sixty-four and crowded out everything that could actually be sampled. The batch of sixteen was
delivering **1.22 nodes**, and exactly **1.00** by the end.

Fixed by two changes: the shortlist went 64 -> 512 with the facing demotion folded into the cheap
sweep, and an occlusion refusal is now **remembered** for `kRefuseFor = 32` wakes instead of being
rediscovered every frame. Batch mean **1.22 -> 15.26**, nodes sharpened **10,486 -> 32,680**,
"neither" **721 -> 0**, and the run reaches a fixed point instead of timing out. The urns have lids
and handles again.

**Two traps this left behind, and both are load-bearing:**

- **`--settle` never settled.** It was hitting `kSettleGiveUp` (180 s) on every run, which is why
  the wall clock was a suspiciously constant 181.3-181.6 s across four arms. **That voids the
  one-flag test of D618** -- it compared two timeouts, not two fixed points. Any measurement taken
  before D619 that relied on `--settle` reaching a fixed point should be re-read with that in mind.
- **A GPU mean is only comparable to another GPU mean over the same window.** An unsettled run
  averages the whole 180 s including refinement churn; a settled one averages the frames after its
  fixed point. `30.350 ms over 5,849 frames` against `6.801 ms over 30 frames` is not a speed-up
  and must not be quoted as one.

R11d is still owed and is still the headline -- but it is about **what is drawn before the ladder
gets there**, which is what it always should have been, and not about this bug.

#### R11c is done as well -- start at R11d

A node is now sampled at `256 / 2^level`, capped at the clip's authored resolution, and the split
threshold is **eight voxels at a pixel each** (`8 x 0.002`) rather than R11b's quarter of the
distance -- with the resolution following the level, a quarter is a node holding voxels sixteen
pixels wide. Settled, every node holds voxels about a pixel across: 32 a metre within sixteen
metres, 8 a metre at sixty, and nothing anywhere sampled at a detail the screen cannot show.

**Proved the same way b was**: forced to full detail on `clips/sampler.clip`, the level ladder
builds a **byte-identical world** (`a1f8bc6c656343b7`, 1,430,104 voxels) through **9,819 samples at
six different resolutions**. That is the check that the resolution ladder composes -- coarse
sample, blown up, then replaced by finer children, ends where one full-detail sample ends.
Facility, default camera: 120 eight-metre seeds become **30,898 nodes**, 9,272 sampled, 14,096 left
coarse behind walls, **worst paste 12 ms**.

**Four things R11d inherits.**

1. **The up-front coarse build is now the floor under everything.** Every node knows what the world
   already holds where it is (`applied_per_metre`, seeded from `--clip-coarse`), and only samples
   when its own level beats it. Remove the up-front build and that floor becomes nothing, which is
   exactly what R11d wants -- but the first frame then has no world at all until the first nodes
   land, so R11d is about what is drawn in the meantime.
2. **A node too coarse to improve must SPLIT, not finish.** The first version marked it done and the
   ladder refined four nodes and declared itself settled. `refine_would_improve` is that question.
3. **The split loop follows the winner down.** Re-picking from the whole list after each split lands
   on some other unsplit node, still large and still keen, so the list is cut finer everywhere and
   nothing is ever sampled.
4. ~~**Only boxes sharp at the authored resolution are written to the cache**, because
   `CachedRegion` has nowhere to record which detail a box holds.~~ **Fixed — D626.** Every leaf is
   written with its key, level and `applied_per_metre`, and the resuming run rebuilds the octree from
   the keys. Writing only the authored-detail boxes did not merely lose coarse work: from a camera
   where no leaf reaches authored detail it wrote an *empty* list, which the format defines as "not
   built through the ladder at all", and the world froze at sixteen voxels a metre for every launch
   thereafter.

**The fly-in gate is still owed**: 60 m to 1 m at the facade with no consecutive-frame spike. What
is proved is the world, not the walk.

**And the fault the gate found, because the shape of it will recur.** The agreement check failed
before it passed: the same node came out **differently** sampled alone and sampled inside the
building, at one and two voxels a metre — 17 of 32 nodes at level 8, 108 cells in all. Neither arm
was right. A box is settled empty in bulk when the field at its centre is further out than anything
inside it could be near, and "near" for a leaf voxel is not nought: the thin-feature rescue keeps a
cell whose centre is outside by up to **half a cell diagonal**. The box test never allowed for it.
At 32 voxels a metre that is 2.7 cm and it almost never bites; at one voxel a metre it is 87 cm.
**Every test in `test_sample.cpp` ran at 32 voxels a metre**, so fifteen months of agreement with
the brute-force definition said nothing about the resolutions R11 is built out of. There are now
subcases at 1, 2, 4, 8 and 16, and a second test that asks a small box and a big one to agree.

**Two hazards to size before starting, both real, and both are sub-steps rather than notes.**

- **Every measurement in this repository is against a content hash and `--settle`** (trap 8, trap
  19). A world that only samples what a camera asked for has a *camera-dependent* content hash by
  construction. `--settle` already means "refinement has nothing left it can do from here" and
  generalises, but `baseline.ps1`'s gate does not, and it has been silently broken by a weaker
  version of this once already (D524). **R11g.**
- **What is saved.** A clip-backed world stops being a 608 MB voxel dump and becomes a clip plus the
  edits made to it — but `save_refined_world`, `world_cache.*` and `CachedRegion` are all written
  against the eighteen-box ladder today, and after it a chisel on a surface nobody has looked at
  closely would carve a blocky approximation that the file then treats as authoritative. **R11f and
  R11h, and they are the two to be most careful with.**

**And the process lesson, which is worth more than the stage.** The mechanism was in the plan from
the day it was written (R8c, 2026-08-09) and the sentence connecting it to loading was added on
2026-08-11, in §8.0's `the cold load, measured` row, in answer to an earlier version of this same
report. Both were true and neither was ever scheduled, because R8c lived inside §7 — an experimental
mode, off by default, last in the order. **A correct answer filed under "measured, not a fault" is
not a plan.** When something the user is complaining about turns out to be already understood, the
question to ask is not *is this known* but *is there a lettered sub-step whose gate would fail if it
were still true* — and here there was not.

### Closed: the paste, which is what a player actually felt

**Both halves of this are now done, and the second one was not what this section spent two sessions
saying it was.** It is left here in full rather than deleted, because the reasoning that was wrong
is the reusable part — see trap 17, which is this section's whole lesson in one paragraph.

What it said: the scene sharpens region by region; the sampling runs on a background thread, the
**paste does not**, and a paste measured **twelve to fourteen seconds** with the main thread
blocked. That is what the in-game overlay reported as a 2,234 ms 99th percentile and a 6,282 ms
worst frame while the GPU sat at 0.92 ms. All true. Both marchers suffered it identically — swap
with F6 — and it predates the rewrite entirely.

What it was: **the paste waiting for the background sampler**, because the two shared a job pool.
Not the paste. D511–D514, and item 2 below.

Two fixes, in the order they paid:

1. ~~**Write the cache when sharpening settles**, not only when it completes.~~ **Done** — D241–D246.
   The cache is written at the fixed point carrying a `CachedRegion` per leaf saying how sharp it got
   (since D626 that is the whole leaf set with its keys and detail; it was once only the sharp boxes),
   and a later run from another camera carries on from it. Default camera, `--settle`: first run
   133.3 s, every run after 6.6 s; two runs from different cameras finish the building and every run
   after that loads a complete world in five to seven seconds. **Read D243 and D244 before comparing
   any figure across this change** — the `scene:` line now carries the world's content hash, and
   figures taken before it are not comparable with figures taken after it.
2. ~~**Slice the paste across frames.**~~ **The stall is closed, and slicing was not what closed
   it** — D511–D514. The paste was never doing the work. Read that block before touching any of
   this, because the shape of the mistake is more useful than the fix:

   The one figure this section quoted covered three things — `paste_clip`, the op-log replay and
   `announce_world_change`. Split, the replay is **0 ms** and the announcement **0–2 ms** in every
   region, so all of it was `paste_clip`. But paste time did not track the *paste*: the same
   991-brick region went in **146 ms and in 7,076**, and 5,359 bricks went in 80 ms while 4,258
   took 1,453. Nothing about the output explains 47× on identical output.

   It tracked the **sample running beside it**, row for row, and the only region with no sample
   beside it pasted in **75 ms**. `JobSystem::parallel_for` queues a take-**loop** over the whole
   range rather than a slice of it, so a worker that picks one up is inside it until that
   submitter's range is exhausted. `pump_refinement` starts the next sample before pasting — on
   purpose, it nearly halves the ladder — and handed both the sampler and the paste `refine_jobs_`.
   Every worker was therefore inside the sample for its whole length, the paste's entries sat
   behind them, and `wait()`, which helps with queued work so a waiting thread is never idle, gave
   the **main thread** the sampler's jobs to run.

   Foreground and background now have separate pools. Cold facility, two flags of one build,
   `--no-paste-pool` the control: worst paste **7,282 → 92 ms**, the twelve pastes of one load
   **34,697 → 719 ms**, frames drawn before the world settles **453 → 5,439**, the sample beside it
   +1–3%, and both arms settle on the same content hash `1f4710eee4ee2585`.

   **What is left of the slicing, and why it is not next.** 31–92 ms — two to six frames, which
   nobody has reported. It is a large change whose hazard is real (a half-pasted world is visible
   to `save_refined_world`, to `--settle`, and therefore to every measurement in this file) and it
   was sized against a premise that has since moved by 79×. Do not build it without measuring it
   again first.

   **A large chisel was a different fault, and most of it is now closed too** (D515, D516). It was
   the same shape one level down: `announce_world_change` named **every brick** in the edited box
   and the pool walked up from each one. On the 36-million-voxel delete that is **1,573,269 bricks
   announced**, producing **13,325 refolds and no rebuilt leaves**, at **718 ms in a single frame**
   — gather 457, descend 257, fold **4**. The pool holds the tree, so it now takes the **box** and
   descends from its own roots, pruning any child whose extent misses it, post-order so the fold
   ordering falls out by construction instead of being sorted for afterwards. **718 → 7 ms**, and
   the edit frame is no longer the worst frame in the run. A one-voxel chisel is unchanged.

   Do not touch that code without reading `NodePool::stale_masks` first. It is the audit that had
   to be built before the change could be believed: a **child mask** decides where a ray is allowed
   to look, and it is invisible to every other check here — the GPU mirror compares the pool against
   the card and both agree about a bit that is wrong in each, and `stale_leaves` compares contents
   rather than reachability. The screenshot audit now prints *the node pool agrees with the world,
   mask for mask*.

   **What is left of that frame is the undo capture, and it is now the largest part**: apply 62 ms,
   undo capture **194 ms into 538,169 inverse ops**, pool refresh 7. An inverse op per changed run,
   captured synchronously, for an edit nobody may ever undo. *That* is what D74's Stage 16 argument
   is actually about now, and it can be reproduced with a flag rather than by watching a load.

**So a first load is now the renderer too.** What is left of it is the *sampling* — 7 s a region on
a background thread — which is not a stall and does not block a frame.

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

### The open one: ambient occlusion is grainy face to face, and R5 is the answer

Reported as *"as if the sub-voxel smoothness of the shadowing of the faces was misaligned or
rotated instead of being smooth."* One real bug came out of it and is fixed (D381): an ambient ray
grazing its own wall was counted as contact, so a flat facade read 129–246 of 255 in blocks. It now
has to rise clear of the face's own plane, and the banding goes.

**What is left is variance and it is at the noise floor.** Face-to-face roughness on a flat wall —
the mean second difference along a scanline, which removes any smooth trend and leaves the steps —
is 2.5/255, against the renderer's own run-to-run noise of 2.50/255. Two suspects were eliminated
on the way, both worth not re-checking: the R10c gradient (0.529/255 in a dark room) and coarse
stand-in faces (49,108 level-0 against 1,301 level-3 at the camera in question).

So this is not a fault to find; it is a couple of hundred Monte Carlo samples per face, made
visible because a dark interior is lit by this term alone and the exposure pushes it hard. **R5,
the face denoise, is the stage that owes it** — filtering across neighbouring faces is exactly its
shape, and it is the first thing in the plan that pays here. Measure with `--debug-mode 18`, which
is the near field on its own, and with the roughness figure rather than a standard deviation: a
plain sd counts the genuine falloff under a soffit as though it were noise. D381, D382.

**Closed, and R5 is no longer what it needs.** D383–D387 took the variance out of each sample
(roughness 8.975 → 1.881 of 255) and D388–D396 took the *waiting* out: a face now takes sixteen
ambient samples a frame instead of one every `face_stride` frames, reaches its 2,048 and **stops
casting rays altogether**. Read that block of the decision log before touching this pass, because
four of the nine entries are things that were built, measured and removed — most importantly
**every attempt to meter the burst made the transient worse** (D394), for a reason that generalises
to anything else in this pass: what it spends on an unconverged face is mostly the face, not the
ray, so the cheapest thing to do with a face that has to measure is to let it measure all at once
and be finished with it.

The instruments are `--debug-mode 19` (green converged and silent, red held short of it by unbuilt
geometry, grey the progress between) and the audit line `ambient on the card: N of M live faces cast
no more rays at all`. **Use them before believing a cost figure here**, because a converged face and
a face one sample short look identical in every shaded view and only one of them is being paid for
— which is what sent the first three attempts at this looking in the wrong place.

### Closed: the world sharpened and the renderer was never told

**The premise R10d rests on — "the host says on the exact frame when geometry changes" — was true of
edits and false of everything else.** The clip ladder pastes each sharpened region straight into the
world's bricks in `pump_refinement`, and that path announced nothing. Two things had been quietly
wrong on every load since the ladder existed, and neither could be seen from a screenshot:

- the node pool held **7,497 of its 17,344 leaves** in the shape the world had before the paste — a
  leaf is a copy taken at build time and eviction is only under memory pressure (D247);
- the ambient term kept the occlusion it had measured through them **for ever**, because since D389
  a converged face stops casting rays and only `edit_min.w == 2` reopens it.

Measured on the same world at the same camera with the same content hash, one run watching it
sharpen against one loading it whole: the near field **19.10 of 255 over 547,411 pixels → 2.43 over
42,096**, where two runs of one camera differ by about 1.5 and 14,000. The fix is that the paste
makes the same announcement an edit makes (`announce_world_change`, which is
`invalidate_edited_chunks` with the ops taken out of it). D397–D400.

**The instrument is the part to keep.** `NodePool::stale_leaves` compares every built leaf's
occupancy against the world's, and the screenshot audit prints either the count with the first
offending brick's coordinate or *the node pool agrees with the world, leaf for leaf*. Nothing was
asking that question: `node_buffers_.audit` asks whether the card agrees with the pool, and both
agree perfectly about a brick neither has looked at since the world rewrote it. **Anything else that
writes to the world without going through an op has this bug today** — that is the class, and this
line is what makes the next one loud instead of silent.

**Open, measured, and deliberately not fixed here:** a face held short of convergence by unbuilt
geometry is **6.0% of surface pixels on a settled world** (50,578 of 848,622 at `--debug-mode 19`),
and when the hold releases it freezes a mean carrying 2,047 samples taken through the shell. The
error that carries has not been measured — nothing counts ambient ignorance by level the way the
sun's audit line does, and that instrument is the first thing to build. The two obvious fixes are
both known-bad without it: collapsing the history on a clean ray oscillates for ever on a face that
permanently borders unbuilt geometry (D394's table is about exactly that state), and dropping the
tainted samples makes the face wrong-bright, which R10e forbids.

### Closed: the light pass while moving, and while moving AND editing

**This was the biggest number in the renderer and it is now 2.1× smaller flying and 5.9× smaller
while flying and chiselling.** D413–D420. The section below records how it was measured and what it
was; read it before touching the pass, and read D417 and D418 before believing any theory about
where the time goes, because two of the three causes the previous session ranked measured as nothing.

**The case that mattered was not the one being measured.** The player named it: *"the test of flying
while continuously editing by erasing and placing, that's the true worst scenario which causes a lot
of lag"*. It is now an instrument — `--chisel EVERY,RADIUS` — and it was worth every minute:

| | face pass | total GPU | CPU |
|---|---|---|---|
| flying | 10.43 → **4.94 ms** | 16.95 → **12.26** | 18.96 → 16.27 |
| flying **and chiselling** | 48.57 → **8.30 ms** | 55.52 → **15.02** | 57.13 → 23.45 |

Eighteen frames a second to sixty-seven, on the GPU, in the case a player is actually in. The
settled picture does not move: on-against-off is 2.836 / 1.286 / 0.077 of 255 at the enclosed, close
and outdoor cameras against a run-to-run floor of 2.728 / 1.161 / 0.096, with speckle identical to
two decimals. 467 tests, 18.0 M assertions, passing.

**Three changes, and the first is the one that matters.**

1. **Light stops at what a pixel has read** (D414). The store keeps a face for six hundred frames
   after anything last asked for it, and every one of them was casting its full burst: 763,800 live
   faces against a frame made of 218,000. `node_visibility.comp` resolves a face for *every* pixel,
   so it stamps the slot it read into a card-owned array (`face_seen`, binding 15); `shade_faces`
   skips a face no pixel has read for `seen_window` frames. **Residency is untouched** — the face
   keeps its slot and its accumulated answer, so the composite reads what it always read.
2. **An edit reaches every face; only a face somebody is looking at re-measures now** (D415). The
   reset still runs everywhere inside `kEditShadowReach`, because a wall deleted behind your back
   must not keep its shadow; the rays wait. Worst frame 75.0 → 19.8 ms.
3. **The dispatch is sized by the work** (D416). `face_worklist.comp` packs the slots that owe
   anything and the shading pass runs indirectly over the count, so no workgroup is eighteen lanes
   busy out of sixty-four. 0.096 ms for itself.

**Two rules before touching any of it.** Both levers are **run-time flags**, so the arms of an A/B
are one build and D407's warning is satisfied by construction: `--no-face-gate`,
`--no-face-worklist`, `--no-face-prolong`. And `face_work_of` in `node.glsl` is the single place
that decides whether a face owes work — the worklist pass and the shading pass both call it, and if
they ever disagree in the direction of the worklist being stricter, faces stop being lit and nothing
says so.

**What is left, in the order it looks worth doing**: sorting the work list by Morton code so the
sixty-four lanes of a workgroup walk one neighbourhood of the tree instead of sixty-four (it cannot
change a pixel — it only reorders the same invocations — and nothing has measured it); R5's face
denoise, which is what would let `kSkyConverged` fall from 2,048 and was measured as worth 5.05 →
3.55 ms at 512. `rebuild_coarse_grids` was the third item and is **deleted** (R1e, D522): it was
O(world) for a change one metre across and by a wide margin the largest thing an edit cost, at
**3.86–4.14 ms** against the op's own 0.24. The same run now prints **apply and undo 0.24 ms, world
bounds 0.00, invalidation downstream 0.00**. What is left in an edit is the undo capture.

### How the moving case was measured, and why the grid could not see it

**The user asked for the lights to be much faster and for more frames *while moving*.**
There was no measurement of that case at all — every figure in this file above this line is settled,
and `--settle` discards transients by construction, which is what it is for. D410–D412.

`tools\_flybench.ps1` is the instrument. It drives `--fly` along a fixed path at the fixed 1/60 step
and prints the pass table:

```powershell
.\tools\_flybench.ps1 -Tag mine -Rounds 3
```

**Close camera, `--fly 0,0,3,15`, 2560×1440, quality 7, `--settle`, 200 measured frames:**

| pass | mean ms | worst | budget | same camera, standing still |
|---|---|---|---|---|
| visibility | 3.30 | 5.8 | 9.50 | 1.09 |
| **faces (the light)** | **11.75** | 17.4 | **4.40** | **1.11** |
| resolve | 2.78 | 4.7 | 0.80 | 0.89 |
| **total GPU** | **18.6** | 24.6 | — | 3.35 |

Three runs give 11.588 / 12.090 / 11.581, so the spread is 4%, and the convergence state is
reproducible to 0.1% — 280,551 / 280,887 / 280,695 faces still bursting. **That is what makes this
case usable as a gate**, and it is the thing D407 says to check before reading any time from this
pass.

So the light pass was **63% of a moving frame and ten times its own settled cost**, and §6 of the plan
— "lighting stops scaling with resolution" — was only ever measured on the standing-still half.

**The cause that was named there was not the cause.** D412 ranked `node_walk_reset()` first — 6.3 M
rays a frame each paying an eleven-level descent from the 512 m root before their first step, on
rays bounded at one metre. It was removed, it is bit-identical, and it measured **11.85 against
11.77**, inside the spread (D417). The descent is cache-resident and instructions are not what this
pass is short of. It is kept because a removed load cannot become a cost, and it is recorded because
the reasoning was sound and the answer was still no.

**And the worst case was not on the list at all**: flying *while editing*, which the player named and
which `--chisel` now measures. It read **48.57 ms** on the face pass — five times the flying figure
above, on the same camera.

**Two rules before touching any of it.** Measure on `_flybench.ps1`, not on the grid, or the case
under repair is invisible; add `-Chisel "8,16"` or the *worst* case is invisible too. And the arms of
an A/B must be **two flags of one build**, not two builds (D407): `--no-face-gate`,
`--no-face-worklist`, `--no-face-prolong` exist for exactly that.

### Closed: an edit flashed a slab of the wrong colour, and so did standing still

Reported in two halves (D421–D429). *"Whenever I modify a voxel, either place it or carve it, I can
see for a brief moment how the brick I placed that voxel on becomes a grey cube whatever material it
might be"* — **closed** (D422). *"Sometimes when I stay still I see bricks flashing with different
geometry and with the colours the facility is made of, they even cast shadows"* — **closed for the
picture** (D426, D427); the light-side loop behind the last clause is measured, named and open, and
it is [the next thing to do](#open-a-light-ray-keeps-asking-for-what-the-pool-keeps-throwing-away).

**Photograph the deterministic half before theorising about the other.** `--chisel 300,8` fires
exactly one 4,913-voxel edit, so edit+1 against edit+60 is a controlled pair:

```powershell
.\build\bin\WorldShaper.exe --screenshot out.png --screenshot-frame 61 --settle `
  --width 1280 --height 800 --cam "0,2,-20,90,0" --quality 7 --chisel 300,8 `
  --no-vsync --no-update-check --no-auto-quality --max-seconds 0
```

At edit+1 the edited cell and its neighbours came out as **flat, hard-edged rectangles of black,
cream, sky-blue and tan**; at edit+60, a clean stone cube. Those rectangles are `node.glsl`'s R2d
stand-in painting an *ancestor's* folded colour over the whole cell, and the colours are arbitrary
because a fold is only ever over the children that happen to exist. **The stand-in is not the bug** —
drawing the parent while waiting is right, and the alternative is a hole. The bug is that anything
was waiting: `NodePool::update` dropped the brick `dirty_` named and left the only route back as the
feedback round trip, which is three frames. It re-derives it from the world in place now, in the
frame that knows it changed. The ancestors come right for free, because the fold below it averages a
child that is there instead of averaging around a hole. It is **cheaper**: node-pool CPU 0.196 /
0.195 / 0.219 → **0.141 ms** at 4K under `--chisel 8,16`, because a brick that is never missing is
never missed, reported, re-descended from the root and rebuilt.

**The standing-still half, and the instrument that closed it.** D425 said what to build first — a
count of evictions of nodes that were on screen — and that instrument is now the useful part of this
whole section, because it can be pointed at the next residency question without being rebuilt.
**It cannot be made out of `node_last_read_`**: that array decides what is cold, it is stamped from
feedback, and feedback was the suspect. A count taken from the signal under suspicion agrees with the
policy however wrong the policy is. So there are two witnesses that owe it nothing:

- **the frustum**, built in `NodePool::in_view` from the same four vectors that fill the parameter
  block the marcher reads, so it cannot disagree about where the rays went. It over-counts, since it
  says nothing about occlusion — the right direction for an instrument about wrongly-evicted nodes,
  and it doubles as the price of the "refuse to evict anything on screen" fix;
- **churn**, a node requested again within `kChurnWindow` of being evicted. That is the harm itself,
  and it needs no theory about *why* the signal was lost.

Hanging off churn are the three that actually decided it: the node's level, the brick's fill, and
**whether any ray had ever reported reading it at all**. Close camera, 1280×800, settled, static,
un-edited, frame 2400: **249,454 evictions over the run, 228,964 inside the frustum, 37,213 asked for
again within two seconds — and 249,414 of them nodes no ray had EVER reported reading.** Three
resolutions barely move it (37k / 43k / 50k while the reporting lattice's period goes 64 → 256 →
1,024 frames against a 600-frame window), which is what killed "the sampling is too sparse".

**The cause was the one `touch_slot`'s comment already claimed was impossible.** It stamps the node a
ray STOPPED on, so every brick in front of that one — the ones a ray walks voxel by voxel to get
past, which is most of a facade at a grazing angle and every window reveal, cornice and step — was
read every frame and stamped never. One call after the inner walk falls out without hitting solid
fixes it (D427), on the same lattice the stop report already uses.

**Measured, against a same-build control** (`--no-node-crossings`; two flags, never two builds):

| close camera, 1280×800, settled | control | crossings on |
|---|---|---|
| churn over the run | 37,213 | **29,077** |
| ...asked for by a primary ray's miss | 1,177 | **0** |
| ...by a dilated neighbour | 6,313 | **313** |
| resident leaves | 21,747 | 20,382 |
| consecutive frames of `--debug-mode 3` at 1440p, seven pairs | 13 / 0 / 3 / 50 / 0 / 2 / 59 px, worst 153 | **nought on every pair** |
| outdoor camera, two runs of the SAME arm | 12,484 px at 0.480 | **674 px at 0.081** |

That last row is the one to keep: the change does not merely stop a flicker, it makes the outdoor
camera **reproducible**, which is what every image diff in this file is measured against. Cost is
nothing measurable — interleaved on `_flybench.ps1` at 1440p, visibility 3.700 against 3.643 ms and
total GPU 11.36 against 11.65, inside a control that spans 3.456–3.777 by itself.

`refine` also stamps the chain it walks, which closes the same hole for the proximity radius (twenty
metres that is *requested* every frame and was evicted anyway), and it **measured nothing** on the
close camera, which is sixty metres out and holds almost no proximity set. D424.

### Closed: a light ray keeps the cell that stopped it

**With D427 in, 28,695 of the 29,017 remaining rebuilds were asked for by a light ray stopped by
ignorance, and none by a primary ray.** It was a closed loop with a period of exactly `cold_frames`:
a shadow, ambient or lamp ray is stopped by a cell the pool has not built and reports it (D292's
narrowing, R9i) → the pool builds it → **no primary ray ever reads it**, because it is an occluder
and not a visible surface → six hundred frames later the erosion sweep takes it → the next light ray
to reach it reports it again. The bricks are solid (283 of 512 against a resident average of 253),
and an unbuilt cell is opaque to occlusion (D302), which is where *"and they even cast shadows"*
comes from.

**The fix is the second half of a sentence D292 had only written the first half of.** That rule says
a light path may name *the one cell that stopped it* and nothing it merely crossed. Naming it as
**missing** was the only thing a light ray could ever say about it; it can now say it is **using**
it, on the same one cell. Same set, both directions, and it asks for nothing. D430.

| close camera, 1280×800, settled | control | light keeps it |
|---|---|---|
| rebuilt within two seconds of eviction | 29,017 | **4,660** |
| evictions over the run | 242,794 | 204,973 |
| resident leaves | 20,368 | 20,746 |
| feedback, 4K flying and chiselling | 61,736, none dropped | **65,505, none dropped** |
| faces pass, 1440p flying and chiselling | 7.78 ms (7.46–8.01) | 7.88 ms (7.81–7.99) |

**Three things to know before touching it**, and two of them are things that were built and removed:

1. **One entry per RAY is not a throttle.** The first version reported from every ray stopped by the
   brick, on a per-slot schedule. That bounds the rate per node and says nothing about the total —
   thousands of rays are stopped by the same brick, and at 4K flying and chiselling it measured
   **1,538,219 reports against a 131,072 capacity, 1,407,147 dropped**. What gets dropped includes
   the *miss* reports that stream geometry, so the cure starves what it was meant to help, and it
   reads as a settled-camera success and a moving-camera disaster. `node_seen` — a card-owned word a
   slot, exactly what `face_seen` is to the pixel's reads (D414) — makes it one entry per node per
   window however many rays hit it. D431.
2. **Nothing cheaper may stand in front of that array.** A slot-hash pre-gate, so the load lands on
   a sixteenth of the reads, costs most of the benefit — churn **8,651 against 1,028** — because a
   brick is then only reported if a ray happens to read it on the one frame it is eligible, and a
   converged face casts nothing on most frames. It was chasing a +6% that did not exist: two
   interleaved rounds said so and four rounds put the arms inside each other's spread. D432.
3. **Stamping what a light ray CROSSES is not the same rule and is not carried.** It is worth
   churn 4,606 → 1,028 and costs the faces pass 7.88 → 8.25 ms flying and chiselling, outside a
   spread stop-only sits inside — but the deciding argument is that "keep everything a shadow ray
   crosses" is the thing D292 exists to forbid. D432.

The levers are `--no-light-keeps-geometry` and `--light-read-period N`, the second being the window
in frames (a power of two; 0 is off), because the trade this rule makes is the window against the
feedback entries and a trade nobody can sweep at run time is a trade somebody guesses at.

**What is left**: 4,660 rebuilds over a settled run, which are faces genuinely reopening — seen,
dropped from the gate, seen again. None of it moves a pixel between consecutive frames on any camera
measured. **And one thing that did not survive contact**: the guess that this was behind R10's open
item — *6.0% of surface held short of convergence by unbuilt geometry* — is not reproducible on this
camera at all. The held-short count reads nought in every arm, including one with both rules off, so
that open item is still open and still unexplained.

### Closed: an edit relit the whole room, so the lamps never converged again

Reported in three parts — *"after playing for a short while the light becomes like squares and they
flicker rapidly"*, *"placing voxels makes them look like an incorrect corrupted version of what I
placed for a while"*, and *"undo doesn't delete all voxels"*. The first two are **one cause and it is
closed** (D433, D434). The third is not a geometry fault at all (D436).

**Photograph the terms separately before theorising about the picture.** Two consecutive frames of a
static camera under `--chisel 60,16` — one edit a second, which is what building feels like — diffed
in each debug view:

| ten frames after an edit | sun (16) | sky (17) | near field (18) | **lamps (20)** |
|---|---|---|---|---|
| pixels differing between two consecutive frames | **0** | 791 | 6,253 | **442,227** |

and the audit line says it without a picture: never edited, `lamps on the card: 111,372 of 111,373
live faces cast no more rays at all`; with one edit a second, **`0 of 121,013`**.

**The cause.** An edit is announced for the reach a SHADOW has — sixteen metres — and every term
inside that box was reopened on it. The near field already had its own two-metre reach for exactly
this reason (`kEditAmbientReach`); the lamps had none. So a one-metre chisel stroke restarted the
lamp estimator on every face in the room, 64 frames of burst apiece, and the next stroke landed
first. Per face, so it reads as squares; every frame, so it flickers.

**The fix is a geometric question asked exactly**: a face's lamp light can only change if the moved
geometry stands *between* it and a fitting. `lamp_path_crosses_edit` slab-tests the segment from the
face to each fitting against the edited box, on the one frame the edit is announced, capped at
`kLampEditProbe` fittings. R9g's *a face never loops over lights* is about the per-sample cost and is
not broken by it — see D434.

**Measured against `--no-lamp-edit-scope`, which is the control arm and is the same build:**

| enclosed camera, 1280×800, `--settle`, `--chisel 60,16` | control | scoped |
|---|---|---|
| faces holding a settled lamp term | 0 of 120,833 | **89,408 of 121,026** |
| lamp term, two consecutive frames | 242,842 px at 4.86 | **36,747 at 2.15** (floor 27,072 at 1.97) |
| the shaded picture, two consecutive frames | 302,797 px at 5.84 | **79,949 at 3.03** (floor 70,649 at 2.92) |
| two frames after a placement, against the same scene converged | 578,934 px at 11.42 | **84,023 at 3.33** |
| faces pass at 1440p, one edit a second | 11.116 ms mean, 17.468 worst | **7.560, 11.073** |
| total GPU at 1440p, one edit a second | 18.788 ms mean, 25.902 worst | **15.332, 18.438** |

Nothing is lost: 440 frames after a 2.1-million-voxel placement the two arms' converged lamp
pictures differ by 28,718 pixels at 2.23 against that 27,072 floor, with the identical converged
count; a never-edited settled run is inside the run-to-run floor. 470 tests.

**Undo is not what it looked like** (D436). Place 2,146,689 voxels, undo, and the world comes back to
the byte — same content hash as a run that never edited — with `the node pool agrees with the world,
leaf for leaf` and a worst pixel difference of 35 of 255, with no block-shaped residue in the
difference image. What is left is the lamp term re-measuring across the room, which for a box that
large is honest work: **64 frames**, `kLampConverged / kLampBurst`. **Still open**: nothing has
measured what a larger `kLampBurst` costs, and D394 is the standing warning against metering it.
**Also unexplained**: the first half of that report — an undo leaving voxels *in the world* — which
no run here reproduces.

**One change kept although it measured nothing** (D435). `edit_min.w == 2` set a face's counters to
nought, which is the one thing `face_accumulate`'s comment promises never happens; it now drops to
`kFaceEditSeed` at the same ratio. Measured neutral on every case that could be photographed, because
the visibility pass reads the counters before the shading pass writes them. Kept for the case no
screenshot reaches — a face reopened while nobody is looking at it takes no sample at all.

### Closed: the same flicker again, from moving rather than from editing

Reported as *"after playing for a while the lights turn blocky and start flickering rapidly and
randomly"*, with a photograph of the enclosed room. **Word for word the section above, and a
different cause** (D500, D501). The section above scoped the *edit* announcement; this arrives
through the **light list**, which is global by design and which no scoping in `shade_faces.comp`
can narrow.

`build_light_list` ranks fittings by what each delivers **at the camera**, so the same lamps come
back permuted when the player has moved. `light_list_hash` ran over that order, so the host
concluded the lamps had changed and set `light_reset`, which reopens the lamp term of **every face
in the store**. A player who moves between two edits therefore relights the whole room on the second
one — and a region paste from the clip ladder counts as an edit, so it also happens on its own while
the building sharpens.

**Measured, facility, warm cache, 1280×800, `--chisel 60,16`, 600 frames, one build:**

| nine chisel strokes | version bumps | lamps on the card |
|---|---|---|
| static camera | 1 | 469,861 of 507,251 cast no more rays |
| flying, before | **9** | **0 of 997,296** |
| flying, after | **1** | **264,456 of 995,684** |

**The audit line said it in one reading and nothing had been pointed at it.** `lamps on the card: N
of M live faces cast no more rays at all` is D403's, it is printed at every screenshot, and `0 of M`
is the whole diagnosis: nothing converges, so every face re-measures every frame — per face, so it
reads as squares; every frame, so it flickers. That is trap 16 from the other end. **Reach for that
line before any picture** when light is reported as blocky or flickering, and note that the other
two terms have one each beside it (`ambient on the card`, and `faces sun on the card`).

The gate is headless: *walking to the other lamp does not change the list identity* in
`tests/test_light_list.cpp`, which requires that the ranking really did change before it checks that
the hash did not — a test that cannot tell the fix from a build where the camera makes no difference
would be trap 15. 543 tests, 18.7 M assertions.

### Closed: the blocky flicker itself — the store fills, and the card's stand-in fills the hole

**Verified the way this project's acceptance test actually works**: the player went looking for it in
a build with both changes in and *could not find it*. That is the evidence that closes it, and the
numbers below are why it went rather than whether. Note what it took to get there — two causes, two
rounds of "still there", and three diagnoses made from repros of mine before the game was made to
report the state from a real session. **Read D510 and start there next time.**

The report came back after the above, with the two clarifications that closed it: **"the bug still
happens even when you haven't placed or erased a single voxel"**, and it takes a while to appear.
Neither is consistent with anything edit-driven. D502–D507.

**Reproduce it in ninety seconds** rather than by playing, which is the whole reason `--face-budget`
exists:

```powershell
.\build\bin\WorldShaper.exe --world <world> --face-budget 120000 --screenshot out.png `
  --screenshot-frame 900 --width 1866 --height 745 --cam "0,2,-20,90,0" --quality 7 `
  --no-vsync --no-update-check --no-auto-quality
```

That is the reported picture — the facade and steps in hard blocky patches — and the flicker is a
number: **two consecutive frames of a static camera with no edits differ on 231,409 pixels of
1,390,170 at a mean of 6.44**, against **56,284 at 1.55** with room to spare.

**The chain, and every link of it was already written down somewhere:**

1. the store holds 1,048,576 faces and gives one up after 600 frames of nobody *claiming* it;
2. `last_read_` is stamped by a CLAIM and by nothing else, and a claim comes from the one pixel in
   stride² the moving lattice is asking with — so the store keeps ten seconds of everything the
   camera has walked past. Measured: **995,684 live after ten seconds of flight**, against a visible
   set of about half a million;
3. the table fills, and `claim` starts returning `kNoFace`;
4. a pixel whose fine face is missing reads the coarse stand-in three levels above it — and there is
   no host stand-in either, because claiming one is also a claim. So it falls to the **card's
   provisional** face, which by construction re-claims itself and takes **one fresh sample every
   frame** (`node_face_claim`, D316–D318, and its comment says so);
5. one ray per 8³-voxel block per frame is a blocky picture that is completely different next frame.

**Three things fixed, and the trade in the middle one is the part to read before touching it:**

- **the eviction floor** was `kFaceMinCold = 32`, reasoned from how long a face takes to converge.
  The binding quantity is how long the lattice takes to come *back*: 64 frames at 1440p, **256 at
  4K**. The emergency sweep reached 18 and evicted what the camera was pointed at. It is
  `2 × claim_period` now, set every frame from the render resolution (D503);
- **the store gives history up before it is full**, because a refusal has no graceful form. Under an
  eighth free the window drops to a quarter of itself, under a sixteenth to the floor. Spinning at
  `--face-budget 600000`: **75,421 refusals → 0** with the live count unmoved. At the real budget,
  flying: **771 and 139 → 0 and 0** over two interleaved rounds (D504);
- **where it starts is measured and the obvious setting was wrong.** At *half* free it removes every
  refusal and costs the faces pass **1.918 → 8.086 ms**, because a short window also gives up faces
  whose coverage is under a pixel — which the lattice samples rarely however plainly they are on
  screen — and each pays its whole ambient burst again. At an eighth the arms interleave inside each
  other's spread (D505).

`--no-face-pressure` is the control arm. The audit line now reads `N live of M … R REFUSED, cold
window C frames (floor F)`: **read the refusal count and the fullness before anything else** when
light is reported as blocky, because `out_of_room()` is a state and a store that fills and recovers
sixty times a second reads as fine at every moment anybody asks (D507).

**Then residency was given the exact signal, and it did not buy what it was built to buy.** D508,
D509. `last_read_` is now stamped by what a pixel READ: `face_read` is a card-owned word a slot and a
face reports itself down the feedback buffer at most once per `face_read_period` frames however many
pixels are on it — `node_seen` (D431) doing for the store what it already does for the pool. It
works, it costs nothing measurable (740–5,271 entries a frame, 63,822 of 131,072, none dropped), and
**it does not pay for D505**: flying with the squeeze turned up, faces reads-off **7.108 ms** against
reads-on **7.181**. The prediction was that a short window was expensive because eviction was blind
to sub-pixel faces; it is expensive because faces that genuinely leave the screen and come back have
to burst again, and no signal makes that free. Kept for what it does buy — "cold" means what it says,
and the eviction floor stops scaling with resolution (128 frames rather than 512 at 4K).
`--no-face-reads` is the control arm and `--face-pressure-from N` sweeps the trade D505 measured.

**And the instrument that should have existed three rounds ago** (D510). Every number about this
store was printed at the screenshot audit and nowhere else, so the state that produces the reported
picture was invisible in the one situation it gets reported from. `worldshaper.log` now carries a
rate-limited warning naming the refusals and the fullness while somebody is *playing*, and a
heartbeat every six hundred frames whether or not anything is wrong. **When this is reported again,
read the player's own log before building any repro** — three rounds of it have now been diagnosed
from repros of mine, and the second and third were repros of the wrong thing.

### Opening a world re-samples the clip — and the hiccup half of this is closed

Reported alongside the above as *"you're not using the resolution pixel screen based loading of
detail system streaming in worlds"* and *"the game has massive hiccups"*, and **both halves were
true**. The hiccups are **fixed** (D511–D514): they were the paste waiting on the sampler through a
shared job pool, not the paste, and the twelve pastes of a cold load now come to **719 ms between
them** rather than 34,697. The re-sampling half stands. Numbers from the real path a player takes
rather than from `--clip-file`:

- a world whose `.world` cache is **complete** loads in **119 ms** with no pastes and no hiccups;
- a world whose cache is not complete re-runs the ladder. Each region paste used to **block the
  main thread**: 1.4, 6.5, 7.0, 13.0 and **14.1 s** in single frames. It is now **31–92 ms**, and
  the sampling behind it — 7 s a region — never blocked a frame and still does not;
- the facility had **never** completed one, because the cache is written at refinement's fixed point
  and the late regions cost 7–26 s of sampling apiece. Progress *is* carried between launches —
  `cached world has 12 of 18 regions sharpened; carrying on from here` — so it finishes eventually,
  and until it does every launch pays;
- and every paste is an announced world change, so it rebuilds the light list. That is how the
  flicker above fires with nobody editing anything.

**A run that edits the world now writes no cache at all** (D607) — not "no cache until the ladder
finishes", which is what it used to mean. The file is keyed on the **clip** and handed to every
world built from it, so a square carved into a finished facility came back in every new world made
from that clip. If you are measuring loads, that means `--edit`, `--chisel` and anything that puts
an op in the log leave the next run cold, and the log says so in as many words: *"N of M regions
sharpened, but the world has been edited; not caching it as the clip's own"*. Two arms of one build
on `many_lamps.wsworld` prove it, and note the arm that nearly lied: with `--settle` the ladder
finishes long before the **default edit frame of 100**, so the edit arm cached an unedited world and
looked identical to the control. Pass `--edit-frame 1`.

**A trap for anyone measuring loads:** `--clip-coarse` is part of the cache key, because
`src/app/main.cpp` divides `script.settings.voxels_per_metre` by `coarse` *before* the key is
computed. A 608 MB cache written under `--clip-coarse 1` is deleted as stale by the next default
launch. The arms of a `--clip-coarse` sweep destroy each other's caches. D501.

### R1e is done. The renderer has no chunks in it.

**All five slices have landed** (D521–D525). What this section used to say was the plan for the
fifth, and two of its three warnings were wrong in a way worth keeping:

1. it said `world_buffers_` could not be trimmed **until the descriptor bindings were renumbered**,
   and called that *the risky edit in the stage*. Nothing had to be renumbered. A Vulkan layout's
   binding numbers need not be contiguous, so the cloud pass keeps 13, 20 and 21 and the seventeen
   descriptors no shader declares are simply absent from the set;
2. it said the chunk system cost **about 12 ms of CPU a frame**. The instrument had been saying
   0.003 ms mean with a 12–15 ms *worst* the whole time, so that was one frame's spike quoted as a
   steady state. The costs that were real: 970 MB of device memory, 165 ms of every warm start,
   and **3.86 ms on every edit** in `rebuild_coarse_grids`;
3. it was right about the order — `residency.*` could not go until nothing read the buffers it
   fed, and nothing read them once both orphaned descriptor sets went.

**Measured against a same-session control build**, because the machine drifts about 10% over a long
session and two runs an hour apart are not an A/B (D407, and D523 for it arriving through the grid):
content hash `766f2fd63f1a01c4` unmoved, the settled enclosed picture inside its own run-to-run
floor, `GPU mirror matches`, *leaf for leaf*, *mask for mask*, `--validation` clean, 505 tests.

### There is no minimum light, and there is a gate that says so

**Asked for directly and both halves were real faults** (D541–D543). Two constants in the composite
made a black surface impossible: `kIndirectFloor = 0.5`, the share of the sky term applied wherever
measured sky visibility fell short, and `kGroundBounce = 0.12`, which was added to **every surface in
the world** unconditionally. Both are deleted rather than switched off — R9's bounce measures what
they were standing in for.

**The air was the second way in and the worse one.** `apply_media` lit every cubic metre of fog with
the full sun and the full sky whether or not either reached it, so a sealed room with fog glowed —
and that glow sits in *front* of the walls, so no amount of black paint removes it. It takes
`sun_reach` and `sky_reach` now, filled from the surface's own measured visibilities. It is a proxy
and is documented as one: fog in front of a shadowed wall is dimmer than it should be, which is the
direction this renderer is required to err in.

**The gate**, because a constant added to a lit scene is invisible in every other test and these two
survived the whole rewrite:

```powershell
.\tools\darkroom.ps1              # brightest channel 0 of 255, every pixel
.\tools\darkroom.ps1 -Fog         # ...and with fog in the room
```

`clips/sealed_dark.clip` is four metres of air inside two metres of stone — no opening, no emitter,
no sky — so every term is nought by construction and any pixel above nought is the renderer adding
light it was not given. Run it after anything that touches the composite or the air.

**On the facility it costs almost nothing**, which is the point: the enclosed camera's mean pixel
goes 126.3 → 124.8 and the portico's 135.0 → 133.6, while **the darkest pixel indoors falls from 4.7
of 255 to 0.4**. And **`--no-bounce` is no longer a way back to the old picture** — the floor it used
to restore does not exist.

### Bounce is in, and it is the fourth term

**`kIndirectFloor` is no longer what lights an interior** (D533–D538). The ambient far ray — unbounded,
cosine-weighted about the face's normal, and already being cast — now returns what it FOUND: the sky
where it escaped, and the outgoing radiance of the face it landed on where it did not, read from that
face's own record. Three words of face light hold the sum; the far field's own count divides it,
because it is one ray answering two questions.

**Measured, enclosed camera, three interleaved rounds of one build**: faces pass **0.757/0.762/0.780
against 0.644/0.722/0.726**, total GPU **3.101/3.155/3.219 against 3.024/3.028/3.072** — about **+7%
of the light pass and +5% of the frame**. Flying at 1440p the arms overlap. The picture moves by
**16.998 of 255 over 763,794 pixels of 1,024,000** and is *quieter*: **speckle 17.62 with 9 fireflies
against 21.22 with 81**.

**The prediction in this file was wrong and the direction is the useful part.** It said one measured
bounce would be dimmer than a constant of 0.5 and that interiors would darken until the exposure meter
had a writer. Outdoors it is the opposite by a wide margin — sunlit stone bounces far more than the
constant stood in for, and the portico, the shafts and the steps go from crushed black to legible.
Indoors the constant was flat and the measurement has shape.

**Four things to know before touching it**, three of which were faults on the way:

1. **`kFaceLightWords` was declared twice** — in `node.glsl` and again in `resolve.comp` — and the
   record grew to twelve words with only the writer's copy changed. The composite then read every
   face's record at the wrong stride and the enclosed camera came out as **salt and pepper over a
   black building**, which reads exactly like an estimator with too few samples and cost two fixes
   aimed at the wrong thing. It lives in `shaders/face_terms.glsl` now, which both include (D534).
2. **The bounce is believed in proportion to its samples**, smoothly, over `kBounceBelieve` = 32. A
   hard switch is a discontinuity per face, which is D387's measurement (D535).
3. **`kSkyFarEager` was already the right convergence rate.** Extending the eager phase to the whole
   of `kBounceMin` converges in two seconds instead of thirteen and costs the faces pass **4.3–4.6 ms
   flying against 2.0–2.4** — over budget for the first time since D416, and it buys nothing the
   confidence blend does not already give (D536).
4. **A `vec4` in a push block aligns to sixteen bytes and an `f32[4]` in a C++ struct to four**, so
   the shader declared 128 bytes against a range of 124 and read every later field early.
   `--validation` is the only thing that says so, and the `static_assert` on that struct is an
   equality rather than a bound for the same reason (D537).

**Open, reported from playing and not closed: a fine grid on flat surfaces.** *"Subtle horizontal
lines on everything"*, then *"it seems to be a grid"*, then *"when I set the window to max size it
gets fixed as I leave and come back"*. It is **not** the bounce (5.65 with the term off against 5.66
with it on, on the same patch), not the model, not the detail level, not the material, not the
prolongation, not the lamps, and not the ray footprint — D539 has the eight eliminations, each with
its number, and D540 has the measurement that started it and was wrong. What is left is the class
rather than a bug: every light term here is a per-face Monte Carlo estimate and **nothing filters
across faces**, which is R5's charter. The one clue with nothing behind it yet is the revisit —
`kFaceAmbientDone` freezes a face's ambient and bounce for the life of the face, so faces that
converged while the room was still filling keep a darker answer than their neighbours until something
evicts them. That is testable and it is the thread to pull first.

**That thread was pulled and it was a real fault** — D547–D552, written up below as *the light
remembers where you have been standing*. It was not `kFaceAmbientDone` on its own: the freeze is what
makes it permanent, and what makes it WRONG is that the bounce was a cumulative mean of a quantity
still in motion. Fixing it took the enclosed room's speckle from **20.701 to 16.970** and its mean
pixel from 122.785 to 126.412, so it is very likely a share of the grid as well — but the grid has not
been re-measured since, and nobody should assume it went with it.

### Closed: the light remembers where you have been standing

Reported from playing: *"when i stay still for a while and then move back the voxel faces where i
stood still are way betterly rendered than the rest and are often brighter"*. **Two halves, one cause
for the brightness half, and it is closed** (D547–D552). The other half is a sample count and is
R5's; it is at the bottom of this section, named, so it is not mistaken for this one later.

**The repro is `--cut`, which is the one instrument that can put two dwell histories on one camera:**

```powershell
.\build\bin\WorldShaper.exe --screenshot dwell.png --screenshot-frame 900 --settle `
  --width 1280 --height 800 --cam "0,2,-20,90,0" --quality 7 --no-vsync --no-update-check `
  --no-auto-quality
.\build\bin\WorldShaper.exe --screenshot arrive.png --screenshot-frame 900 --settle `
  --width 1280 --height 800 --cam "0,2,-20,-90,0" --cut "600,0,2,-20,90,0" --quality 7 `
  --no-vsync --no-update-check --no-auto-quality
```

Same camera, same settled world, same content hash, same frame number — and the two pictures were
**5.461 of 255 apart over 260,752 pixels of 1,024,000, against a run-to-run floor of 2.868 over
79,519**, with the arrival the darker. Standing still and doing nothing at all, the whole frame's mean
pixel climbed **131.290 → 131.794 → 132.608 → 132.697** at 150, 300, 900 and 2,700 measured frames.

**The cause.** `bounce_radiance` reads what the face a ray landed on is giving off *at that moment*,
and that face is itself climbing from black as it takes its own samples — a progressive radiosity
solve, iterated one ray at a time. The estimator over it was `sum / far_n`, **a cumulative mean of a
quantity that is still in motion**, so it converged to the average of the climb rather than to the top
of it, with a time constant that grows with the sample count. Then `kFaceAmbientDone` froze it. Every
other term in the record measures something that does not move, which is why the same estimator is
right for all of them and wrong only here.

**The fix is three words of the record it already had and one `min`**: the bounce is a mean with a
memory of `kBounceMemory` = 128 samples, `mean += (sample - mean) / min(far_n, N)`, and `kBounceMin`
is four memories so that three turnovers have happened before a face may fall silent on what it holds.
Nothing about the ray RATE changes — it is the same one unbounded ray every `face_stride` frames — so
no frame casts more rays than it did.

| 1280×800, `--settle`, frame 2,700, every face silent | before | after |
|---|---|---|
| **enclosed** speckle | 20.701 | **16.970** |
| **enclosed** mean pixel | 122.785 | **126.412** |
| outdoor speckle | 16.752 | 15.351 |
| outdoor mean pixel | 161.590 | 161.649 |
| close camera, dwell bias against unbiased | −0.85 of 255 | **−0.05** |
| close camera speckle | 47.63 | **45.53** |
| flying at 1440p, faces pass, two rounds each | 6.691 / 7.349 | 7.258 / 6.529 |
| static, frames 1,350–2,700, faces pass | **0.607** | **1.427** |

**Five things to know before touching it**, and the first is the one that decides everything else:

0. **Changing what a word means makes every rule written about it suspect** (D553). Four places write
   the bounce words. Three are obvious; the fourth is the edit reset, which scaled them by
   `seed / far_n` with a comment explaining that it had to *because they are a sum* — correct when it
   was written, and after this change it divides a MEAN by four and reads as the room going dark
   whenever the player chisels. There was no compiler error and there would have been no obvious
   picture, because an edit already reopens half the terms in the room. The gate is that an edited
   room is not darker: enclosed camera under `--chisel 60,16`, mean pixel **127.341 against 127.177
   before and 126.368 unedited**.

1. **Read `kBounceMemory` and `kBounceMin` together or the trade comes out backwards.** Memory 32 at
   min 256 is the *noisiest* arm in the sweep (speckle 54.00) and memory 128 at min 512 is the
   quietest (45.53) — because most of the noise was never the memory. The far ray answers the sky as
   well, and four times the samples is half the error in `open_sky`. The full five-row sweep is in
   D550–D552.
2. **The one real cost is the tail, and it is the case the player was in.** A face that has stopped
   costs nothing, and this makes a face stop after about forty seconds of standing still instead of
   ten: over frames 1,350–2,700 the faces pass reads **1.427 ms against 0.607** and total GPU 4.427
   against 3.506. Both are far inside the 4.40 ms budget and neither is a moving frame. **The moving
   case is provably untouched** rather than luckily so — no face lives long enough while flying to
   reach even 128 far samples.
3. **The reclaim is known and deliberately not built.** A face only needs the extra turnovers while
   the light it is sampling is still moving, and `bounce_radiance` can already see whether the face it
   landed on has finished, in a record it has already loaded. That would let a face stop at 128 once
   its own sources are silent. It is unbuilt because the case that costs is an interior, where every
   ray lands on another face that is also waiting, so the wavefront has to start somewhere and nothing
   has measured where.
4. **`--debug-mode 19` shows a seam that is not this fault.** After a turn, the part of the facade the
   camera had been pointed at is solid green and the newly revealed part is grey, which looks exactly
   like the report. That seam closes by itself in two seconds; the brightness did not close at all.
   Read the mean pixel against dwell time first.

**The other half of the report is open and is R5's.** A face the camera has just revealed carries
**46 sun samples against 203**, so its shadow is genuinely coarser for about seventeen seconds — the
sun's counters halve at `kFaceWindow` rather than growing for ever, so it does catch up exactly, and
the wait is bounded and the same everywhere. Extending an eager phase to shorten it is known-bad by
measurement (D536 did it for the far ray: 2.2 → 4.6 ms flying, over budget). Filtering across
neighbouring faces is what actually pays here, and that is R5.

### R9a is in: the face set is no longer only what the camera can see

**Three sub-steps landed and the picture did not move, deliberately** (D526–D532). The ambient far
ray — the unbounded one, already cast, cosine-weighted about the face's normal — now names the one
face it LANDED on, and the store claims it in a class of its own:

- **R9a**, the report. It is the deliberate exception to D292: a light ray may name what it landed
  on, which is one face, and never the geometry it crossed. The host's answer is a face claim, which
  builds nothing and asks the world for nothing, so **R9h's *no light path may cause streaming* holds
  by construction** — measured, node requests **18,828,939 against 18,830,058** over a settled run;
- **R9b**, the cap. A quarter of the table, and past it a claim is **declined rather than refused**.
  The store also declines the whole class the moment it is under any pressure, so this cannot be what
  pushes the table into refusing a face somebody is looking at — which is D502's picture;
- **R9e**, the counting. `the set on the card` and `the off-screen set` in the audit.

**Nothing read the set, and that stayed true for longer than the sentence below expected** — the
bounce reads a face's record, and every record in this class was empty. See *the off-screen set held a
quarter of a million records with nothing in them*, below, which is the change that closed it and the
one that overturned this file's own advice about what to do next.

**Two traps came out of it, and both are the kind that produce a plausible picture and no error.**

1. **A change that makes a pass cheaper by giving it less to do is a regression wearing an
   improvement's clothes** (D527). `face_stride`, the sun's ray budget, was divided by the WATERMARK,
   so 262,144 off-screen faces diluted the refresh rate of every face on screen: the faces pass read
   **0.96 ms against a control's 1.16**, and the cost was in a number no pass table shows — **72 sun
   samples a face against 84**. Print a convergence figure beside every timing in this pass.
2. **An instrument's own bookkeeping must never live in a field the card is writing to** (D528). The
   class was a flag of `GpuFace::packed` for an afternoon. `packed` is mirrored, the uploader sends
   whole records for dirty slots, and the record has two owners — so promoting a face sent the host's
   zeroed counters over the light the card had accumulated. That is D295 through a door D295 did not
   name, it would have cost **29,882 faces a flight** their light, and the picture, the mirror and
   every audit would all have looked right.

**And one measurement that is bigger than the change that found it — now closed, and it was not what
it looked like.** The face pass's cost while moving is a function of how many live records the
**card** holds, and the card ran **up to 434,838 records ahead of the store** while flying, because
an upload that ran out of staging cleared nothing and retried the whole dirty set next frame (165
frames of 400 did). R9a's flying win (**faces 3.123 → 1.621 ms median**, five interleaved rounds with
no overlap) was that backlog moving rather than the light getting cheaper.

**D544 fixes it and D545 is the part to read.** A partial upload now marks clean exactly what it sent:
the card is **0 records ahead against 80,211**, and the upload runs out on **1 frame against 253**.
What that uncovered is that the backlog was not making the pass shade too much — it was making it
shade almost **nothing**. The audit line says it without a picture: **`seen on the card: 0 of
721,911`** in the control arm, because the card's bucket table was too far behind for a pixel to find
its own face, so `may_cast` was false everywhere and the frame was drawn from **8,255 throwaway
provisional stand-ins a frame against 723**. The faces pass therefore goes **2.0 → 6.8 ms** flying and
none of it is new work. The picture goes from hard-edged black and white blocks on the balustrade and
the cornice to lit stone: **44.90 of 255 over 2.76 M pixels of 3.69 M**, speckle **23.86 with 2,720
fireflies → 19.92 with 944**.

**So the faces pass is over its 4.40 ms budget while flying, honestly, for the first time.** That is a
budget question for R5 rather than a reason to put the backlog back. The instruments to read first
are `the card is N records ahead of the store`, `seen on the card` and `the card's own stand-ins`, all
at every screenshot.

### Closed: the light of a room was the first thing thrown away when you left it

**R9f's outlive half is in** (D554–D560). The coarse pyramid — the stand-in faces, one per 512 fine
faces — is what a returning camera rebuilds everything from, and it was the **first** record the store
gave up, not the last. `last_read_` is stamped by a CLAIM; a stand-in is claimed only when a fine face
under it is NEW; so a camera that has stopped discovering geometry never stamps one again. It goes
cold at 600 frames while every child is still live, and then the children go too.

**The control arm holds nothing at all above level 1**, which is the whole diagnosis in one line of
the audit:

| close camera, 1280×800, settled, frame 900 | `--no-coarse-keep --no-coarse-bounce` | default |
|---|---|---|
| coarse faces live in the store | **0** of 711,000 | **21,794** of 759,000 |
| stand-ins given up over the run | 21,796 | **0** |
| faces by level | 0 and 1 only | 0, 1, **3, 4, 6** |
| gathering rays that found no light but had a coarse face that did | 2,881 of 32,153 (9.0%) | **8,291 of 27,016 (30.7%)** |

Two rules, two control arms, because either could have been the one that paid: `--no-coarse-keep` is
the store keeping them, `--no-coarse-bounce` is a gathering ray reading them.

**What it is worth, and where.** Converged at frame 2,700, one round a camera: close mean pixel
**133.5 → 140.0** with speckle **45.5 → 38.5**; enclosed 126.4 → 127.6 with fireflies **36 → 9**;
outdoor unmoved (0.214 of 255) with fireflies 288 → 171. Most where the store forgets most, nothing
outdoors where rays reach sky, quieter everywhere. Three frames after walking back into a room, the
card's own provisional stand-ins — the most expensive face in the renderer, one fresh unbounded ray
and one fresh lamp burst *every frame* — go **3,137 → 99**, with speckle 34.35 → 19.58 and fireflies
1,494 → 387.

**Four things to know before touching it.**

1. **It errs bright where the old answer erred dark, and `tools\darkroom.ps1` is why that is safe.**
   A gathering ray that lands on a real surface and returns nought says that surface emits nothing.
   Both darkroom arms are still BLACK at 0 of 255, so nothing is invented; what is recovered is light
   that was measured and thrown away. Run that gate first after anything here.
2. **It cost the sun's stride, and the timing said nothing** (D557). A kept stand-in is a PRIMARY
   face, so 21,799 of them went into the denominator of the sun's ray budget and took the stride from
   5 to 6 — the faces pass read **1.553 ms against a control's 1.564** while **107,582 of 497,656
   faces had converged against 475,632 of 476,230**. That is D527's sentence for the third time.
   Anything that adds faces which cannot cast must come out of that denominator, and the tell is
   always a convergence number beside the timing, never the timing.
3. **The fold is NOT done and is a separate change.** A coarse face still measures itself with its
   own rays; it does not average its children. The shape that keeps D191's one-writer property is a
   PULL — a coarse face reading the four child faces under it — not 512 children pushing into it.
   §8 R9f now says this in full.
4. **Reading light where the pool has not built is blocked on the marcher** (D558). An ignorance stop
   carries no face key: `node_face_hit` runs at the leaf hit and nowhere else. That clause of R9f
   cannot be attempted without changing `node_march` first.

**Two instruments came with it and both outlive the change.**

- **`--cut` is repeatable.** One cut measures arriving somewhere; two measure LEAVING and coming
  back, which is what a player does and what "walk out of a lit room and back" needs. The frames are
  absolute measured frames and a cut that is not after the one before it is warned about rather than
  silently reordered.

  ```powershell
  .\build\bin\WorldShaper.exe --screenshot back.png --screenshot-frame 1260 --settle `
    --width 1280 --height 800 --cam "0,0,0,-90,0" --quality 7 --no-vsync --no-update-check `
    --no-auto-quality --cut "300,0,10,-60,90,-6" --cut "1200,0,0,0,-90,0"
  ```

  Diff that against the same run with no cuts, which is the same camera having never left. **Do not
  read the first few frames after a cut as a light measurement**: at return+3 most of the difference
  is the NODE POOL rebuilding, and the two systems recover at different rates (46.0 against 45.2 at
  +3, 7.291 against 6.265 at +60).

- **The gathering ray is counted.** `the gathering ray, last frame:` reports what the unbounded
  ambient ray landed on — sky, a lit face, a surface with no face at all, a face that has measured
  nothing yet, a cell the pool has not built — as a rate over one frame, plus how many of the ones
  that found nothing had a coarse face that could have answered. That last figure is printed whether
  the rule is on or off, so it reads as *what this would recover* in the control arm and *what it is
  recovering* in the other. It also carries the by-level histogram of ignorance stops that R10's open
  item has never had. `--no-light-probe` turns it off; the dials live in word 0 of the probe buffer
  because the push block is exactly 128 bytes full.

**What the probe says is left.** On the close camera, settled: 79,310 gathering rays a frame, 47.3%
reach sky, 18.6% land on a lit face, and **34.1% land on a surface with nothing to give** — of which
R9f answers 30.7%. So about a quarter of everything the bounce integrates is still black, and that is
the size of R9c and R9g–R9h.

### Closed: the off-screen set was a quarter of a million records with nothing in them

**This file said to do R9c next, and one audit line said R9c would have changed no pixel.** That is
the part to keep: the reasoning behind the order was sound, and reading the instrument before building
the plan overturned it in five minutes. D561–D568.

R9a claims a face for whatever a gathering ray lands on. Nothing then lit those faces, because
`may_cast` is `node_face_recently_seen` and that stamp is written by the **visibility** pass, which
only ever runs on pixels. Close camera, settled, frame 900:

- **229,413 off-screen faces of a cap of 262,144, at nought sun samples each and nought with a
  finished ambient term** — a quarter of the table holding empty records;
- **12.4% of every gathering ray in the frame landed on one of them** and read the emptiness back.

R9c claims a margin of faces just off the edge of the screen. Those are off-screen faces. It would
have moved rays out of *"landed on a surface with no face"* and into *"landed on a face with nothing
measured yet"*, and lit nothing at all. **The prerequisite is R9b's ray share, and `make_node_push`
had been saying so in as many words** — *"the off-screen class casts no rays at all today, so its
share of this budget is nought"* — which read as a fact about the world rather than as the missing
half of a sub-step.

**What landed**: an off-screen face measures its own light, out of a budget of its own.
`face_gathered` is a card-owned word a slot stamped by `bounce_radiance` — `face_seen` for the pixel,
`node_seen` for the light's geometry, and this for the light's faces — and a face is worth a ray when
something is *integrating* it. It must be a second array and not a second meaning for `face_seen`,
because that population is what the sun's budget is divided by and a shared stamp is D527 for the
fourth time.

| close camera, 1280×800, settled, frame 900 | `--no-secondary-light` | default |
|---|---|---|
| gathering rays landing on a **lit** face | 18.5 / 18.8 / 18.7% | **30.2 / 29.9 / 29.9%** |
| ...on a face in the store with nothing measured | 12.5% | **1.7%** |
| ...on a surface with no face at all | 21.3% | 21.9% — **unchanged, and it is R9c's** |
| enclosed mean pixel, speckle, fireflies | 127.51, 16.13, 9 | **150.17, 12.80, 0** |
| close mean pixel, speckle, fireflies | 139.80, 40.13, 108 | **143.10, 35.83, 27** |
| outdoor mean pixel | 161.75 | 161.83 — nothing, correctly |

Three interleaved rounds, one build, two flags. The enclosed picture is the same at frame 3,600, so
it is the answer rather than a transient; outdoors nothing moves, because outdoors a gathering ray
reaches sky. `darkroom.ps1` BLACK clear and with fog, so what is recovered is light that was measured
and thrown away rather than light invented. 519 tests.

**Three things to know before touching it, and the first decides the other two.**

1. **The cost is a TAIL, not a rate, and the budget cannot buy it back** (D566). Thirty-five faces
   shaded cost **0.85 ms**; the next 2,335 cost 0.94. Eight times fewer faces returns a tenth of the
   cost and gives up more than half the win, which is why the default is the generous end of the dial.
   Three explanations were priced and rejected — the rays themselves, a scattered load in front of the
   stride (reordered, worth 0.1 ms, kept anyway), and lane divergence in the compacted dispatch. What
   fits is that the pass ends when its last workgroup does and one face taking its first full ambient
   burst sets that floor alone. **That is a hypothesis that fits, not a proved cause**; what would
   prove it is grouping the class's slots in the work list, which is the Morton-sort lever under a
   different name and is not built.
2. **The moving case pays most of the cost for a tenth of the win** (D568). Flying at 1440p, the faces
   pass goes **6.69 → 7.91 ms** on a pass already over its 4.40 ms budget, for **16.1% against 15.0%**
   of rays landing lit. Flying, two thirds of every gathering ray reaches sky and no off-screen face
   lives long enough to converge. The class is worth most where a player stops to build and look.
3. **The instrument is counted, not predicted** (D567). The host divides a population by a budget and
   hands down a stride; that is what it *meant* to happen. `the off-screen set cast on N faces this
   frame` is what did, printed beside the `off-screen stride` on the `faces:` line. Read them as a
   pair — and note it is a rate over one frame, so nought on a fully converged camera means "nothing
   was due", which the `set on the card` line beside it disambiguates.

The levers are `--no-secondary-light` (the control arm, and the state every figure above this section
was measured in) and `--secondary-light-share N`, the divisor of the on-screen set's shading rate.

**What this makes more pressing, and neither is its to fix.** R6's exposure meter, because
`kPreviewExposure` is still the constant 3.2 with no writer and the enclosed room is now 150 of 255
rather than 128. And R5, because the argument for this pass being inside its budget while moving is
further from true than it was.

### Closed: R5a smeared every lit face into the eight around it — and the report was the diagnosis

**Reported from playing:** *"speckles that show for a very brief second when looking at new places or
turning your camera, especially on dark places"*, then, unprompted, *"the speckles I think have the
size of 3x3 voxel faces"* and *"where the central pixel is properly coloured"*. That is a description
of `face_denoise`'s kernel, and it named the fault in two sentences. D579–D581.

**What D573 got right and what it over-generalised.** A coplanar neighbour needs no plane test, no
normal test and no depth test — a change of plane is a change of face key and the lookup simply
misses. That stands. What does not follow is that it needs **no test at all**: a flat plane carries
real lighting discontinuities across it — a shadow edge, the line where an alcove stops shading, the
last voxel a sconce reaches — and blending across one is a bias rather than a denoise. So every lit
face lent light to the eight around it. The lit face itself stays right, because its own answer
dominates its own average; the ring is what moves.

**Each neighbour is now weighted by how much it AGREES with this face, and the strength of that test
rises with how well this face knows its own answer** (`far_n / kBounceBelieve`, which is already
stored). A face with almost no samples accepts its neighbours whole — that is R5a's borrowing and it
is untouched — and a face that has measured itself for hundreds of samples refuses a neighbour four
times brighter. Without the scaling the two halves fight: an unconditional edge test forbids
borrowing, and no edge test smears.

| enclosed, settled, 1280x800 | no filter | filter as reported | with the agreement test |
|---|---|---|---|
| **edges: count at mean strength** | 91,707 at 62.93 | **79,781 at 66.12** | **87,883 at 63.64** |
| roughness | 3.0340 | 1.7480 | 1.8622 |
| speckle | 12.210 | 8.097 | 8.921 |
| fireflies | 0 | 9 | **0** |
| faces pass | 2.632 ms | 2.808 | 2.809 |
| close: roughness, speckle | 4.3284, 33.627 | 2.9688, 26.321 | 3.0730, 27.963 |
| close: faces pass | 3.617 ms | 3.995 | 3.877 |

**13% of the enclosed camera's lighting edges were being destroyed and 4.2% are now**, for 90% of the
roughness win and 80% of the speckle win kept. Flying, two interleaved rounds: faces 7.459 / 7.634
against 7.316 / 7.350. `--denoise-edge 0` is the control arm and restores the smearing filter exactly;
`--denoise-edge N` sweeps the sharpness.

**Two things to know, and the first is the reason this shipped on an argument.**

1. **The transient it was reported from cannot be measured** (D580). Three runs of ONE arm,
   `--cut` from outdoors into the room at cut+6: mean pixel **69.990 / 82.286 / 84.888**, fireflies
   1836 / 2268 / 2007. With the meter held fixed too, the two arms sat well inside each other's
   spread. **Three tables of transient figures were produced and thrown away before that was
   checked.** `--settle` makes the SETTLED state reproducible and says nothing about frame six of a
   rebuild. What was used instead is the settled edge population above: the artefact is light where
   there should be none, which is a destroyed edge, and a destroyed edge is countable on a settled
   camera even though it is only *visible* during the transient. **A mechanism can be measured where
   a symptom cannot** — and the acceptance test is a player going to look for it and not finding it,
   which is how D510 was closed.
2. **A trimmed mean was built first and taken out.** Find the brightest tap, drop it if it is more
   than four times the mean of the others, on the theory that one bad face was contaminating nine. It
   measured as nothing, because the case is the reverse: the extreme tap is usually the *correct* one,
   a genuinely lit face beside dark ones, and what needed excluding was the disagreement rather than
   the extreme.

### Closed: the light meter had no ceiling, so no room could be dark

**Asked for directly: "add an auto exposure floor so that at some point darkness is pitch black".** A
meter has no opinion about absolute brightness — it makes every scene average to the same grey — so
without a ceiling there is no such thing as a dark room. A sealed unlit corner of
`clips/many_lamps.clip` wound it to **429x** and read a mean pixel of 35.6 with 30,104 pixels over
200: a lit-looking picture of a room with no light in it. That is D541–D543's deleted light floor
arriving through a different door, and it sits above the whole picture rather than under each
surface, so no amount of black paint reaches it.

**The number is measured against the one scene that legitimately needs a large exposure**:
`clips/exposure_range.clip`, a room lit through one window, which the meter takes to 33.3x and reads
correctly there. Everything past that is a room with less light in it than one window.

| | ceiling 4096 (what R6a shipped with) | ceiling 64 |
|---|---|---|
| the dark corner: the meter chose | 435.995x | **63.999x** |
| ...mean pixel | 35.603 | **10.239** |
| ...pixels over 200 | 30,104 | **1,020** — the sconces themselves |
| the window room: mean pixel | 149.379 | **149.329** — untouched |

4096 was the tracer's and was a guard against NaN rather than a decision. `--exposure-max N` sweeps
it. The dial lives in a new `tone` vector in the parameter block rather than in `motion.w`, which was
free since R3d deleted the accumulator it described — D553 is why not.

### Closed: the brightness dial had no writer, and two test scenes could not be used because of it

**`resolve.comp`'s `kPreviewExposure` was the constant 3.2 and nothing had written it since R3d
deleted the tracer and R1e deleted the frame-statistics buffer under it.** It is quoted as an open
item in this file, in the plan and in the decision log; this closes it. D577, D578.

**The two cases it broke are both clips somebody wrote to test exposure and then could not use:**

| 1280×800, `--settle` | fixed 3.2 | metered |
|---|---|---|
| `clips/many_lamps.clip`, mean pixel | **248.873** | **150.598** |
| ...pixels over 200 of 1,024,000 | **1,022,963** | 65,322 |
| ...fully blown, over 254 | **72,736** | **0** |
| `clips/exposure_range.clip`, mean pixel | **35.754** | **149.312** |
| the multiplier the meter chose | — | **0.187×** and **33.178×** |

`exposure_range.clip`'s own header predicted its own result before any meter existed — *"the sky
clips to white, and the room, which is where the light actually has to be judged, comes out black"* —
and it read 35.8 of 255.

**The facility moves by 2–6% and not by 35%, and that is a decision rather than luck** (D578).
Metering this building to middle grey alone chooses **1.214× enclosed, 1.485× at the steps and
1.177× outdoors** against the constant's 3.2, so every picture anybody has looked at sits about one
and a third stops above a middle-grey meter — a log average is dragged by its darkest pixels and this
building is mostly shadowed stone. `kExposureBias` is +1.3 stops and is a **separate constant** from
`kMiddleGrey`: the meter is a measurement and the compensation is a look, and folding a taste
decision into a named physical standard is how a standard stops being checkable.

| settled, two rounds each | fixed 3.2 | metered | the meter chose |
|---|---|---|---|
| enclosed mean pixel | 157.385 / 157.471 | 153.636 / 153.654 | 2.993× |
| close | 144.075 / 144.063 | 152.061 / 152.056 | 3.655× |
| outdoor | 161.856 / 161.844 | 155.503 / 155.488 | 2.898× |
| resolve pass | 0.819 / 0.729 / 0.559 ms | 0.820 / 0.728 / 0.555 | |

**Five things to know before touching it.**

1. **It does not pump, and that is the one thing a settled grid cannot see.** Two consecutive frames
   of a static camera read **2.991× and 2.991×**, to the digit. Arriving somewhere new,
   `--cut` from the outdoor camera into the room takes it 2.899× → 4.014× by cut+5, which is the
   half-second ease. **If light is ever reported as breathing or pulsing, read the meter line at two
   frame numbers before anything else.**
2. **Two slots, and the reason is a gradient across the picture rather than a wrong number.** Slot 0
   is the frame being drawn and is added to with atomics; slot 1 is the frame before it, complete and
   written by nobody while it is read. Every invocation reads slot 1 so every invocation computes the
   same exposure. Reading slot 0 gives each invocation however much of the frame had run when it
   looked — the same scene exposing differently across the image and differently again on another
   card, which reads as a shading bug.
3. **The control arm is exact and no flag reaches the shader.** `--no-auto-exposure` has the host
   zero BOTH slots every frame, which drives the shader down its own "nothing has been measured"
   branch and applies `kPreviewExposure`. The arm IS the old constant rather than a second code path
   that can drift from it.
4. **`darkroom.ps1` still passes and is a stronger gate now.** Exposure is multiplicative, so a
   sealed black room stays at nought however far the meter winds up — and it winds up to **460×**
   there, so anything the renderer invented would be 460 times more visible than it was.
5. **`--validation` earned its keep on the first run.** `create_device_buffer` grants TRANSFER_DST
   and not TRANSFER_SRC, and the slot rotation is a copy of this buffer into itself. The copy ran,
   the picture looked right, and the layer was the only thing that said otherwise.

**The instrument is `the light meter:` at every screenshot** — the multiplier, the frame's own
log-average in stops, and the workgroup count. An exposure with no printed number would have been
the same fault as the constant with an extra buffer in it.

**What this makes incomparable.** Every picture figure taken before this line is at the fixed 3.2 and
every one after it is metered. The facility moves 2–6%, so it is not the wholesale re-baselining this
file warned it would be, and `--no-auto-exposure` is what reaches the old ones. **`tools\baseline.ps1`
has not been re-run.**

### R5a is in: a face's light is no longer measured alone

**The first thing in this renderer that filters across faces.** Every light term here is a per-face
Monte Carlo estimate, and until now each face argued with its neighbour by its own standard error —
which on a flat wall is the only thing there is to see. That is the class the reported *fine grid on
flat surfaces* belongs to, and D539's eight eliminations found no ninth cause because there was not
one to find. D573–D575.

**Read this before touching it, because the property it rests on is the data model rather than the
filter.** A face is keyed by *(node, level, direction)*. A neighbour at the same level and direction,
one step along one of the two axes the normal is **not** along, is **coplanar and contiguous by
construction** — a change of plane is a change of key and the lookup simply misses. So there is no
edge-stopping weight, no normal test and no depth test: what a screen-space a-trous spends most of
its arithmetic on does not arise here at all. The kernel is a 3×3 tent over those neighbours, three
voxels wide, which at the leaf is 9.4 cm.

| 1280×800, `--settle`, frame 900, two interleaved rounds | `--no-face-denoise` | default |
|---|---|---|
| **close** roughness (mean \|2nd difference\|) | 4.3539 / 4.3434 | **2.9693 / 2.9673** |
| **close** speckle | 35.199 / 35.176 | **27.525 / 27.703** |
| **close** mean pixel | 143.912 / 143.926 | 144.099 / 144.054 |
| **close** faces pass | 3.625 / 3.454 ms | 4.057 / 3.939 |
| **enclosed** roughness | 3.0087 / 3.0107 | **1.7292 / 1.7172** |
| **enclosed** speckle | 12.113 / 12.137 | **7.986 / 7.842** |
| **enclosed** mean pixel | 157.494 / 157.390 | 157.452 / 157.471 |
| **enclosed** faces pass | 2.625 / 2.620 ms | 2.777 / 2.769 |
| **outdoor** roughness, speckle | 1.4783, 15.853 | 1.4568, 14.640 |
| walked out and back (`--cut` twice), roughness and speckle | 3.2157, 13.210 | **2.6859, 10.847** |
| flying at 1440p, faces pass | 7.140 / 6.801 | 6.993 / 7.086 — inside each other's spread |

**The lamp tap is more than half of the enclosed figures on its own** (D576), which is why the term
order was decided by photographing each on its own before anything was built rather than by argument:
sky and bounce alone took that room to 2.4458 and 9.975, and the lamps took it the rest of the way.
The walk-out-and-back row is from the sky-and-bounce build and has not been retaken.

The mean pixel moves by 0.02 to 0.10 of 255 on every camera, against a run-to-run floor of 0.018 and
0.07 — which is what a filter that only takes variance out should do. Gates: 523 tests,
`darkroom.ps1` BLACK clear and with fog, `--validation` clean, all three pool audits clean.

**Five things to know before touching it.**

1. **It reads words 0–11 and writes words 12–15**, and that is not tidiness. An a-trous step that
   reads what it writes is applied again on every visit and blurs without bound until a wall is one
   colour; the usual answer is ping-pong buffers and this is the same answer in one buffer.
2. **Only the composite reads the filtered words.** A gathering ray reads the RAW bounce, because the
   bounce chain is already a progressive radiosity solve over many frames and feeding a filtered
   value back into it is the same unbounded blur arriving through the light transport instead of
   through the buffer. **The filter is a display of an estimate, never part of the estimator.**
3. **Which terms, and the two that are deliberately left alone.** The two answers of the one
   unbounded ray — `open_sky` and the bounce — because they are the slowest estimator here and the
   bounce is a radiance with no bound to average against; and the **LAMPS**, which that photograph
   says are the noisiest term indoors, 23.0 of 255 at the enclosed camera. A lamp estimate is noisy
   because `pick_light` keeps ONE fitting per face per frame in proportion to what it would deliver,
   which is what makes *a face never loops over lights* true and a hall of a thousand sconces cost
   what a hall with one costs — the variance is the price of that constant cost, and it is paid per
   face, which is exactly the shape a coplanar blend removes. Not the SUN: a shadow edge is a real
   high-frequency feature. Not the NEAR FIELD, photographed at 6.0 of 255 and the smoothest thing
   here. **One thing the lamp tap does soften** and it is worth knowing: a sconce's own shadow
   boundary across a single flat plane is a real feature of that term, and 9.4 cm of it is blurred.
   Across a change of plane nothing is crossed at all, because that is a different face key.
4. **A metric defined against a neighbourhood cannot judge a change whose purpose is to move the
   neighbourhood** (D574). The firefly count doubled in the first round and was unchanged in the
   second, and the reading that settles it is absolute: pixels over 250 go 902 → 892 at the steps,
   over 254 stay at 391, and **the enclosed camera's brightest pixel FALLS from 233 to 232 while its
   pixels over 230 go 2,764 → 1,463 and its firefly count goes 18 → 45**. Nothing got brighter
   anywhere. Trap 10 living in the harness.
5. **A roughness figure alone cannot tell a denoiser from a flattener**, so `rough.ps1` reports the
   edge population beside it, split at 24 of 255. The count falls while the mean STRENGTH holds or
   rises, which is only possible if what left was noise spikes rather than silhouettes: enclosed
   91,364 edges at 63.74 → **80,076 at 66.62**, and at the steps 264,454 at 63.39 → 245,697 at 63.29.
   A filter that was flattening the picture would take the strength down with the count.

**What it costs is memory and the close camera's budget.** The face light record goes from twelve
words to nineteen, **50,688 → 80,256 KB**, and it is now the largest allocation in the renderer —
packing the two filtered radiances as half floats would halve that and has not been needed. And the
faces pass reads **3.94–4.06 ms settled at the close camera against a 4.40 budget**, where it read
3.45–3.63 before. Flying is unchanged, which is the case with no headroom, but **the settled margin
is 8% now and the next thing added to this pass should be measured against that figure** rather than
against the flying one.

**What did not land, with its numbers, so nobody re-derives them** (D575). D572 measured
`--secondary-period 32` as brighter and quieter by speckle and worse by fireflies, and said the
answer was R5 rather than a dial. With R5a in, the enclosed room reads mean pixel **157.40 → 163.10**
with speckle **9.97 → 9.44** and roughness **2.4495 → 2.2819** — so the prediction held. It is not
taken here because at the steps the same arm reads the faces pass **3.871 → 4.105 ms** against a 4.40
budget standing still, on a pass already at 7–8 ms flying. That wants its own change and its own
flying measurement.

### Closed: a room's light was capped at a quarter of the store while three quarters of it sat idle

**This is the section that overturned the one below it, and the overturning is the part to read.**
D569–D572. Everything in this file, in `21-renderer-rewrite.md` §8 R9c and in the §8.0 ledger said
the next thing to do was R9c, the halo, because *21.9% of gathering rays land on a surface with no
face in the store at all and that is R9c's and R9g's*. The first half of that is a measurement. The
second half is an inference from it and nobody had tested the inference.

**It took an afternoon of existing flags, because the dials were already there.** Close camera,
1280×800, `--settle`, frame 900, one build:

| | rays landing on no face | landing on a lit face |
|---|---|---|
| default | 21.8% | 29.9% |
| `--secondary-period 16` | 19.3% | 33.5% |
| `--secondary-share 2` | 20.4% | 30.8% |
| the two together, plus `--face-pressure-from 32` | **8.7%** | **41.5%** |

A halo claims faces just off the edge of the screen. It cannot move a number that three dials about
**claim throughput** move by two thirds. Most of that bucket is surfaces a gathering ray *does* name,
whose claim was turned away by a cap or evicted before the next ray got there.

**What landed is the cap** (D570). It was a fixed quarter of the table, and a quarter is a share of
the wrong thing: what the class may safely hold is whatever the on-screen set is not using, and the
on-screen set is 111,377 faces in the enclosed room, 497,880 at the steps and near a million while
flying. So the cap is the table's **spare room** now — everything above the on-screen set, less the
same headroom the pressure rule already reserves — and the two rules agree by construction rather
than by coincidence: a class that fills this cap has taken the table to exactly `pressure_from` free,
which is the frame the squeeze starts on.

| settled, 1280×800, frame 900 | `--secondary-share 4 --no-class-eviction` | default |
|---|---|---|
| **enclosed** off-screen set | 250,302 of a cap of 262,144 | **344,578 of 802,305** |
| ...claims declined by the cap over the run | **222,587** | **0** |
| **enclosed** mean pixel, speckle, fireflies | 150.139, 12.825, 0 | **157.414, 12.168, 0** |
| **enclosed** faces pass | 2.652 ms | 2.613 |
| **close** off-screen set, declined | 231,958 of 262,144, 48,262 | **269,438 of 419,797, 313** |
| **close** rays landing on a lit face | 29.9% | **30.9%** |
| **close** mean pixel, speckle, fireflies | 143.088, 35.848, 36 | **143.893, 35.250, 54** |
| **close** faces pass | 3.560 ms | 3.529 |
| **outdoor** mean pixel, speckle | 161.821, 15.849 | 161.822, 15.833 |

Run-to-run floor from four control runs: close mean pixel spread **0.018**, enclosed **0.07**. So
+7.3 of 255 in the enclosed room is two orders of magnitude outside it, and outdoor not moving is the
right answer rather than a null result — outdoors a gathering ray reaches sky and nothing was missing.
`darkroom.ps1` BLACK clear and with fog, `--validation` clean, all three pool audits clean, 523 tests.

**Three things to know before touching it, and the first is a fault that was measured rather than
predicted.**

1. **Growing the class is only safe with the EVICTION ORDER beside it** (D571). With the class free
   to fill the table the store spends most of its life one step into the squeeze, and at that step
   the old policy gave everything cold up on one clock. Measured without the fix: the coarse pyramid
   went **21,795 stand-ins to 62**, the coarse answer to a gathering ray that found nothing went
   **31.7% to 10.2%**, and the close picture came out *slightly worse* for a bucket that had fallen
   from 21.8% to 13.3%. There are three kinds of record here and they are not worth the same — a face
   a pixel has read is the picture, a face only a light ray has asked for is one bounce sample, and a
   coarse stand-in is what a whole room is rebuilt from at 512 fine faces to one. They are now given
   up in that order.
2. **The control arm is two flags, because it is two rules.** `--secondary-share 4` restores the
   fixed quarter and `--no-class-eviction` restores the single clock; either alone measures half the
   change, which is trap 15 with the flag rather than the harness getting it wrong.
3. **What binds it now is the claim RATE, and the obvious dial was measured and not turned** (D572).
   The class no longer fills its cap — 269,438 of 419,797 close, 344,578 of 802,305 enclosed — so
   `secondary_period` is the constraint. At 32 the enclosed room is another **5.6 of 255** brighter
   and quieter by speckle (12.13 → 11.39) and goes from **no fireflies to eighteen**; at 16, to
   eighty-one. Feedback holds in every arm. A term that trades a mean against outliers wants R5 built
   before the dial is turned, and `--secondary-period 32` is one flag for anyone who disagrees.

**What did not happen.** Flying is neutral — `_flybench.ps1` at 1440p, two interleaved rounds, faces
7.579 / 7.434 against 7.067 / 7.791 — which is the expected answer, since flying the on-screen set is
what fills the table and the cap collapses on its own. **And the coarse pyramid still loses about
22,000 stand-ins over a flight in BOTH arms**, so something other than the ordinary sweep is spending
them there. That is unexplained, it is not this change's doing, and it is the next thing to find in
this pass.

### R9 is finished: the fold is in, measured, and opt-in

**R9f's fold, the last piece** (D590). A coarse face takes its sky, its near field and its bounce
from the four faces under it pointing the same way, pulled on a visit it was making anyway — four
lookups, one writer, no atomics, which keeps D191's *one invocation owns each face* rather than
arguing around it.

| close camera, settled, two interleaved rounds | `--no-face-fold` | `--face-fold` |
|---|---|---|
| gathering rays landing on a **lit** face | 31.0 / 31.1% | **41.8 / 41.8%** |
| faces pass | 3.66 / 3.72 ms | **6.43 / 6.57** |

A third more of what the bounce integrates finds light instead of black, and the pass goes 75% over
a budget it was inside — so it is **off by default**, the same call D586 made for the halo.

**Three attempts to make it cheaper, all measured, none of which worked**, and the third is the one
to keep: folding one visit in eight cost **more** (7.5 against 6.4) and won less (36.3% against
41.8%), so whatever this costs is **not proportional to how often it runs**. Look at what it makes
*other* passes do rather than at its own arithmetic — a coarse face that is answerable is suddenly
read by `visibility.comp`'s stand-in path and by every gathering ray that walks up.

**And one figure to distrust**: the control arm read 3.66 ms in one round and 4.02 twenty minutes
later, which is D407's machine drift. Only interleaved rounds are evidence here.

### R9g's persistence is in, and R9h needed a correction rather than code

**The lamps come back with the world** (D588). A chunk's emissive cells are written into the world
cache beside the region index, so a loaded world knows where its lamps are instead of reading every
brick to find out: the rebuild at load goes **14.2 ms scanning 74 chunks to 0.09 ms scanning none**,
same 21 emitters, same list version. Two things about it are trap 7 and both would be silent: an old
file carries no emitters and that means *unknown*, never *none* — the other way is a building that
loads with its lights off; and the file is written by scanning anything not already known rather
than by dumping whatever is in memory, or its contents would depend on where the camera stood while
it was built, which is R9's own fault arriving through the cache.

**R9h was measured and mostly needs nothing** (D589). Its three claims have three answers. *The
analytic sky past the last node* is already done — a gathering ray that leaves the world returns
`sky_radiance`. *The coarsest folded colour* is worth **3 gathering rays of 482,773** on the close
camera under continuous editing, which is the most unbuilt geometry this engine can be put in; it is
not built, and the number is here so nobody re-derives it. Giving those rays a colour needs an
irradiance to multiply it by, there is not one, and inventing it is D541–D543's deleted light floor
arriving through a third door — `darkroom.ps1` is the gate that would catch it.

**What was actually wrong was the rule.** *"No light path may cause streaming"* is stated as
absolute with R9a as the single deliberate exception, and **R9i is a second one that postdates the
sentence**: a shadow ray reports the cell that stopped it, and D430 lets it say it is using that
cell. Both are deliberate, both are bounded to one entry per node per window by `node_seen` (D431),
and over a settled run the pool builds **1 node** with them on against 4 with
`--no-light-keeps-geometry`. The rule now reads *a light path may name the one cell that stopped it
and the one face it landed on, never what it crossed* — which is what the code does. **An absolute
the code deliberately breaks in two places is worse than no rule**: the next reader either "fixes" a
working mechanism or stops believing the document.

### R9g: the two faults it names cannot happen here, and the one it does not name cost 14 ms an edit

**Asked for as "do R9g", and one run said the stage was written against a symptom this engine cannot
produce** (D587). §8 R9g's case is that a lamp in an unloaded region does not exist and one past the
cap blinks out. The facility has **21 emitters against a cap of 1,024**, and nothing anywhere
unloads a chunk from `World` — `chunks_` is erased only when a chunk is emptied. Neither is
reachable.

**What was real was standing beside it, unprinted.** `build_light_list` walks every brick of every
chunk, and `announce_world_change` sets `lights_dirty_` — so it ran on every chisel stroke and every
region the ladder pastes:

| facility, `--chisel 20,16` | `--no-emitter-cache` | default |
|---|---|---|
| each rebuild | **13.99 / 13.99 ms** | **2.54 / 2.48 ms** |
| chunks rescanned by the last | 74 of 74 | **8, 66 reused** |
| emitters, list version | 21, v2 | 21, v2 |

against the edit's own **0.19 ms** to apply and undo. Finding the lamps was 74× the cost of the edit
that provoked it and the largest single thing a chisel stroke spent on the CPU. **That is
`rebuild_coarse_grids` — O(world) for a change one metre across, D522 — four times over, three
stages later, in a different file.** The class is worth more than the fix: *anything rebuilt from
the whole world on an announcement is this*, and two have now been found by timing the suspect
beside the thing it was reacting to.

**Why the split is safe, and it is arithmetic rather than care.** A cluster cell is four voxels and
a chunk is 256, and **4 divides 256 exactly**, so no cell straddles a chunk and two chunks' cells
have disjoint keys — the cached halves concatenate. The MERGE is not cached and must not be: it
ranks and caps globally, and a fitting may straddle a boundary. The gate is identity, not
plausibility — `light_list_hash` against the whole-world scan, on a world built to contain a
straddling fitting.

### R9c is built and switched off, and the measurement is why

**The premise had never been measured and half of it was wrong** (D585, D586). The plan says a pan
*"reveals unlit geometry that then lights over several frames"*. It reveals no unlit geometry at
all: the full-sun fallback is **nought pixels of 605,945** at every band, panning or standing still,
because R3e claims a stand-in in the pass that discovers it and R9d reads the coarse face three
levels above. What is wrong is the *quality* of what arrives.

**How to measure a pan, because three ways of doing it produced numbers that measured something
else.** Two pan RATES end at different poses. A pan against a static camera at the computed end yaw
ends at a different pose too, because the camera moves while the world settles — that pair read 8.09
of 255 apart with **54% of pixels landing on a different face**, which `--debug-mode 11` says in one
run. What works is **two arms arriving at ONE pose from opposite directions**, pinned with `--cut`
so the settle frame drops out: cut to 90−195 and pan +30, against cut to 90+195 and pan −30. Then
`tools\bands.ps1` splits the frame into vertical bands, because a deficit down one edge is a
rounding error in a whole-frame share and is the entire fault.

| ambient convergence by band | `--no-halo` | `--halo` | `--halo-lead 96` | *trailing edge, same pixels* |
|---|---|---|---|---|
| band 7, leading | 14.04 | 18.18 | 21.25 | **88.8** |
| band 6 | 40.40 | 47.76 | 53.79 | 90.4 |
| band 5 | 78.65 | 93.25 | 107.43 | 97.5 |

**It works, it costs no frame time, and it is off by default.** Faces pass interleaved while panning:
20.897 / 21.197 against 21.011 / 20.631 ms — inside each other's spread, and the reason is not what
D566 predicts: a halo does not create rays, it **moves them earlier**, so the total over a pan is
unchanged. What it costs is `sun stride 6` in both control runs and **7 in both halo runs** — every
face on screen refreshing 17% less often, for a quarter of one edge's deficit. That is D527 and D557
a third and fourth time, it is invisible in a pass table, and the rule those two wrote down is the
only reason it was caught. **`--halo` is opt-in until a halo face is counted in a class of its
own**, which is R9b's existing machinery rather than a new idea.

### R4a is in: a face knows what it is made of, and nothing looks different yet

**The user was asked nothing and said it anyway: *"its not halo, its directional faces"*.** So R9c
is not next; R4 is, and this is its first sub-step. D582, D583.

**What was in the way, and it is not what the plan's one-line description says.** A face is
*(node, level, direction)* and nothing else. The store has never known what the surface under a face
is made of and neither has the light pass — `bounce_face_light` reads the **folded average colour
the marcher carries** and its comment says why: this pass has no binding for the interned tables. An
albedo is the whole of what a Lambertian face needs. A lobe is roughness and metalness, and those
live in the visual record and nowhere else, so the first thing R4 costs is getting two bytes to a
place that could not ask for them.

The two tables are bound to the node set now, and a face descends to its own brick **once** and
keeps the answer in a card-owned word a slot. It is the fourth array with that exact guarantee after
the face light and the three stamps, and it is not `GpuFace::bins` — the field the plan reserved for
this — because that field is on the **host's** side of a record with two owners (D295, D528), and
because the host could not fill it either: the claim path has a key and no world lookup.

| two flags of one build, interleaved | `--no-face-materials` | asking |
|---|---|---|
| close camera, settled, faces pass | 4.013 / 4.037 ms | 4.061 / 3.994 |
| flying at 1440p, faces pass | 7.867 / 7.433 | 7.387 / 7.032 |
| sun samples a face, flying | 18 / 19 | 18 / 19 |
| faces a pixel is reading, flying | 213,922 / 213,884 | 213,982 / 213,903 |

The arms interleave. The last two rows are there because of trap 20 — a timing alone cannot tell a
pass that got faster from a pass that stopped doing its job — and this change could not have made
anything cheaper, so equal convergence is what says both arms did the same work.

**The census is the part to keep, because it sizes everything after it.** `what the faces are made
of` and the line under it are printed at every screenshot, and the store is emphatically not the
population that matters:

| | carry a material | roughness quarters | some metal |
|---|---|---|---|
| enclosed, whole store | 372,854 | 455 / 43,776 / 292,118 / 36,505 | 25,352 |
| enclosed, what a pixel reads | 111,373 | 0 / 21,184 / 87,079 / 3,110 | **14,130** |
| close, what a pixel reads | 416,143 | 9,293 / 34,123 / 229,273 / 143,454 | **22,158** |
| flying at 1440p, what a pixel reads | 214,983 | 7,813 / 13,870 / 63,586 / 129,714 | **7,479** |

**Three things to know before touching it.**

1. **Materials in this game are continuous and per voxel, and nothing in R4 may branch on a material
   identity.** A clip writes `rough=64 metal=225` as free numbers and `sample.cpp` nudges them per
   voxel from a hash of where the voxel is; there is no material enum in the engine to switch on.
   That is why the census reports a **distribution** rather than a count past a cutoff — an
   instrument that picks a threshold is how a threshold reaches the shader six weeks later, and the
   plan forbids one anywhere in this stage.
2. **A coarse face has no material and says so, in a third state.** Encoding it as *known, roughness
   nought* was the first shape of the word and roughness nought is a **mirror**: every coarse face
   in the store would have read as polished chrome, in the census and as bright green in view 21,
   and at the outdoor camera nearly every visible face is coarse. Caught by running the new view at
   the camera it would look worst on before believing any number from it (D583). Near the camera
   this costs nothing — the close camera reads 416,143 level 0 of 416,143 — and a **distant** dome
   will stay matte until R9f's fold exists.
3. **Nought in that word must never become sticky.** A face over a brick the pool has not built
   resolves to nought and asks again next visit; a face over polished granite must not. Trap 7, in
   a new array.

**What is next, and it is the one a player will see**: R4c, the outgoing bins. **Done — see the
section below, and read it before believing the sentence that used to be here.**

> **This paragraph over-promised and the next section is what it cost.** It said the bronze doors
> and the gilt paterae *"will show the portico and the sky in them"*. They do not, and the first
> thing the user said on being shown the result was *"i dont see any reflection in any picture"*.
> Bronze at `rough=110` is a lobe 10.7 degrees across and a brushed bronze door does not mirror a
> portico in life either — but the promise was made in this file, in those words, and it should not
> have been made from the plan's prose without a number beside it. D592.

### R4c is in: the metals stop being Lambertian, and there is still no reflection

**The energy split landed, it costs nothing, and the picture moved in the direction it was asked to.
The reflected IMAGE did not arrive, the user said so immediately, and D592 is the measurement of
why.** Read that entry before sizing R4b; it changes what R4b is.

**What a player sees now.** Walk up to the great bronze door on the portico, or look at the gilt
paterae under the entablature, or the lead flashing and the copper dome. Before, every one of them
was drawn as chalk with a colour on it: one albedo, the same in every direction. Now what leaves a
metal is split — the diffuse loses the metal's share, and what it loses comes back as a lobe. The
doors read as darker, deeper, more saturated bronze with tonal variation across a panel, the paterae
read as gold discs rather than pale blobs, and the window glazing bars stop being cream. A sunlit
metal also gets a proper highlight that slides across it as you move, which needs no storage at all.

**What a player does NOT see, and it is the thing they asked about first.** There is no recognisable
reflected image anywhere: no portico in the door, no building in the water. Three separate causes and
they are worth keeping apart, because only one of them is a choice that can be changed cheaply:

- **sixteen bins is 20.4 degrees of half-angle**, and no image survives that;
- **this building's metals are genuinely rough** — copper at 24 degrees and lead at 25.4 are
  *wider than one bin*, so for those two the bins are already the right resolution;
- **the near-mirrors are dielectrics and are shut out of the pool.** Glass and water are the only
  polished surfaces in the facility and `face_lobe_worth` ranks them at 0.040 against a floor of
  0.050. `--lobe-floor 0.038` admits them.

**The experiment that settled it is the part to keep.** 256 bins — 5.1 degrees — were built,
measured on the water basin at a grazing angle where a dielectric reflects hardest, and **reverted**:

> **A bin is filled by the face's own gathering rays and there are about five hundred of them.**
> `kBounceMin` is 512, so sixteen bins get thirty-two samples apiece and 256 get two. Angular
> resolution is bought one for one with noise, and the currency is rays.
>
> **And the rays are cosine-weighted about the normal while a reflection is read at a GRAZING
> angle.** The density at 80 degrees is 0.17 of the peak, so the bins a visible reflection comes out
> of are the emptiest a face has. Water across a pool is where a reflection is most visible and
> where this sampling serves it worst.

So a sharp reflection is **R4b plus a second ray**, not two constants. It is one ray for one face in
nineteen — 22,158 of 416,143 at the close camera carry any metal — and it is sized by the census R4a
was built to produce.

**The instruments, and use them before believing anything here.** `--debug-mode 22` is the pool's
residency: green holds a block and has measured into it, yellow holds one and has not, **red asked
and was turned away**, dark grey is a face whose lobe is not worth a block, cyan a coarse face and
blue one that has not looked yet. `--debug-mode 23` is the lobe on its own, tone mapped, with the
diffuse taken away — that is the view the claim above is checked against. The audit prints *the lobe
pool: N faces holding a block of 65,536, M asked and were turned away, K taken over from another
face*, and the third number is the one that separates a full pool from a thrashing one.

**The control arms are run-time flags, so D407 is satisfied by construction.** `--no-face-lobe`
takes the bins away and leaves everything else (a metal then reflects its hemispherical mean, which
is the same lobe with one bin in it); `--no-face-materials` takes the whole of R4 away and is the
picture as it was; `--lobe-floor N` is the pool's capacity dial, in worth, and `--lobe-floor 0` gives
a block to every face that knows what it is made of.

**Three things to know before touching it.**

1. **A coarse face has no material, so it has no lobe.** At the outdoor camera **one** face in the
   whole frame holds a block, because at 60 m the dome is drawn from level-1 and coarser faces.
   Distant metal is matte and will stay matte until R9f's fold carries a material up the tree.
2. **The diffuse share is applied by BOTH readers and the lobe by only one.** `face_terms.glsl`
   exists so the composite and the gathering ray cannot disagree, and they do not — about the
   diffuse. The bounce does not read a lobe, so **a mirror still does not light the room**; that
   errs dark, which is the direction this renderer errs in, and it is what R4c's remaining half owes.
3. **The pool is a cache, not an allocator, and that is deliberate.** A face has no destructor: the
   store hands a slot to a different face and nothing tells the card. A block is *held* — four-way on
   the slot, taken when a way is free, cold at 600 frames, or held by a face worth less — so a
   forgotten block is recovered rather than leaked. A DECLINE is not a black surface; it is a face
   reading its hemispherical mean.

### R4b's ray is in, and the reflection has something in it at last

**Asked for directly, after D592 measured that a cosine-weighted ray cannot fill the bins a
reflection is read out of.** A face that holds a lobe now casts a ray of its own: it picks one bin
round-robin, draws a half vector from that bin's own kernel, reflects the bin's direction about it,
and marches unbounded. Taking the view direction as the BIN rather than as the normal is the whole
of what puts samples at grazing angles.

**In game:** the great bronze doors read as deep metal with panel structure and gilt bosses where
they were a flat warm wash, and the window glass gains a sky-coloured sheen. Against `--no-lobe-ray`
— the same pool, the same bins, the same energy split, only the march removed — that is **3.913 of
255 over 103,874 pixels at the door** and 2.090 over 46,690 at the close camera.

**What it costs:** nothing measurable standing still (close faces **4.229 ms** against the no-ray
arm's 4.218 and pre-R4's 3.879) and **1.5 ms flying** (9.318 against 7.825), on a pass that is over
its 4.40 ms budget in the moving case either way.

**Three tunings, and none of them was a preference. Read all three before changing a constant here.**

1. **The burst is eight rays a visit, not thirty-two, and that REVERSES D394 for this term.** D394
   measured every attempt to meter the ambient burst as making the transient worse, because what
   that pass spends on an unconverged face is mostly the face and not the ray. The difference is
   that an ambient near ray is **bounded at a metre** and a lobe ray is **unbounded**: at
   thirty-two a visit the flying case read **11.931 ms against 7.825** and cast 270,853 rays a
   frame, 71.5% of every gathering ray in the picture. And D394's population is bounded and
   converges, so letting it measure hard empties it — a flying camera refreshes the lobe population
   continuously, so there is no state to get out of and the per-frame rate is the whole cost.
2. **A warm block is only taken by a face worth 1.5 times its holder.** A take-over zeroes the
   block's sample count, so its holder starts its burst again; without a margin two faces of nearly
   equal worth sharing a set trade one for ever. Measured: **417 blocks changing hands a frame and
   883 faces still bursting on a settled camera**, with the pass 2.2 ms over where it should have
   been. With it, 13 and 249. **That is what the third counter on the audit line is for** — a pool
   that is thrashing and a pool that is full read identically in the first two.
3. **Thirty-six bins in a pool of 131,072, not sixty-four in 32,768.** The worth floor is 0.038 now
   so that the glass and the water get lobes, and that takes the population asking to about
   **44,700** — 47% of askers were turned away by a four-way pool of 32,768. A smaller block buys
   four times the blocks and eight ways for 38.9 MB, takes declines to **0.8%**, and cuts the rays
   a face needs over its life from 1,536 to 864. It costs 13.5 degrees of blur instead of 10.2.

**The mottling that was owed here is closed** (D595). Thirty-six bins at twenty-four samples is a
per-bin estimate of a radiance, so the lobe on its own was visibly grainy face to face. It is the
fourth term to want `face_denoise`'s idea and it needed no rays: a lobe is blended with its coplanar
neighbours' bin against the same bin, **once**, on the visit after it converges and marked in the
block's header — because there is no room for a second copy of thirty-six bins and the only thing
that bounds a filter reading what it writes is that it runs once. Measured on the lobe alone with
`--debug-mode 23`, the speckle at the great door goes **21.73 → 14.75**, and it costs 4.292 ms
against 4.229 settled and 9.865 against 9.318 flying.

And the bin count still does not follow **pixel coverage**, which is D186's own sentence and the
half of R4b that is not built. A mirror filling the screen gets the same thirty-six bins as one
across the room.

### R4d is in: light passes through a window, and so does the eye

**Two commits, and the first one on its own looked finished and was not.** D602 gave the marcher a
`see_through` flag and turned it on for the shadow, ambient, gathering and lamp rays: a window stopped
blocking the sun, and a wing hall that had been drawn **black** by the lamp term alone became daylit —
95.577 of 255 over 1,018,413 pixels of 1,024,000, the largest single number this stage has produced.
D603 then found two faults in it one commit old: the tint the marcher accumulated was **read by
nothing**, so coloured glass tinted nothing; and the attenuation was applied **per voxel** where the
number is a property of the material, so a four-voxel pane transmitted 0.32 and the hall went dark
again with the light meter pinned at its ceiling. Opacity is taken over a **metre** now and rooted
down to the voxel, the same as `absorb` in the same record and for the same reason.

**D604 is the other half: the primary ray.** The flag stayed OFF for it deliberately — a face is
claimed where a pixel lands, so a window the primary ray passed straight through would have no face,
no sun, no lamps and none of R4c's lobe. The pane was still drawn as a flat milky panel. What that
needed was the visibility buffer carrying **two** surfaces, and it now does: one extra `node_march`
per glass pixel with `see_through` TRUE, into a new `rgba32ui` image on binding 26 that
`visibility.comp` writes and `resolve.comp` alone reads. `out_face` stayed r32ui — widening it would
charge every pixel in the frame for the few per cent with glass in front of them.

**Both surfaces go down the same shading path.** `resolve.comp`'s 533-line surface shading was lifted
out of `main` into `shade_surface` and is called twice, the far layer first so the near pane wins the
shared lobe state. Faking the far layer instead gives a window you can see through onto a world lit
differently from the one beside it, which reads worse than the panel did. The composite is
`(1−T)·diffuse + specular + emission + T·behind` — **only the diffuse term is attenuated**, because a
reflection happens at the face and a lamp's glow is emitted by it.

**In game:** the window that was one frosted panel is fifteen separate lights in a five-by-three
grid, with the wooden glazing bars and transoms reading across them, daylight through the panes and a
second window at the right edge showing through as well.

**What it costs**, both arms at the same world hash, settled, 1280×800 quality 7:

| camera | before | after | mean of 255 | pixels past 8 |
|---|---|---|---|---|
| **window** `13.5,3.6,5.0,90,0` | **5.147 ms** | **5.393 ms** (+4.8%) | 19.85 | 707,823 of 1,024,000 |
| outdoor | 4.240 | 4.172 (noise) | 0.16 | 2,549 |
| enclosed | 6.603 | 6.603 | 2.42 | 33,949 |

All of it lands in the visibility pass, 0.737 → 0.966 ms, and it is **charged per glass pixel**: the
two cameras with almost no pane on screen cannot tell the arms apart. That camera is not in
`tools\_grid.ps1` — none of the seven canonical views has a pane close enough to judge, so it is
written down in D604.

**Two traps this stage cost, both in the measuring and not in the code.** `--no-see-through` is **not
a control arm**: it disables the light rays' transmission too, so the room comes back black and cheap
for reasons that have nothing to do with the change. Stash the three files, rebuild, measure, pop.
And the **clip cache advances between runs** — it went 9 of 18, 17 of 18, 18 of 18 across one session
with three different `content` hashes, and two runs at different hashes are not comparable at all.

**What R4d still owes:** there is no Beer-Lambert over the true path length, only opacity per metre
rooted to the voxel, and the `absorb` bytes are unread. Refraction bends nothing — `ior=1.5` is
carried by the material and used by no ray, so a pane displaces nothing behind it. And if `through`
falls below the marcher's 0.02 continuation threshold inside the glass, the second march stops on a
deeper glass voxel; that layer is then weighted at most 0.02, so the error is bounded at two per cent.

### Where to start now, and the two orders are not the same order

**By the plan, the next stage is R4 — and R4's own prerequisite is R9.** §8 puts R4 directional
faces after R3, and R9's opening says why it cannot be first: R4c reads *other faces* for a
reflection and R4d for a refraction, and the store holds only what a primary ray landed on. A mirror
facing a wall behind the camera reflects nothing, because the wall has no face. §8 R9 has the table
of five cases and the rule that fixes them — *the face set is the transitive closure of what the
screen sees, one bounce at a time, bounded by a budget per bounce*. R9d is already done (D308–D311)
and R9i's first half with it (D324, D341–D343); **R9a, R9b and R9e are done too** (D526–D532), and
**R9f's outlive half** with them (D554–D560). What is left is **R9c** the halo, **R9f's fold**,
**R9g** the emitter list, and **R9h** the fallback.

**The order inside what is left, now that the probe can price it — and this paragraph has already
been wrong once, so read the probe before following it.** It used to say R9c was the cheapest thing
that moved the first number, and that was true of the number and false of the picture: R9c claims
off-screen faces, and off-screen faces held nothing until R9b's ray share landed (D561). With that in,
the two buckets are no longer the same size and the order is no longer a judgement call:

- **21.9% of gathering rays land on a surface with no face in the store at all** — unmoved by R9b,
  and now nearly all of what the bounce still finds black. **This is R9c's and R9g's**, and R9c is the
  cheaper of the two;
- **1.7% land on a face that has measured nothing yet**, down from 12.4%. Nothing left there.

So **R9c is still next, and now for a reason that has survived being measured** rather than for one
that a single audit line could overturn. Size it against D566 rather than against how many faces the
margin holds: a halo face is an off-screen face, and lighting one is priced by a tail. The fold is the
*accuracy* of what R9f already returns rather than more of it, so it is after that unless a picture
complains.

*(Read the two notes under this before acting on that sentence. It has now been wrong three times,
and the third time it was a reader who had not written any of it who noticed.)*

> **The paragraph above was wrong for the second time and is left standing on purpose** — see *a
> room's light was capped at a quarter of the store* above, and D569. "This is R9c's and R9g's" was
> never measured; it was inferred from having explained the other three buckets. Three run-time dials
> about claim throughput take that 21.9% to **8.7%**. The first of them is now the default (D570) and
> the rest is D572's. **R9c is about the entry side of a pan and always was**; what it is *not* is
> the answer to a settled camera's black bounce, and the general form of the mistake is that an
> attribution nobody has measured is a guess however many alternatives have been eliminated.

> **The user said otherwise, which the sentence below invites**: *"its not halo, its directional
> faces."* R4 is the stage in progress and R9c is not next. It is not blocked either — R9a, R9b,
> R9e and R9f between them already put measured light on off-screen faces, which is exactly what a
> reflection ray needs to land on. See *R4a is in* above.
>
> **Then they asked why R9c was ever being called next, since it looked done — and most of it is.**
> The code has no halo in it: the only widened frustum anywhere is `kExploreMargin`, which pads the
> ray CLIP BOX for streaming and has nothing to do with claiming a face. But three of the four
> things somebody means by "the halo" have shipped under other names, and the fourth has never been
> measured:
>
> - **the EXIT side is built and this plan says so in its own words** — a face stays in the store
>   for `cold_frames` after it leaves the screen, and §8 R9c reads *"that is most of what is wanted
>   from reprojection and it exists"*;
> - **arriving lit is built** — R9d (D308–D311) has a face with nothing of its own read the coarse
>   face three levels above it, so newly revealed geometry is lit on the frame it appears rather
>   than flashing. That is the symptom a player would call "the halo problem", and it is closed;
> - **the bucket R9c was justified by is mostly gone** — 21.9% of gathering rays landing on no face
>   at all reads **8.7%** under three dials that already existed (D569), and the first is the
>   default (D570);
> - **what is genuinely left is the entry side of a PAN, and nobody has measured it.** No number
>   anywhere in this file says how much of a turning frame is drawn from faces that were claimed
>   after they came on screen. `--fly 0,0,0,N` is a pure yaw and would answer it in one run. Until
>   that exists, "R9c is next" is an attribution nobody has measured — which is the mistake the
>   note above this one is about, arriving a third time.

So the plan's sequence is **R9, then R4**, and that is the one to follow unless the user says
otherwise — it is what makes reflections, refraction and bounce possible at all, which is the half
of *"everything is per voxel face based — even reflections and those things"* the rewrite has not
delivered yet.

**The change this section used to specify — a gathering ray reading the face it lands on — is done**
(D533–D538), and so is the clause it turned out to be hiding: there is no minimum light anywhere any
more (D541–D543). Both are written up above. What is left of R9 is **R9c**, the halo, and
**R9f–R9h**, light from regions that are not loaded; then R4.

**One prediction in this section was wrong and the correction is worth carrying forward.** It said a
single measured bounce would be *dimmer* than a constant of 0.5, that interiors would darken, and
that the change therefore collided with the exposure meter. Outdoors it is the opposite by a wide
margin — sunlit stone bounces far more than the constant stood in for — and on the facility the mean
pixel barely moves at all. **The exposure item is now independent of this and is still open**:
`kPreviewExposure` is the constant 3.2 with no writer, which is why `clips/many_lamps.clip` comes out
blown white.

**The other order is by which open items have a measurement behind them**, and it is a different
list. Do not confuse the two, and do not quote one as though it were the other:

- **R5, the face denoise.** Three measured items point at it: the ambient term's face-to-face
  variance (§5, at the noise floor and not a fault to find), `kSkyConverged` falling from 2,048 —
  **5.05 → 3.55 ms at 512** — and the enclosed room's last open gate clause, 1.35× outdoor on the
  Deck against the 30% the plan asks for;
- **R6's exposure meter, which now has no writer at all.** `resolve.comp`'s `kPreviewExposure` is
  the constant 3.2, and the frame-statistics buffer went with R1e having been written by nobody
  since R3d deleted the tracer. That is why `clips/many_lamps.clip` comes out blown white. It wants
  its own change and its own baseline, because fixing it makes every screenshot in this project
  brighter at once;
- **the undo capture**, 194 ms into 538,169 inverse ops on a large edit, which is now the largest
  single thing an edit costs (D515);
- **sorting the face work list by Morton code**, which cannot change a pixel and has never been
  measured;
- **R10's open item**: 6.0% of surface held short of convergence by unbuilt geometry, which needs an
  instrument counting ambient ignorance by level before either of its two obvious fixes can be
  tried, because both are known-bad without one.


### R3d, and what deleting it turned up

**Deleted** (D517, D518): `shaders/pathtrace.comp` and `shaders/pt_normals.glsl`, the rgba32f
accumulator and its barrier, the camera-moved test that existed only to reset it, `--pathtrace`, F4
and every `path_trace_` branch. 3,297 lines out, 52 in. Warm start unchanged; the cost it carried
was a **cold driver shader cache**, where the before-build read 8,053 ms and then 551 for a second
run of itself. Picture untouched, `--validation` clean, 548 tests.

**Two of the five findings below turned out to matter and are now facts rather than warnings**: the
descriptor set is kept whole because the cloud pass shares it, and `world.glsl` is down to one
includer. **The one that bit** was not in the list at all — removing a descriptor write means
renumbering the `write_count` literals a few lines under the array, and `--validation` is the only
thing that says so (D518).

**Still owed from R3b**: the `GpuFace` split. **Not R3d's to close**: the gate's *no per-pixel
random numbers* clause, whose last holdout is `hash_u32` in `resolve.comp`'s ordered dither — R5c
deletes that dither by name.

### What the scoping pass found, kept because R1e inherits most of it

**R3d is the next step and R1e is the one after it** (see the section below for why that order).
None of this is built; what follows is what a scoping pass turned up, recorded because every item
is something the plan's one-line description does not say and each would cost a session to
rediscover.

1. **The descriptor layout is shared with the cloud pass on purpose, so it survives R3d.**
   `pathtrace_layout_` / `pathtrace_set_` are built at `src/app/main.cpp:5687` and the comment there
   is explicit: *"They share this layout whole, because the cloud pass needs the parameter block and
   the sun and nothing else, and a set of its own would be the same set with holes in it."*
   `clouds_.create(...)` passes `pathtrace_layout_`. So R3d **renames** these to what they will then
   be — the cloud pass's — rather than deleting them. Trimming the bindings the tracer alone used is
   separate, optional, and wants doing after it builds rather than during.
2. **Deleting the tracer removes the WRITER of the frame-statistics buffer.** `resolve.comp:151`
   says so in as many words: *"The tracer's frame_exposure() reads the frame statistics buffer it
   fills itself, which this pass has no binding for and no business writing"*, which is why
   `kPreviewExposure` is the constant 3.2. R6's real exposure meter therefore needs a new writer,
   and R3d is where that dependency is created. It is also the reason `clips/many_lamps.clip` comes
   out blown white today.
3. **It removes the only ground-truth renderer.** The blown-white case above was diagnosed *by
   comparing the face pass against `--pathtrace` on the same camera*. After R3d there is nothing in
   the tree that computes the same picture a second way. That is the plan's intent (§9 deletes it),
   but it means anything wanting a reference comparison should be measured **before** R3d lands, not
   after.
4. **The include graph says exactly what dies with it.** `pathtrace.comp` is the *sole* includer of
   `pt_normals.glsl`, `pt_material.glsl`, `pt_sampling.glsl` and `pt_post.glsl`. Only the first is
   named for deletion; the other three become orphaned files that nothing compiles, and §9 wants
   `pt_post.glsl`'s exposure meter and tone curve kept for R6. Leave them, and say in the commit that
   they are R6's inheritance rather than dead code, or the next sweep deletes them.
   `pt_sky.glsl`, `pt_clouds.glsl` and `pt_media.glsl` are shared with `clouds.comp` and
   `resolve.comp` and must stay. `world.glsl` is left with one includer, `visibility.comp`, which is
   R1e's.
5. **`path_trace_` is not one branch around a dispatch.** It is interwoven with barriers, the
   accumulator's demote-on-edit rule and the face cache's clear: `src/app/main.cpp` 4219 (accumulator
   reset on residency change), 4429 (`node_writes_faces`), 4473 (the accumulator barrier), 4977
   (the face-cache clear), 5045–5077 (the dispatch) and 5247. The rgba32f accumulator and
   `trace_samples_` go with it; check each barrier rather than deleting the block around it.

**The debt R3d carries**, from R3b: split `GpuFace` so the CPU's half and the card's half are never
in one copy, which is what makes D295 — the upload coalescing across clean records and sending the
CPU's zeroed counters over what the card wrote — unrepresentable rather than merely absent. The
split line is already known: `x, y, z, packed` are the host's and `irradiance, photons, counters,
bins` are the card's, and **`src/gpu/face_light.*` already exists as the card-only home** (built for
R10a, D325, for this exact reason), so there is somewhere for the second half to go.

### R3 comes before R1e, deliberately

R1e's bulk is moving `pathtrace.comp` from `world.glsl` onto the node pool — and §9 of the plan
**deletes** `pathtrace.comp` at R3 and replaces it with the face pass. Doing R1e first is building
something to throw away one stage later, so R3 goes first and leaves R1e with nothing to port
(D278). What that costs is the chunk system staying in the build, which since it was resized to
what still read it is 226 ms of load rather than 1.7 s. The "12 ms of CPU a frame" this used to
say alongside it was never measured and was wrong — see D522, where the instrument reads 0.003 ms
mean with a 12–15 ms worst.

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

### R1e — delete the old addressing (done)

What it was: delete `src/world/residency.*`, `thumb_cache.*`, `thumbnail.*`, `summary_tree.*` and
their tests; delete `shaders/world.glsl`'s chunk walk, the coarse grids and the thumbnail tiers;
fold `node_visibility.comp` into `visibility.comp` and drop `--chunk-marcher`; and trim
`src/gpu/world_buffers.*` to the two interned tables, which is `src/gpu/type_tables.*` now.

All of it is done — D521–D525, and the section above says what the plan for it got wrong. Two
things in it stopped being true before it landed: the path tracer did not have to be ported,
because R3d deleted it (D278), and the enclosed-room regression it said to settle first was settled
by R1h, which is why there was nothing left to compare the old marcher against.

*Gate: the grid table does not move, with those tests deleted rather than disabled.* Met against an
interleaved same-session control rather than against the recorded file — see D523, and trap 19 for
why the file could not answer.


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

**What is left of R3c**: nothing. The sky became R10 and landed first; the lamps are below; bounce
was always R9's. **R3d** deletes the per-pixel light path, and carries one debt from here — split
`GpuFace` so the CPU's half and the card's half are never in one copy, which is what makes bug 5
impossible rather than merely absent.

### R3c — the lamps, and how a converged face is made to notice one going out (done)

D401–D409. Emissive fittings are aimed at from the **face**, not from the pixel: the same estimator
`pick_light` runs in the reference tracer — score `kLampCandidates` fittings by what each would
deliver here unshadowed, keep one in proportion, draw a direction inside its cone, correct by the
density that chose it — accumulated into the face's own record as irradiance. **A face never loops
over lights**, so a hall with a thousand sconces costs a face what a hall with one costs, and the
term converges at `kLampConverged` and then casts nothing at all.

**Measured**, enclosed camera, 1280×800, quality 7, `--settle`, 300 measured frames, against a
same-commit control built with `kLampConverged = 0`, both on scene hash `766f2fd63f1a01c4`, **three
interleaved rounds with a rebuild between every run** — see the warning below, which is the part to
read first: settled faces **2.613 → 3.075 ms mean** and 4.54 → 5.21 worst, total GPU **5.706 →
6.217** (+9%), with `lamps on the card: 476,700 of 476,700 live faces cast no more rays at all`. So
**+0.46 ms, +18% of the pass, inside its 4.40 ms budget** — and it does not grow with resolution,
which is the whole claim. The picture moves by **21.78 of 255 over 261,393 pixels of 1,024,000** —
almost all of it the portico, which is the one place in this building the sun never reaches and
which was lit by a constant until now.

> **Do not compare two builds of this pass from adjacent batches.** The same build read **2.41 ms
> and 3.75 ms** on the faces pass over one session, on one scene, with the store converged in both.
> The pass is a function of how much of the store is still measuring, and that state is not
> reproducible frame for frame — trap 8 says a measurement is against a *scene*, and this adds that
> it is also against a *convergence state*, which the `scene:` line does not show. Interleave the
> arms, rebuild between every run, and check `still bursting` and the live-face count agree before
> reading the times. One conclusion in this session was published wrong before this was understood
> (D406) and had to be withdrawn.

**Three things to know before touching it**, each of which cost something to find:

1. **A converged face must touch nothing, and "nothing" includes the load that finds out.** The
   first version read the lamp count and wrote it back every visit — half a million scattered
   read-modify-writes a frame on an answer that had stopped changing. `kFaceAmbientDone`'s comment
   makes this argument in full and this block did not follow it. The gate is `kFaceLampIdle`, bit 28
   of `photons`, the last free bit in that word. **It is kept on that argument and not on a
   number**: it first appeared to be worth 0.89 ms, that was two builds compared at different
   convergence states, and interleaved it comes out inside its own spread. D406.
2. **"Instantly" is the host's job.** A silent face cannot discover that a lamp has gone out. So
   `light_list_hash` gives the emitter list an identity, a change bumps a version, and
   `light_reset` is 1 for exactly one frame — which reopens every face; each then compares the
   version its own samples were taken under. Flag as gate, stamp as decision, the same shape D373
   settled for edits. A reopened face **keeps its mean and drops its confidence to eight samples**,
   so the picture moves at once instead of exploding into noise: measured on the lamp term alone,
   **73% of the change at edit+1, 85% at edit+5, 97.5% at edit+15, 99.3% at edit+50**.
3. **The one case that must go straight to nought is the last lamp going out.** With no emitters
   there is nothing left to measure, so a kept mean would light the room for ever.

**The larger cost is the transient**, and it is charged where the change is: across the eighty
frames after a 17.2-million-voxel carve, two interleaved rounds give the faces pass **17.08 → 19.80
ms mean and 24.25 → 28.83 worst** — so **+2.7 ms mean on top of a re-burst that is already 17 ms
before lamps exist** and is the ambient term reopening every face inside `kEditShadowReach`. The
dial is `kLampConverged` over `kLampBurst` and the trade is linear; D394 says not to meter it.

**The instrument is `--debug-mode 20`**, the lamp term alone, tone mapped because it spans orders of
magnitude. Magenta no face, blue no samples yet, green no geometry — black is a legitimate answer
here, which is why it is not shared with any of the three.

**Open, found by building this, deliberately not fixed:** `clips/many_lamps.clip` — a sealed hall
lit by thirty-six sconces and nothing else — comes out **blown white**, while the reference path
tracer on the same camera draws the same distribution correctly exposed. The face pass is right and
`kPreviewExposure` in `resolve.comp` is the constant 3.2, which its own comment says is a later
stage's job. It was invisible while every interior was lit by a fraction of a sky constant. Fixing
it makes every screenshot in the project brighter at once and every figure in this file
incomparable, so it wants to be its own change with its own baseline.

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

- **The baselines directory holds three files and only the newest means anything.**
  `r1e-chunks-gone.csv` is the live one — twenty-one realtime rows taken with the chunk system
  deleted, and the first grid in the file that records the world's **content hash** per row, so a
  later run can tell whether it is looking at the same world (D524). Note that its `close` and
  `enclosed` rows were taken late in a long measuring session and read about 10% high; the gate
  that decided R1e was an interleaved same-session pair, not this file (D523).
  `r2-node-pool.csv` is superseded and **must not be compared against**: it was recorded before the
  face pass computed sun, ambient occlusion or lamps, so every near camera reads as a large
  regression and every distance camera as a large win, and neither is true (D519).
  `r1e-chunks-going.csv` is **superseded too**: it holds no content hash, so nothing can check what
  it was measured against, and comparing against it now says so on every row.
  And `tools/baseline.ps1` no longer offers a `pathtrace` mode and throws if asked for one: the flag
  it passed has not existed since R3d, and an unknown flag is IGNORED rather than refused, which
  made half of every grid run a second realtime pass wearing the other mode's label (D519).
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
- **A library delete does nothing on Linux.** `send_to_recycle_bin` has a Windows body and a
  `return false` everywhere else, so on a Steam Deck the shelf refuses every delete and says so.
  The two tests that cover it are skipped there rather than deleted, so writing the XDG trash
  (`~/.local/share/Trash`, with the `.trashinfo` record that makes a restore possible) makes them
  pass on their own. Found by building on Linux for D636, not by a player, and it is the only
  platform gap that whole build turned up.

---

## 6. The state of the tree

**New:** `src/world/node_pool.{hpp,cpp}`, `src/world/face_store.{hpp,cpp}`,
`src/gpu/node_buffers.{hpp,cpp}`, `src/gpu/face_buffers.{hpp,cpp}`, `src/core/pass_ledger.{hpp,cpp}`,
`src/core/dirty_set.hpp`, `src/gpu/type_tables.{hpp,cpp}`,
`shaders/node.glsl`, `shaders/node_visibility.comp`,
`shaders/shade_faces.comp`, `shaders/face_worklist.comp`,
`tests/test_node_pool.cpp`, `tests/test_face_store.cpp`,
`tests/test_pass_ledger.cpp`, `tests/test_world_cache.cpp`,
`tools/{baseline,facecount,_grid,_measure}.ps1`, `documentation/21-renderer-rewrite.md`, this file.

**Modified:** `CMakeLists.txt`, `src/app/main.cpp`, `src/core/hash.hpp`, `src/gpu/image.hpp`,
`src/world/world_cache.{hpp,cpp}`, `src/gpu/profiler.{hpp,cpp}`, `shaders/{params.glsl,resolve.comp}`,
`src/gpu/render_params.hpp`, `src/debug/hud.{hpp,cpp}`, `tools/{speckle,baseline,package}.ps1`,
`documentation/{13-decision-log,README}.md`.

**Deleted**, by R3d and R1e between them: `shaders/pathtrace.comp`, `shaders/pt_normals.glsl`,
`shaders/world.glsl`, `shaders/visibility.comp` (the chunk marcher's — `node_visibility.comp` was
renamed into its place), `src/world/residency.{hpp,cpp}`, `src/world/summary_tree.{hpp,cpp}`,
`src/world/thumb_cache.{hpp,cpp}`, `src/world/thumbnail.{hpp,cpp}`, `src/gpu/world_buffers.{hpp,cpp}`
and `tests/test_{residency,summary_tree,thumb_cache,thumbnail}.cpp`. About 6,500 lines. With them
went both orphaned descriptor sets, the tracer's 256 MB face cache, the frame-statistics buffer,
`rebuild_coarse_grids`, `--stream-frames` and `--stream-log`.

**Nothing in the renderer addresses a chunk now.** `World`, `Chunk`, `serialize` and `world_cache`
are untouched: a chunk is still what `03-voxel-data-model.md` says it is, a unit for saving and
networking, which the renderer never sees.


## 7. Commands

**`.\test.bat` was not running its third stage, and now is (D605).** That stage invoked
`--stream-frames 300`, which R1e deleted; an unknown argument is only a warning and the wall-clock
deadline only binds a run that asked for a scripted mode, so it opened the game and sat on the title
screen for ever while the batch still printed `All tests passed`. It is very likely why the overnight
loop was killed at sixty minutes twice. It now takes one 640×400 screenshot, which is how the node
pool's three audits are reached, and requires all four of their phrases to appear. **If you add an
audit that logs rather than returns, add its phrase to the `call :require` list in `test.bat` or it
is not a test.**

**The tree builds and tests on Linux again, and one session found that out the hard way.** A
session that has no Windows and no GPU — a cloud container, or a Deck — can still do everything on
the CPU side of this project: `cmake -S . -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo`
then `cmake --build build-linux`, and `build-linux/bin/ws_tests` runs the whole suite. It needs
`libvulkan-dev glslc` and SDL3's X11 packages, nothing exotic, and gcc's warnings are errors there
too, which is what four small portability fixes in D636's commit were. **Two library tests are
skipped there** — a delete goes to the system recycle bin and there is no Linux implementation of
`send_to_recycle_bin`, so a player on a Deck cannot delete from the library at all. That is real
debt, tracked below, and not a test problem.

**And the renderer itself runs there too, which nobody had tried** (D641). Not on a GPU — on Mesa's
software Vulkan, `lavapipe`, under a virtual X server:

```bash
apt-get install -y mesa-vulkan-drivers vulkan-tools xvfb          # plus libvulkan-dev glslc
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json \
  xvfb-run -a -s "-screen 0 640x400x24" ./build-linux/bin/WorldShaper \
  --no-title --width 160 --height 100 --face-budget 16384 --cam 0,2,-20,90,0 \
  --screenshot shot.png --screenshot-frame 25 --clip-coarse 8
```

The ICD path is `lvp_icd.json` and NOT `lvp_icd.x86_64.json`; naming the file that does not exist
gives `SDL_CreateWindow failed: Installed Vulkan doesn't implement the VK_KHR_surface extension`,
which reads like a missing driver rather than like a typo, and cost a session twenty minutes. SDL's
`offscreen` driver gives no Vulkan surface at all, so the X server is required.

**Two costs dominate and both have a flag.** The first frame is **204 seconds** — every compute
shader is JIT-compiled by the driver, once, so a longer run amortises it. And a frame at the shipped
face table is **267 SECONDS**, because the passes that sweep a million slots are sized for hardware;
`--face-budget 16384` takes that to **0.5 s a frame**, a 530× difference that is entirely about the
table and not about the pixels. Shrinking the window barely matters by comparison.

**What it is good for and what it is not.** Nothing timed there means anything. But a picture is a
picture, every audit prints, and the content hashes are the same numbers as on the development
machine — so the *correctness* half of a change can be gated in a container: does the world agree
with the pool, does a sealed room come out black, does a frame reproduce. **A small `--face-budget`
is the pressured regime (D306), not the shipped one**, so anything about eviction or the cold window
measured that way is about a different state.

**Running it there found a real bug in this engine, which is the argument for doing it** (D641).
`--validation` on that driver reports `VUID-VkWriteDescriptorSet-descriptorType-00333`: the node
pool's **512 MB payload is bound as one storage-buffer descriptor and this device binds at most
128 MB**. Nothing had ever read `maxStorageBufferRange` because every part this has run on reports
4 GB. It is read now and the budget is capped to it before the pool is told — a no-op on every
machine this project has measured on, and the difference between working and an invalid descriptor
on one that reports less. **A limit that is generous where you develop is still a limit**, and a
Deck is the target that makes that not hypothetical.

**One render run at a time.** Two of these launched together killed the container twice in a row,
both times within a minute of the second one starting — a software Vulkan device holds the whole
world, a JIT of every shader and four worker threads, and two of them together is more than the box
will carry. It costs a session ten minutes each time and reads like an unrelated infrastructure
failure, which is why it is written here rather than left to be rediscovered.

**It still segfaults at frame 16–17 with validation clean — and that is now bisected to ONE RULE
(D642).** Five arms of one build: `--no-secondary-faces` survives forty frames and writes a picture
of the facility; secondary faces on with secondary light off crashes. **R9a — the one ray in the
renderer that may add to the face set — is what provokes it.** Not the paste path (128× less pasting
crashes at the same frame), not face reads, and not D641's descriptor, which is fixed and which the
crash outlived. R9a's own append is correctly bounded, so the fault is downstream of the face set it
grows; the untested suspect is the lobe pool, which R9a feeds and which `--face-budget` does not
size.

**So the container renders forty frames with `--no-secondary-faces` and about sixteen without.**
And the question a Windows session should ask, because D641's fault was exactly this shape and
turned out to be ours: `--validation` plus
`VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_GPU_ASSISTED_EXT` checks indexing *inside* a shader.
If it names a buffer, an RTX has been forgiving about a real out-of-bounds write for months.

What that session could NOT do is anything with a clock: a CPU four cores wide and several times
slower than the development machine, and a software rasteriser besides. **Nothing timed on such a
machine is comparable to anything in this file.** Content is: the facility's field is `3744 nodes,
923 with no box` there and here, and `clips/sampler.clip` measures 1,430,104 voxels on both.

```powershell
.\build.bat                          # build; NEVER pipe this to Out-Null while measuring
.\test.bat                           # build, 527 tests, the world audit, the node pool audit
.\build\bin\ws_tests.exe             # the whole suite - not a name filter, which silently skips
.\tools\darkroom.ps1 [-Fog]          # a sealed room must be BLACK: brightest channel 0 of 255
.\tools\baseline.ps1 -Out docs.csv   # the fixed grid; -Compare <csv> to diff a previous run
.\tools\facecount.ps1                # distinct visible faces per view and resolution
.\tools\_flybench.ps1 -Rounds 3      # the MOVING case, which the grid cannot see (D410)
.\tools\_flybench.ps1 -Chisel 8,16   # ...and the WORST case: moving while editing (D413)

# Is this build's clip the same clip as the last one's? A hash of the voxels, headless, in a
# second, where this used to mean a whole game run with --refine-all (D640).
.\build\bin\WorldShaper.exe --clip-file clips\sampler.clip --no-despeckle   # content da8d21629c21a25d
# ...and what single precision would do to it, both arms built in the one run
.\build\bin\WorldShaper.exe --clip-file clips\sampler.clip --eval-f32

# The clip's field, its bounding boxes and which ops have none, WITHOUT sampling anything —
# and then what one evaluation walks, by op (D636, D638). The parse decides all of it, so this
# is a second where the full measure is minutes.
.\build\bin\WorldShaper.exe --clip-field --clip-file clips\facility.clip
```

**The two arms of an A/B on the light pass are two flags, never two builds** (D407, and now D420
for what happens when the harness itself gets it wrong):

```powershell
.\tools\_flybench.ps1 -Tag before -Chisel 8,16 -Extra "--no-face-gate --no-face-worklist"
.\tools\_flybench.ps1 -Tag after  -Chisel 8,16
```

`--no-face-gate` lights every face in the store again whether or not a pixel has read it;
`--no-face-worklist` dispatches the shading pass over every live slot again;
`--no-face-prolong` is the third and is already the default (D417);
`--no-node-crossings` restores residency hearing only about the brick a ray stopped on, which is the
control arm for D427 and the state everything above this section was measured in;
`--no-light-keeps-geometry` stops a light ray saying it is using the cell that stopped it, which is
D430's control arm, and `--light-read-period N` is the window that rule reports on, in frames;
`--no-lamp-edit-scope` reopens the lamp term of every face within sixteen metres of an edit again,
which is D434's control arm, and `--face-edit-seed N` is how many samples an announced face keeps,
where 0 is D435's control arm and restores the wipe;
`--no-face-pressure` makes the face store wait until it is FULL before giving anything up, which is
D504's control arm, `--face-pressure-from N` is how little free space starts the squeeze as a divisor
(2 is "from half free", which is the setting D505 measured and rejected), `--no-face-reads` stops a
face telling the host that a pixel read it and is D508's control arm, `--face-read-period N` is the
window it says it in, and `--face-budget N` shrinks the table so the full-table state — the blocky
flicker of D502 — is reachable in ninety seconds instead of after minutes of play;
`--whole-set-retry` puts the face upload back to clearing its dirty sets only when the whole set
fitted, which is D544's control arm and the state every figure above it was measured in;
`--no-bounce` is the bounce control arm — and it is **not** a way back to the picture before R9, since
the ambient floor that stood in for that light is deleted rather than switched off: with it, an
interior is lit by the sun, the sky and the lamps it can see, and by nothing else. `--bounce-min N` is
how many far samples a face takes before its bounce may stop (512), and `--bounce-memory N` is how
many of them it REMEMBERS (128) — **the control arm for D550 is `--bounce-memory 4096 --bounce-min
128`, both together**, because a memory larger than `far_n` can reach is the cumulative mean exactly
and the two numbers are one trade;
`--no-coarse-keep` makes the store give a coarse stand-in up on the same clock as any other face,
which is R9f's first control arm and the state everything above that section was measured in, and
`--no-coarse-bounce` stops a gathering ray reading the coarse face over a surface that has no light
of its own, which is its second — the two are separate flags because they are two rules, and the
first is the one to switch off when measuring the second;
`--no-light-probe` stops counting what gathering rays land on, which costs nothing and is the arm to
use if that counting is ever suspected of costing something;
`--no-secondary-faces` stops a light ray naming the face it landed on, which is R9a's control arm and
the state everything above that section was measured in, `--secondary-period N` is the window in
frames a face may name one in (a power of two; 64 is the default) and `--secondary-share N` is the
off-screen cap as a divisor of the table (4 is a quarter);
`--no-secondary-light` stops a face nobody is looking at from casting any ray at all, whatever is
reading it — R9b's control arm, and the state every figure taken before D561 was measured in — and
`--secondary-light-share N` is that class's ray budget as a divisor of the on-screen set's shading
rate (8 by default; larger is cheaper and slower, and **D566 is why sweeping it downwards buys much
less than it looks like it should**);
`--denoise-edge N` is how hard R5a's filter weighs a neighbour against what a face already holds, and
**0 is the control arm** — no agreement test at all, which is the filter that smeared each lit face
into the eight around it (D579); `--exposure-max N` is how far the light meter may lift a dark scene,
where a large value restores R6a's original 4096 and is the state a room with no light in it read as
lit (D581);
`--no-auto-exposure` restores the fixed brightness multiplier of 3.2 this pass applied for the whole
of the rewrite, which is R6's control arm and the state every picture figure above that section was
measured in — it works by having the host zero both of the meter's slots every frame, so the shader
takes its own "nothing has been measured" branch and no flag reaches it;
`--face-fold` makes a coarse face take its sky and bounce from the four faces under it rather than
from its own rays, which is R9f's fold — **off by default**, worth a third more light in the bounce
and 2.8 ms of the faces pass;
`--no-emitter-cache` makes the emitter list rediscover every chunk on every announced change again,
which is R9g's control arm and the state every figure before it was measured in — it is a cleared
map rather than a second code path, so the arms cannot differ by a branch;
`--halo` claims faces past the edge of the screen over a margin sized by how fast the camera is
turning, which is R9c — **off by default**, because it costs the sun's stride (6 → 7) for about a
quarter of one edge's ambient deficit, and `--halo-lead N` is how many frames of head start it aims
for; the margin is nought whenever the camera is still, so the settled case is identical either way;
`--no-face-materials` stops a face working out what the surface under it is made of, which is R4a's
control arm and, since R4c shades with it, is now the arm for **the whole of R4**: the composite sees
no material, applies no energy split and adds no lobe, which is the picture as it was before the
stage;
`--no-face-lobe` is R4c's own control arm and takes only the BINS away — a metal still loses its
diffuse share and still gets its sun highlight, and its environment comes back as the hemispherical
mean it already stores, which is the same lobe with one bin in it. So the two arms differ by exactly
the stored direction and by nothing else;
`--lobe-floor N` is the pool's capacity dial, in worth: 0.05 is the default and admits every metal,
0.038 reaches glass and water, and 0 gives a block to every face that knows what it is made of, which
is the arm that says what the pool's size is costing;
`--no-face-denoise` reads a face's far field and bounce raw rather than blended with its coplanar
neighbours', which is R5a's control arm and the state everything above that section was measured in —
it leaves the WRITE in place and takes the eight neighbour lookups out, so the composite reads the
same four words in both arms and the A/B prices the filter rather than a branch in the reader;
`--no-class-eviction` puts every record in the face store back on one eviction clock, whoever asked
for it, and spends the coarse pyramid at the first step of the squeeze — **pair it with
`--secondary-share 4`**, which restores the off-screen class's fixed quarter, for the whole control
arm of D570 and D571, because they are two rules and a control arm that reverts one of them measures
one of them;
`--no-paste-pool` puts the region paste back on the background sampler's job system, which is
D513's control arm and the state every paste figure above this line was measured in — pair it with
`--no-clip-cache` so the ladder actually runs, and watch the `region:` line, which now splits the
paste from the op replay from the announcement. `--chisel EVERY,RADIUS` carves
and fills alternately at whatever the camera is looking at, through the same code path the mouse
button takes, and prints what it changed — **read that line before reading any time from a chisel
run**, because a run that missed every time and a run that never fired print the same pass table.

**The grid and `_flybench.ps1` answer different questions and neither substitutes for the other.**
The grid starts its window at refinement's fixed point, so it measures a store in which every face
has converged and nothing is casting a ray — right for comparing marchers, and blind to the light
pass, which reads 1.11 ms there and 11.75 ms while the camera moves.

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

**The node pool is the only marcher** (R1e). `--chunk-marcher`, `--node-pool` and the F6 toggle
are gone, and so are `--pathtrace` and F4 — R3d deleted the reference tracer, so there is no second
renderer to switch to and nothing left that computes the picture a second way. `--debug-mode 11` writes each pixel's face key as four
exact bytes; `12`–`15` write one word of the visibility buffer the same way, which is how a
disagreement gets localised to a field instead of argued about from a screenshot. **`16` is the sun
term on its own** and is the instrument for anything about shadows: grey is the visibility fraction,
**magenta** a face the composite could not find, **blue** one it will not believe yet, **green** no
geometry. Magenta and blue are the pixels being lit by the fallback, so their share of the surface
is a number rather than an impression — that is what R9d was measured with. **`17` is sky
visibility and `18` the near field**, which are the two halves of the ambient term and are gated
separately now that they no longer share a ray. **`19` is the ambient term's own convergence state**
— green a face that has finished and casts no more rays, red one held short of it by geometry the
pool has not built, grey the progress between, blue no samples, magenta no face — and it is the view
to reach for when the face pass costs more than it should, because a converged face and an
unconverged one are the same picture in every other view. **`20` is the lamp term on its own**, tone
mapped because a lamp's contribution spans orders of magnitude between standing under a sconce and
standing across a hall from one; magenta no face, blue no samples yet, green no geometry, and black
is a legitimate answer rather than a failure. **`21` is what each FACE is made of** (R4a) — red is
metalness and green is smoothness, so limestone reads near black, gilt and bronze read orange and
glass reads bright green; magenta no face, blue a face that has not looked yet, **cyan a coarse face,
which has looked and has no one material to have**, green at a third no geometry. It draws what the
*face* knows and not what the pixel knows, which is the only way to tell that the material reached
the light pass at all. **`22` is R4c's pool** — green a face holding a block of outgoing bins and
measuring into it, yellow holding one with nothing in the bin the eye is reading, **red asked and was
turned away**, dark grey a face whose lobe is not worth a block (which is most of a stone building
and is the ordinary answer), and the same magenta, blue, cyan and green-at-a-third as 21 so the two
can be read against each other without a legend. **`23` is the lobe on its own**, tone mapped, with
the diffuse taken away — the view to check "does the reflection sit still on the surface as I walk"
against, because in a shaded frame a metal's lobe and its diffuse are the same brown. The node pool's
GPU mirror is checked automatically at the screenshot and logs either
`GPU mirror matches` or the first differing byte.

**Three more arrived with R9f, and the first is the one to reach for when light is reported as
missing rather than as wrong.** *The gathering ray, last frame* is what the unbounded ambient ray
landed on, as a rate over one frame: sky, a lit face, a surface with no face in the store, a face
that has measured nothing yet, or a cell the pool has not built — and under it, how many of the ones
that found nothing had a coarse face above them that did. That figure is printed whether the rule is
on or off, so it reads as *what this would recover* in a control arm and *what it is recovering*
otherwise. *The coarse pyramid* (host) and *the coarse pyramid on the card* say how much of the store
the stand-ins are and how much of it was given up anyway — read as a pair, because a live count alone
cannot tell "the rule is holding them" from "there were none to hold". They are counted APART from
`ambient on the card` deliberately: a stand-in nobody is reading casts nothing, and counting it as
*still bursting* is what sent a reader to the wrong pass once already.

**Five lines about the face store are printed at every screenshot, and three of them are new.** `the
set on the card` splits the store into the on-screen and off-screen classes with the samples each
carries — the measurement R9 is judged on, because the risk of that stage is not frame time but a
store spread too thin. `the off-screen set` says what light rays offered, what was claimed, what the
cap declined and what a pixel later promoted; **declined is not refused** and the two must never be
added. It now also carries **the cap itself and the window that class is given up on**, because since
D570 neither is a constant anybody can look up: the cap is the table's spare room and moves with the
size of the on-screen set, and the window is shorter than everything else's the moment the table
tightens. Read them as a pair — a live count against a cap says whether the class is being held back,
and the two windows beside each other say whether it is being spent.
`the card is N records ahead of the store` is the one to read before any moving-camera cost
figure: the face pass shades what the CARD holds, and an upload that runs out of staging clears
nothing and sends the whole set again, so the card can hold hundreds of thousands of records the
store gave up (measured: 434,838 while flying). `the card's own stand-ins` counts the provisional
faces the card claims for itself, each of which takes a fresh unbounded ray and a fresh lamp burst
every frame it is needed. The fifth is `faces:`, which now also prints the **sun stride** — what the
sun's ray budget is being divided by, and the number two builds with the same store and the same
picture can differ by (D527) — and beside it the **off-screen stride**, the same number for the other
class, where nought means that class is casting nothing at all.

**And one more, which is the only counted figure among them**: `the off-screen set cast on N faces
this frame` says what the off-screen class actually shaded, measured in the pass that spends it rather
than predicted from a population and a stride on the host. Read it against the stride beside it — a
stride without a count is what the host *meant* to happen, and the two disagreeing is the only thing
that can say this class has silently stopped casting. It is a rate over one frame, so nought on a
camera whose whole store has converged means "nothing was due"; `the set on the card` is what says
whether the class ever measured anything at all. D567.

**Three audits run at every screenshot and they answer three different questions.** `GPU mirror
matches` asks whether the card holds what the pool holds — and both can agree perfectly about
something neither has looked at since the world changed. *The node pool agrees with the world, leaf
for leaf* (`stale_leaves`, D400) asks whether a built brick still has the shape the world gives it,
which is what catches a writer that skipped `invalidate`. *The node pool agrees with the world, mask
for mask* (`stale_masks`, D516) asks whether a **child mask** matches the world — where a ray is
allowed to look, as against what it finds when it gets there. A wrong bit there is either a phantom
request every frame for ever (D133) or geometry no feedback will ever ask for, and it is invisible
to the other two. Read all three before concluding the tree is healthy.
