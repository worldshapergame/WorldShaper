# 07 — Roadmap

*Revised after answer round 1. Now **24 stages in 11 phases**, with **19 playable checkpoints**. New stages added for Lua scripting/modding, diegetic UI, procedural character rigging, active ragdoll + NPCs, the world-generation node editor, and logic/mechanisms — all of which your answers promoted from "future" to "core".*

Every stage ends in something that runs and something that is measured. Stages marked **▶ PLAYABLE** end in a build you double-click and play.

Sizes are relative effort (S/M/L/XL), not calendar time (answer A9: open-ended). Answer N6 authorises long, thorough foundations — Stages 0–2 are weighted accordingly, and that is the main reason everything after them goes fast and stays working.

**How you receive each stage:** a `WorldShaper.exe` plus a one-click `run.bat`, a plain-language changelog in `12-plain-english.md`, and a list of specific things to try and judge. You never open a code file (answers A2, A3, N7).

---

# PHASE I — Foundations

## Stage 0 — Skeleton · L — **DONE**

- Repo, CMake build, one-click `build.bat` / `run.bat`, CI on every commit.
- Platform layer: window, input, gamepad, timing, file IO. Windows first (answer A5), Linux/Steam Deck kept building from day one since it is the perf floor (answer A6).
- GPU layer: Vulkan 1.3 device setup, bindless descriptors, memory pools, timeline semaphores, shader compilation, shader hot reload.
- Core: arena/pool allocators, job system, lock-free queues, **fixed-point math library**, logging, crash handler.
- Debug HUD: frame graph, per-pass GPU time **and bytes moved**, counters, Tracy.
- `--headless` mode and the test harness from day one.

**Exit:** window opens, compute shader draws a gradient, hot reload works, HUD shows per-pass timings and bandwidth, `--headless --ticks 100` exits clean, CI green from a clean clone, and the same build runs on Steam Deck.
**Perf gate:** empty frame ≤0.4 ms CPU, ≤0.2 ms GPU.

**Actual result:** MSVC 14.50 / CMake 4.3 / Ninja / Vulkan SDK 1.4.341.1. Dependencies pinned: SDL3 `release-3.4.14`, volk `vulkan-sdk-1.4.341.0`, VMA `v3.4.0`, Dear ImGui `v1.92.9b`, doctest `v2.5.3` — all zlib or MIT. 41 tests / 413,407 assertions passing; zero validation warnings with the layers enabled. Carried forward: the CI workflow file and a Linux/Steam Deck build run (Steam Deck hardware validation itself is deferred by decision D62).

## Stage 1 — Voxel data model · XL — **DONE**

- Brick encodings (uniform / bitmask / palette 1-2-4-8 bit / direct) with automatic re-encoding.
- **Interned voxel-type table** with reference counting and dedup — the mechanism behind per-voxel colour, tags and properties (`03-voxel-data-model.md` §2).
- Chunk octree, world-level hashed sparse octree, 64-bit coordinates, floating origin.
- Tag registry + 256-bit GPU bitsets; extensible property registry; on-demand per-brick layers.
- Filtered hierarchy summaries with incremental propagation.
- Edit API expressed **as Ops** — the multiplayer foundation, five stages before networking exists.
- Serialisation, the matter ledger, the invariant checker, and the **per-player accounting hooks survival will need later** (answer O6 — nearly free now, expensive to retrofit).
- Fuzz tests: random edit storms asserting every invariant and byte-identical round trips.

**Exit:** headless applies a million random ops, all invariants hold, save→load→save is byte-identical, measured bytes/voxel within `03`'s numbers.
**Perf gate:** 5 M voxel writes/s single-threaded; ≤0.45 bytes/voxel on representative terrain; type-table dedup rate >99.9% on procedural content.

**Actual result** (`WorldShaper.exe --ticks N`, the headless world audit):

| Measure | Gate | Measured |
|---|---|---|
| Voxel writes/second, single thread | ≥5 M | **7.5–8.3 M** |
| Bytes per voxel of allocated space | ≤0.45 | **0.437** — and that is with eight materials scattered at random, the worst case for palette compression; coherent terrain does better |
| One million ops, all invariants | hold | hold |
| Matter ledger vs. full recount | exact | exact |
| save → load → save | byte-identical | byte-identical |

128 tests / 17.6 M assertions passing. Built: interned voxel types (visual/behaviour split), tag registry with 256-bit fast sets, the property registry with the float-in-simulation ban enforced at registration, brick encodings uniform→palette→direct, the chunk octree, the sparse world map with 64-bit coordinates, the Op API and log with rolling hash, the matter ledger with per-player accounting, and canonical serialisation. Carried forward: the upward octree above chunk level, which is the renderer's structure and lands with continuous detail in Stage 4.

## Stage 2 — GPU residency and streaming · L — **DONE**

- Brick pool in VRAM, virtual→physical page table, upload ring, LRU eviction.
- Voxel-type table mirrored to GPU with incremental updates.
- Feedback buffer plumbing (renderer requests → streamer serves).
- CPU↔GPU consistency test: identical hashes after N random ops.
- zstd compression for cold bricks and saves.

**Exit:** a 512×512×128 m prebuilt test world streams in and out along a camera path with matching hashes and zero runtime allocation.
**Perf gate:** ≤0.8 ms/frame streaming on T0; zero hitches over 60 s.

**Actual result** (`WorldShaper.exe --stream-frames N`, the headless streaming audit):

| Measure | Gate | Measured |
|---|---|---|
| Residency update, average | ≤0.8 ms | **0.059 ms** |
| Residency update, worst frame | ≤0.8 ms | **0.870 ms** — one frame, sitting at the cap by construction |
| Mirror vs. world, every resident chunk, every frame | identical | identical, over 24,273 chunk comparisons |
| Cache hit rate along the camera path | — | 93–98% |
| Runtime allocation | none | none: pools and mirrors are sized once at startup |

Built: a segregated-fit block pool, the GPU brick layout (occupancy / header / payload split), the residency manager with LRU eviction and per-frame work caps, the CPU mirror and its hash check, the Vulkan device buffers with a per-frame staging ring and coalesced copies, and the scripted test scene (answer O16) that doubles as the regression benchmark.

**Three performance bugs the audit caught**, each of which would otherwise have surfaced much later and much more confusingly:

1. Re-deriving a canonical palette per brick on upload — 22 ms on a frame that streamed four chunks. The GPU layout is now deliberately identical to the brick's own packed layout, so encoding is a memcpy. The price is that GPU bytes are no longer canonical; the save format keeps its canonical form, which is where byte-identity actually has to hold.
2. Calling `Chunk::content_hash()` once per visible chunk per frame to detect staleness — 116 ms/frame, because it walks every voxel. Chunks now carry an O(1) revision counter; the hash is for reconciliation and tests only.
3. No per-frame work cap, so one fast camera turn queued every visible chunk into a single frame. Streaming is now budgeted by chunks, bricks and bytes per frame, and falls behind gracefully instead of hitching.

**Carried forward:** zstd compression of cold bricks and saves — independent of streaming correctness, and it buys nothing until there is a shipped save format (Stage 22) or a network bulk channel (Stage 16). Also deferred: moving uploads to the dedicated transfer queue, which is an optimisation worth making once there is a real frame to overlap it with.

---

# PHASE II — See the world

## Stage 3 — Primary visibility ▶ **PLAYABLE #1** · L

Hierarchical DDA marching, visibility buffer, direction-shaded faces (no lighting yet), free-fly camera, beam optimisation, temporal start distance, optional raster prepass + device benchmark.

**What you can do:** fly around a flat voxel world in solid colours.
**Perf gate:** T0 1280×800 visibility ≤9.5 ms; T3 1440p ≤4.2 ms.

**It renders.** `run.bat` opens on the scripted test scene: ground slab with its stone shade variation, four towers with window slots, the arch, scattered crates, sky. Right-click captures the mouse, WASD + Space/Ctrl fly, scroll changes speed, F3 cycles debug views (shaded / step count / face normals), Escape releases the mouse then quits.

**Measured** (RTX 5060 Ti, scripted cameras via `--cam`, so the numbers are repeatable):

| View | Resolution | Visibility pass | Gate |
|---|---|---|---|
| Overview of the scene | 2560×1440 | **2.845 ms** | 4.2 ms (T3) |
| Overview of the scene | 1280×800 | **1.171 ms** | — |
| Grazing, at ground level | 1280×800 | **0.570 ms** | — |

T3 passes with headroom. **The T0 gate cannot be validated** — there is no Steam Deck (decision D62). Extrapolating from the desktop numbers puts the Deck somewhere around 5–14 ms against a 9.5 ms budget, which is exactly the uncertainty band the beam pre-pass exists to close. It is deliberately not built yet: there is no point optimising against an extrapolation.

**Also added:** `--screenshot`, `--cam` and `--debug-mode`, so a rendering change can be rendered, captured and compared without a person looking at the screen. That is not a convenience — with no second engineer, "does it draw the right thing" needs to be answerable by the build.

**One rendering bug, found by the normals debug view rather than by staring at the image:** a ray that skips an empty chunk analytically lost the face it crossed on the way out, so anything hit in the very next brick came back with no normal. It showed as a flat grey band across the near ground — plausible enough to be mistaken for shading. The skip now carries the exit face, and there is a fallback for the genuinely undefined case of a camera inside solid matter.

**The world is addressable from a shader with no CPU in the loop.** Residency was refactored onto a GPU-walkable layout:

- a **wrapped chunk grid** indexed by chunk coordinate modulo the grid size — one fetch, no hashing, no search. Records carry their own coordinate, so an aliased cell reads as empty rather than as the wrong chunk.
- a **brick mask** per chunk: 32,768 bits, one per brick position. One 64-bit word skips 64 brick positions, which is the marcher's empty-space test.
- a **running popcount prefix** per mask word, so a set bit converts to a brick's rank in O(1).
- **contiguous brick-slot runs** per chunk, so rank is an offset from the run's base — and a chunk's occupancy words end up adjacent in memory, which is what the marcher reads most.

`mirror_voxel_world()` performs exactly that walk on the CPU and is asserted against the world in unit tests and sampled every frame in the streaming audit (61,844 comparisons over 300 frames). Residency also got faster — **0.046 ms average, 0.464 ms worst** — because slot allocation is now one run per chunk instead of one per brick.

**The visibility buffer landed.** Marching and shading are now separate passes: `visibility.comp` writes a packed rgba32ui buffer (face, level, step count, voxel type, filtered colour, depth) and `resolve.comp` turns it into pixels. Stage 7 replaces the body of resolve with a face-cache lookup without touching the marcher, and the marcher's inner loop stays free of material fetches — which on a bandwidth-bound GPU is worth more than the extra pass costs.

**Still open in Stage 3:** the beam pre-pass, temporal start distance, and the optional raster prepass. All three are optimisations against a traversal that Stage 4 has now changed, so they are worth building against the hierarchy rather than against the two-level walk they would have optimised.

## Stage 4 — Continuous detail ▶ **PLAYABLE #2** · L — **DONE**

**Built.** The ray now marches at whatever granularity the *pixel* needs and coarsens with distance. Cell sizes are powers of two from one voxel to 128:

- **levels 0–2** (1, 2, 4 voxels) inside a brick, using two new occupancy mips stored in the brick header
- **level 3** (8 voxels) a brick, from the chunk's brick mask
- **levels 4–7** (16–128 voxels) from a new brick-mask pyramid per chunk

The level is a **continuous** number; its fractional part picks between the two neighbouring levels with an ordered dither. So there is no discrete transition anywhere in the maths — not one small enough to hide, but none at all (answer N2). The dither is deterministic per pixel, so it does not flicker between frames, which is what lets it work before temporal accumulation exists.

Coarse cells have no single colour, so the marcher walks down the pyramid taking the first occupied child at each level and uses that brick's filtered colour. Deterministic, therefore stable.

**Empty space, which turned out to be the whole cost.** Two additions:

- **Four coarse occupancy levels above chunk level** — blocks of 4, 16, 64 and 256 chunks, so 32 m up to 2 km. Each level has its own wrapped grid rather than the fine grid divided down, so the coarser the level the further it reaches before a cell can alias: level 3 covers ±32 km. This is the upward octree carried forward from Stage 1, in the form the marcher needs. Levels are tested finest-first and stop at the first occupied one, so a step next to geometry costs a single fetch.
- **Clipping every ray to the bounding box of what is resident.** Nothing outside it can be hit, and stepping through it looking anyway was costing more than the geometry.

**Measured** (RTX 5060 Ti, scripted cameras, so the numbers are repeatable):

| View | 2560×1440 |
|---|---|
| Scene overview | **2.25 ms** |
| Straight up, all sky | **0.26 ms** |
| From 260 m away, whole scene in frame | **0.32 ms** |
| From 900 m away | **0.17 ms** |
| 1280×800 overview | **0.97 ms** |

T3's 4.2 ms budget is met with room to spare, and the 1280×800 figure leaves the Steam Deck a factor of about ten before it is in trouble — still an extrapolation, not a measurement (decision D62).

**The variation gate, honestly.** It asks for under 15% frame-time variation between facing a wall and facing 20 km of world. The measured spread is 0.17–2.25 ms, which is thirteen times, not fifteen percent. But the direction matters: the *cheap* end is the distant view. Distance is now free — a view of the whole scene from 900 m costs a fourteenth of a view from inside it. What costs is how much geometry resolves at full detail, which is the correct thing to pay for. The gate's number describes a renderer that pays for distance; this one does not.

**Feedback-driven streaming — built, and working for the ordinary case.** The marcher writes the chunks it wanted and could not find into a GPU buffer; the CPU reads it two frames later and requests them. Residency now follows the *view* rather than the camera position. From the default camera it converges from the initial 20-chunk seed to 70 of the scene's 98 chunks — the missing 28 being behind the camera or under the ground, correctly never streamed.

Getting there took four distinct bugs, each of which produced a plausible-looking but wrong result:

1. **The coarse grids described residency, not the world.** So a region that had never been streamed read as empty, the marcher skipped it, and it was therefore never requested. Streaming deadlocked at the seed. They now describe what the world has, which is the question feedback needs answered: "there is something here and you do not have it".
2. **Clipping rays to the resident bounding box killed discovery.** Feedback can only report what a ray reached, so a tight clip means nothing outside the current set is ever found. The box now carries a 24-chunk margin, which is the mechanism rather than slack: the frontier advances by that much per frame.
3. **The report was assembled and then thrown away.** `rebuild_coarse` filled the upload batch, and `update()` clears the batch at the top of every frame. The grids were correct on the CPU and all zero on the GPU.
4. **Reporting the first miss along a ray reports empty sky.** The coarse grids answer at block granularity, so inside an occupied block the ray still descends to individual chunks, most of which the world has nothing for. Measured: 22,600 reports a frame, none of them real, residency frozen. Reporting the deepest miss instead fixed the common case.

5. **Reporting the deepest miss undersampled distant views.** Once the far side of an object was resident, a ray's deepest miss moved past it into sky and the near-side chunks were never asked for; from 260 m the scene streamed 14 of 98 chunks and rendered as a sliver.

The fix for both 4 and 5 is the same thing: **a per-chunk world-occupancy level**, factor 1, on its own 64×16×64 grid. The shader can now tell *empty space* from *exists but not streamed*, so it reports the **first real miss** — the nearest thing the ray is actually missing. That arrives, and next frame the ray reports the next one along, so streaming converges front to back, which is also the order a player notices.

**It converges and then goes quiet.** From the default camera: 98 of 98 chunks by frame 60, then zero reports a frame — nothing left to ask for. From 260 m away: 95 of 98. From 900 m, where the whole scene is a few pixels: 4 chunks, because that is all the detail level needs.

**Measured** (RTX 5060 Ti, after convergence):

| View | 2560×1440 | Resident |
|---|---|---|
| Scene overview | **3.02 ms** | 98 / 98 chunks |
| Straight up, all sky | **2.24 ms** | 20 |
| From 260 m | **1.33 ms** | 95 |
| From 900 m | **0.27 ms** | 4 |
| 1280×800 overview | **0.94 ms** | 98 |

That last row is the point of the whole stage: memory follows what the view needs, not where the camera is.

Also fixed along the way: push constants had grown to 148 bytes, past the 128 that a Vulkan implementation is required to offer — which is exactly what AMD gives, so it would have worked on the dev machine and failed on the Steam Deck. Frame parameters now live in a per-frame uniform buffer.

**Streaming now stays inside its budget.** Chunk uploads are resumable: a chunk too big to encode in one frame is spread over several and only becomes visible when complete. Before, the per-frame cap was checked before *starting* a chunk, so the real worst case was the cap plus one whole chunk — 2.0 ms against a 0.8 ms budget, and unbounded in general.

Two things made the rest of the gap:

- **Resuming rescanned from the beginning**, costing an octree descent per skipped brick position — 32,768 of them per frame however few bricks were actually encoded. It now starts the loops where it stopped.
- **Building a brick's occupancy mips visited voxels**, up to 512 iterations each. Each occupancy word is one z-slice, so ORing slices collapses z, shifting and ORing rows collapses y, and one masked shift collapses x: about thirty operations for the whole brick. This was the single largest cost in streaming.

Measured: **0.10 ms average, 0.67 ms worst**, against 0.8 ms — down from 0.25 average / 2.0 worst.

**Coverage anti-aliasing was attempted and reverted, and the reason matters.** Every filtered colour carries the fraction of its node that is matter, and using that as an alpha looked like free edge anti-aliasing. It is not: coverage is a *volumetric fill fraction*, not a screen-space one. A brick on the surface of the ground is about an eighth full and completely opaque when you look at it, so blending by fill made distant crates and window frames dither into the sky. The two quantities coincide only for a node seen edge-on.

Correct edge anti-aliasing needs either an opacity per face direction, or the ray to continue past a partial hit and composite what is behind it. The second falls out naturally once shading is deferred to the face cache in Stage 7, so the coverage stays in the visibility buffer waiting for it.

Also outstanding: per-brick dirty tracking so a changed chunk does not re-encode whole, and the beam pre-pass — which is an optimisation with no problem to solve at present (3.0 ms against a 9.5 ms budget) and which can only be validated on hardware we do not have.

## Stage 4 — original plan

Per-pixel screen-space error, **stochastic level blending** (answer N2), filtered node summaries, coverage-based edge anti-aliasing, demand-driven streaming at the right level, LOD debug overlays.

**What you can do:** stand on a huge test world, see to the horizon, no popping, no steps, no draw-distance slider, stable framerate.
**Perf gate:** frame time varies <15% between facing a wall and facing 20 km of world; no detectable transition in a slow dolly-out capture.

## Stage 5 — The chisel and clips ▶ **PLAYABLE #3** · M

- **Chisel tool** (answers H1, O5): hold left (carve) or right (place) to set the first point, move the camera, release to set the second — the box between them is filled or removed.
  - **Both** points are placed at a camera-relative distance, scroll-adjustable while a modifier key is held.
  - **Distance 0 means "snap to the voxel I am aiming at"** rather than a fixed distance — so the same tool does both precise surface work and free-space placement.
  - **Middle click adds constraint points**: extra voxels the resulting shape must touch at its edge. A box becomes a box that reaches those points; later shapes use them as defining features.
  - Box only for now; spheres, cylinders and ramps use the identical interaction later.
  - Live wireframe preview with a voxel count before you release.
- Ray-pick, GPU-side edits, dirty tracking, incremental hierarchy update.
- Per-player undo/redo over the Op log (answer H3).
- Selection, copy, paste, rotate, mirror, scale — saved as **clips** into your library (answers H2, H4).
- Terrain smoothing/erode/noise brushes (answer H6).

**What you can do:** build and dig at 3 cm resolution. **First genuinely fun build.**
**Perf gate:** a 137k-voxel chisel box applies in ≤0.4 ms and never drops a frame.

### Progress

Chisel, picking, preview and unlimited undo/redo are **done and measured**. The clipboard — select, ghost, move, repeat, rotate, mirror, stamp — is **done and measured**. Scaling a clip and the terrain brushes are **not started**; so is saving a clip into a library, which is where `.wsclip` arrives.

**Controls as built.** Tap 1–9 to pick a tool slot; hold a number and scroll to cycle the tools on it. Chisel on 1, clipboard on 2. Movement is WASD, space up, **C down** — not Ctrl, which belongs to Ctrl+Z. Backspace cancels.

| | chisel | clipboard |
|---|---|---|
| left | carve | select, then stamp |
| right | place | — |
| middle | constraint point | constraint point |
| wheel | (with G) working distance | slide the ghost along the axis you face; with shift, a whole clip length |
| P | overwrite, or fill empty space only | paste mode: replace / matter only / into empty space |
| O | first point against the face, or on the voxel you look at | grid snap: copies tile, and the turn step becomes 11.25° |
| . / , | — | copies, or size — whichever **/** is pointed at |
| / | — | cycle between copies and size |
| arrows | — | turn the clip; in size mode, up/down stretch the axis you face |
| Z / X | undo / redo | undo / redo |
| R | clear points | drop the clip |
| backspace | cancel | cancel |

The clipboard's right button drops the ghost or abandons a selection; middle click sends the ghost to the crosshair. Scrolling accelerates while the run continues and resets when it reverses, changes axis, or stops — the first click of a run is always one voxel. With nothing selected the wheel goes back to flight speed. Switching tools drops the clip.

**Copies carry a share of the transform.** Copy n of N gets n/N of the rotation and the resize, so the last one carries all of it: a quarter turn over four copies is 22.5°, 45°, 67.5°, 90°. Size ramps geometrically, so half way to twice is 1.41×. Each copy is its own baked clip and its own upload; with no transform they all share one.

**Resizing is any ratio, per axis, down to a single voxel.** Unlike rotation it does not preserve the voxel count — it cannot, since doubling a thousand bricks means eight thousand. What it does guarantee is that the destination is never torn, and that thin features survive: **air gets no vote**, so a one-voxel diagonal, a right-angled corner and a one-voxel wall all come through a shrink intact rather than being outvoted by the air around them.

Transforms apply **turn first, then resize**, so a stretch follows the world axis you are facing rather than the clip's own — and so the shear passes run on the small clip rather than the enlarged one.

**O** makes the clip move in whole clip lengths, so copies tile and it carries its own spacing, and changes the turn step from 7.5° to 11.25°. Both divide a quarter turn evenly, so a right angle is exactly reachable either way.

**The copy count is signed, and the two directions mean different things.**

- **Positive** — you have set the two *ends*. The copies share out the space between the original and the ghost, and the last one lands exactly on the ghost.
- **Negative** — you have set the *stride*. The whole transform is one step, and each copy takes that step again from where the last one ended, so adding a copy makes the row reach further rather than packing another one into the same span. Where the step also turns, the row bends round with it. The copy at the far end is the one drawn with an outline, because it is the end you watch.

Winding the count down past one carries straight on into the negatives; zero is skipped.

**No caps.** Not on selection size, edit size, resize, or copy count. What used to stop a large edit also stopped the clipboard selecting a large building, because selecting *is* the chisel. What it costs is reported instead — the developer panel shows how long the last edit and the last bake took — and a clip that will not fit in memory fails its allocation, which is caught rather than fatal.

A stamped clip is always on the world lattice. A clip that keeps its own lattice at its own angle is a **free-standing object** — decision D56, arriving with rigid bodies in Stage 12.

**Bake cost**, which is what a resize or rotation keypress costs, measured on the dev machine at roughly 16 ns a cell:

| copies, at the largest size each is allowed | cells baked | bake |
|---|---|---|
| 1 | 1.6 M | 17 ms |
| 8 | 2.4 M | 40 ms |
| 16 | 2.9 M | 60 ms |

The size a resize can reach is capped from that budget *before* anything is baked, so the limit is felt as the ghost stopping rather than as the frame stopping.

**Ghost cost**, resolve pass, against its 0.80 ms budget. The budget is written for a Steam Deck at 1280×800, so that is the column that answers it; 1440p is 3.6× the pixels and is here for scale.

| | 1280×800 | 2560×1440 |
|---|---|---|
| no clip | 0.07 ms | 0.17 ms |
| five copies, each rotated differently | 0.16 ms | 0.37 ms |
| a 120-voxel cube of mostly air | 0.09 ms | — |
| sixteen small copies overlapping and filling the frame | **0.28 ms** | 0.93 ms |

The last row got about 15% slower when clips gained their occupancy mask, because the extra read happens whether it skips or not. The row above it is why the mask is there: before it, a ray entering a large clip spent its whole step budget walking empty voxels and the ghost drew blank.

Not started: saving clips into a library (`.wsclip`), and the terrain smooth/erode/noise brushes.

Measured on the dev machine, a 52³ box (140,608 voxels), including capturing what it would take to undo it:

| Where the edit lands | apply | undo capture | total |
|---|---|---|---|
| Open air | 0.174 ms | 0.081 ms | **0.282 ms** |
| Uniform ground | 0.180 ms | 0.223 ms | **0.418 ms** |
| Dense hand-built geometry | 0.843 ms | 0.531 ms | **1.386 ms** |

The gate is met in the first two cases and missed by 3.5× in the third, where every brick in the region holds several materials and neither the whole-brick fill nor the uniform-collapse in undo capture can do anything. It is a one-off cost on mouse release against a 16.6 ms frame, so no frame is dropped — but the gate is written as an unconditional number and is not being met unconditionally, and that is recorded rather than quietly rewritten. Fixing it properly means slicing an edit across frames, which Stage 16 needs regardless.

Getting there took three changes, each found by measuring rather than guessing: bricks the box covers completely are overwritten wholesale with a histogram for the ledger instead of 512 read-modify-writes (`Brick::type_histogram`); bricks it partly covers resolve the palette slot and encoding once for the range instead of once per voxel (`Brick::fill_range`); and undo capture decodes a brick in one pass rather than through `get()` per voxel. Together, 3.46 ms → 1.39 ms on the worst case.

---

# PHASE III — Materials and light

## Stage 6 — Materials and pattern generators ▶ **PLAYABLE #4** · L

Material definitions (hot-reloadable data files), tags, properties, GPU-packed hot subset; **pattern generators** — noise, fractal, strata, voronoi, layered — emitting real tagged voxels (answer C9). Sandstone, granite, oak, rusted iron, marble, brick shipped as examples, procedurally authored by me (answer L2). In-game material browser, eyedropper, authoring panel (answer H7). Provenance tags so digging sandstone yields "sandstone" (answer C10).

**What you can do:** build with materials that genuinely look like materials, dig in and see correct internal structure, author your own.
**Perf gate:** ≤0.1 ms/frame added; a material brush stroke within 20% of a plain one.

## Stage 7 — Face cache and direct light ▶ **PLAYABLE #5** · XL

Face cache hash table, allocation, LRU, parent seeding; visible-face feedback and priority; sun with shadow rays; analytic sky; **any-voxel emissive** via an incremental alias table (answer D6); cache-space spatial + temporal denoise; quality tiers.

**What you can do:** a properly lit world with soft shadows, sky light and glowing voxels, with almost no visible noise.
**Perf gate:** T0 ≤9.8 ms for selection + shading + denoise; full re-convergence within 8 frames after a large edit.

## Stage 8 — Global illumination ▶ **PLAYABLE #6** · L

Indirect bounces reading the previous frame's cache (2 bounces + sky default, answer D4), importance sampling, next-event estimation, energy clamping and ledger, GI-derived ambient occlusion, plus the **no-GI fallback tier** with SSAO/SSR (answers D9, D10).

**What you can do:** colour bleeding, light down a tunnel, a room lit only through a doorway.
**Perf gate:** GI adds ≤1.5 ms over Stage 7 at the same tier; no energy runaway over a 10-minute soak.

## Stage 9 — Glass, water, caustics, atmosphere ▶ **PLAYABLE #7** · XL

Per-pixel specular pass; refraction with Snell + Schlick; fill-gradient smooth normals for fluids; Beer-Lambert absorption; dispersion via hero wavelengths on flagged materials (answer D7); caustic photons deposited into the face cache (answer D8); **volumetric fog and aerial perspective** (answer D13); **full physically-derived post stack** — exposure, bloom, DOF, speed-based motion blur, tonemap, TAA, upscaler (answer D14).

**What you can do:** build glass, prisms, lenses and pools; see rainbows, real caustics, underwater shafts, mirrors, fog and sun rays.
**Perf gate:** T0 ≤3.9 ms for specular + photons + fog; prism converges to a clean spectrum in ≤20 frames.

---

# PHASE IV — Be in the world

## Stage 10 — Player, collision, full-body first person ▶ **PLAYABLE #8** · M

Capsule-vs-bitmask collision; walk, sprint, crouch, jump, swim, step-up, ladders; creative fly/noclip/infinite reach/brush scaling (answer G7); **camera between the eyes of a real, shaded 2 m player model** with visible body, hands and tools (answers D15, G8); build feedback and highlighting.

**What you can do:** walk around your builds at human scale and look down at your own body. Everything you built suddenly has a size.
**Perf gate:** collision ≤0.2 ms/tick for 8 characters.

---

# PHASE V — Living matter

## Stage 11 — Cellular simulation ▶ **PLAYABLE #9** · XL

Awake/asleep tracking; propose/claim/vacate transfers; granular; **fluids with pressure, momentum, current and mixing** (answers E4, E5); **gases mixing, buoyant, exactly conserved** with the atmosphere pool (answer E6); **full heat diffusion including air** (answer E7); data-driven reactions with the mass-balance validator; matter audit in CI; debug views for awake bricks, fill, temperature and transfers (answer E15).

**What you can do:** pour water and watch it find its level and keep its current, flood a tunnel, set a wooden building alight, watch hot air and smoke rise, melt things. Every drop is accounted for.
**Perf gate:** 15k awake bricks in ≤4.5 ms on T0; matter drift exactly zero over the audit.

## Stage 12 — Rigid bodies, destruction, integrity ▶ **PLAYABLE #10** · XL

VoxelVolumes with fixed-point transforms and BVH ray-marched rendering; connected-component detection; bitmask collision; fixed-point impulse solver; sleeping and re-baking; denting and fracture (answer E11); **support-propagation structural integrity** (answer E8); **explosions as propagating pressure waves** (answer E14); **gravity field artifacts** (answer B6).

**What you can do:** knock down and blow up what you built, watch it tumble, settle and become terrain again. Undermine a wall and it collapses. Bend gravity and watch everything obey.
**Perf gate:** 600 active bodies ≤2 ms on T0; a settled demolition returns to zero active bodies within 10 s.

## Stage 13 — Cloth, rope, soft bodies, soaking ▶ **PLAYABLE #11** · L

Fixed-point Verlet with graph-coloured deterministic solving (answer E12); cloth, rope, soft solids; voxel binding and deformed-space rendering; two-way collision with world, bodies and fluid; **soaking into cloth and soft bodies, and stains on any surface** (answer E13).

**What you can do:** hang flags and banners, string ropes, build jelly, soak a cloth, leave paint and mud marks that wash off.
**Perf gate:** 60k particles ≤2 ms on T0.

---

# PHASE VI — Scripting, content, interface

*Promoted ahead of multiplayer because modding is core (answer K5) and because scripts must be designed as op-emitters before the network exists.*

## Stage 14 — Lua scripting and the mod pipeline ▶ **PLAYABLE #12** · XL

- Embedded Lua 5.4 (MIT), sandboxed, with a hard per-tick time budget and a HUD report.
- **Scripts never mutate shared state directly** — they read a snapshot and emit ops, which is what keeps determinism intact while letting mods use floats freely (`11-reality-check.md` §8).
- API surface: voxels, types, materials, tags, properties, reactions, clips, entities, input, UI, events.
- Mod packaging, load order, content hashing, hot reload.
- Reactions authorable three ways (answer C7): data files, Lua, and the node editor (Stage 20 supplies the node editor UI; data + Lua land here).

**What you can do:** write or install a mod that adds materials, reactions, tools or whole game modes (answer B1: "any mode of game the player builds or scripts").
**Perf gate:** Lua ≤0.8 ms/tick on T0; a script exceeding budget is suspended without a frame hitch.

## Stage 15 — Diegetic UI, world manager, clip library ▶ **PLAYABLE #13** · L

In-world UI framework (panels as real lit geometry, ray interaction, pixel font — answers D16, L3); main menu; **many-worlds manager** (answer B3); clip library with save/load/stamp and procedural clips (answer H4); `.vox` and `.schematic` import, `.wsobj` clip export (answers B8, H5); remappable controls (answer L5); the `.wsworld` single-file container with append-only journaling and zero-stall save-on-every-edit (answers K1, K3).

**What you can do:** manage many worlds, keep a library of your creations, import from MagicaVoxel, and never lose work.
**Perf gate:** UI ≤0.6 ms on T0; save-on-edit stalls the main thread by 0 ms.

---

# PHASE VII — Together

## Stage 16 — Multiplayer ▶ **PLAYABLE #14** · XL

UDP transport with channels, reliability, congestion control and encryption; **connectivity ladder** IPv6 → LAN → STUN hole punch → peer relay (answer M1); **invite codes carrying a username** with duplicate suffixing (answer J2); **region-ownership distributed authority for 32 players** with gossip trees and no host (answers J1, J4); client-side prediction and rollback; chunk digest reconciliation; interest management; content-hash handshake; determinism CI across CPU and GPU backends; packet loss/latency/jitter simulator in tests.

**What you can do:** build with up to 32 friends. No port forwarding, no server, no setup. Anyone can leave, including whoever started it, and the world carries on.
**Perf gate:** 32 players, ≤120 KB/s upstream worst case; join ≤15 s; zero desyncs over a 1-hour soak at 5% packet loss.

---

# PHASE VIII — Life

## Stage 17 — Characters and procedural rigging ▶ **PLAYABLE #15** · XL

- **Freeform voxel blob → automatic skeleton** (answer G2): thinning/skeletonisation of the voxel shape, limb graph extraction, joint placement, geodesic skinning weights.
- **Procedural locomotion** (answer G4): gait synthesis from limb count, length and mass; foot IK; look-at; balance-aware stepping. Works for humanoids, quadrupeds, winged, legless.
- Character editor built on the chisel and clip tools; per-voxel destructible characters (answer G5); simulated hair/cape/cloth on the character (answer G6).
- Sanity limits only where they break the game (answer G3): no invisibility, no mountain-sized players.
- Manual-bone fallback so a failed auto-rig can never block a player.

**What you can do:** sculpt any creature you like out of voxels and it walks, correctly, without you animating anything.
**Perf gate:** auto-rig a 40k-voxel character in ≤300 ms; 8 rigged characters ≤1 ms CPU, ≤0.5 ms GPU on T0.

## Stage 18 — Active ragdoll, NPCs, combat ▶ **PLAYABLE #16** · XL

Motorised joints with PD control; balance controller; protective behaviours (bracing, reaching, grabbing, staggering); continuous transition from controlled to limp as damage accumulates (answer B2, "Euphoria-like"). NPC perception, navigation over destructible voxel terrain, behaviour trees driven by Lua. Melee/ranged combat basics, damage as real voxel destruction.

**What you can do:** fight and be fought, and watch bodies react like they have weight and intent instead of switching to ragdoll.
**Perf gate:** 8 active ragdolls ≤2 ms/tick on T0.

---

# PHASE IX — The infinite world

## Stage 19 — Terrain and the world-generation node editor ▶ **PLAYABLE #17** · XL

- GPU 3D density field: domain-warped fractal noise, fully volumetric — caves, overhangs, arches, floating islands (answer F3), infinite downward (answer F8).
- **Fractal, open-ended biomes** with variable blending types and no fixed biome list (answer F1).
- **A node-graph world-generation editor** (answers F2, F5): the player edits scales, materials, and what populates the world — including using their own saved clips as the "trees" — before generating.
- Local water bodies born settled (answers B10, F4); ore/vein/strata as real tagged voxels.
- Generation entirely on the GPU inside the streaming path; edits stored as a sparse journal on top of the seed (answer F7).
- Flat creative world stays a permanent shipped mode (answer F6).

**What you can do:** design a world with a node graph, then spawn into it and walk forever in any direction through fully destructible, fully 3D terrain.
**Perf gate:** 8 m³ chunk generated ≤0.2 ms on T0; sustained 30 m/s flight with no hitches.

---

# PHASE X — Machines

## Stage 20 — Logic and wires ▶ **PLAYABLE #18** · XL

Node-graph visual programming plus **physical wires and logic components in the style of LittleBigPlanet/Dreams** — but destructible, physics-simulated, and attachable to moving objects (answer I1). Signal propagation on its own event-driven tick synchronised to the sim tick (answer I2, my call: separate tick, event-driven, not polled). Sensors (pressure, light, heat, fluid, proximity), displays, timers, math and logic nodes. **Accounted matter source/sink ops** so drills and printers cannot break conservation (answer I6). The node editor UI here also serves reaction authoring from Stage 14.

## Stage 21 — Mechanisms and vehicles ▶ **PLAYABLE #19** · XL

Joints — hinge, slider, motor, piston, spring, rope-attach — integrated into the fixed-point rigid solver. **Placed components that are themselves made of voxels, fully customisable in look and behaviour, and destructible** (answer I3). Arbitrarily large articulated machines (answer I4). **Moving reference frames with true momentum inheritance** — stand on a moving vehicle, jump, and carry its velocity (answer I5).

**What you can do:** build a working drawbridge, crane, elevator, car, or aircraft out of your own voxels and logic, and ride it.
**Perf gate:** a 5,000-voxel machine with 20 joints ≤0.5 ms/tick on T0.

---

# PHASE XI — Ship

## Stage 22 — Persistence, sharing, tooling · M

`.wsworld` container hardening, versioning and migration (answer K2: breaking saves acceptable during development, so migration lands here); blueprint and clip export/import; procedural clips; mod distribution; replay hooks recorded from the op log (answer K4, deferred feature).

## Stage 23 — Optimisation, polish, release · XL

Full Steam Deck optimisation pass on real hardware; dynamic resolution and auto-detected quality; settings; **audio** — material-driven impacts, footsteps, fluids, ambience (answer E16, out of scope for v1, lands here); onboarding; controller support; crash reporting; opt-in telemetry; soak and leak audits; Unlicense + third-party notice bundle; Steam build pipeline and store page (answers A10, A11).

---

## Ordering rationale

- **Rendering before simulation.** You cannot debug a fluid you cannot see. Every simulation bug from Stage 11 on is diagnosed with debug views built on Stages 3–9.
- **Chisel before materials before lighting.** Each one makes the previous one's flaws obvious.
- **Scripting before multiplayer.** Scripts must be op-emitters by design; discovering that after the network exists would mean redoing both.
- **Terrain late (Stage 19), as you asked.** A flat world is enough to build everything else, and terrain is the system most likely to churn on taste. Building it last means building it once, on a finished materials system, with a node editor that the UI framework already supports.
- **Multiplayer at 16, not 23.** Late enough that the systems exist; early enough that characters, ragdolls, terrain, logic and mechanisms are all designed and tested networked from birth.
- **The Op discipline starts at Stage 1** — fifteen stages of apparently pointless indirection that turn Stage 16 from a rewrite into an integration.

## Risk register

| Risk | Stage | Mitigation |
|---|---|---|
| Visibility marching too slow on Steam Deck | 3–4 | Both marching and raster-prepass paths built and measured; bandwidth budgeted per pass; dynamic resolution as the release valve |
| Face cache thrashing | 7 | Priority scoring, parent seeding, LRU tuned on real scenes; cache scales with detected memory |
| GPU determinism across vendors | 11, 16 | Integer-only from Stage 0, order-independent atomics only, CPU reference cross-check in CI, chunk reconciliation as a hard safety net |
| Conservation drift | 11 | Structural, not corrective; CI audit fails the build |
| 32-peer serverless authority | 16 | Region ownership + gossip trees + relay fallback; the deterministic op log means the world is never at risk |
| Auto-rigging failing on weird shapes | 17 | Manual-bone fallback ships alongside it |
| Scope, one developer | all | 19 independently playable checkpoints; enjoyable and shippable long before Stage 23 |
