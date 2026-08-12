# 21 — The renderer rewrite

*Planned 2026-08-09, and being built against. §8 is the work plan and §8.0 is where it stands;
the decision log gets the decisions as each sub-step lands. R0 and the first half of R1 are done.*

Three systems are rewritten from scratch together, because they are one system pretending to
be three: the path tracer, the chunk system, and streaming with its coarse-resolution tiers.
They are rewritten together because every fault in each of them is the same fault — something
in the renderer is decided per screen pixel that should be decided per piece of world, or
decided per chunk that should be decided per pixel. Both directions of that mistake are in the
current build at once.

---

## 1. The rule

> **Every ray in the renderer starts on a voxel face. A screen pixel gets one visibility ray
> and one lookup, and never a second ray of any kind.**

That is the whole plan in one line, and everything below is a consequence of it.

It is not a reduction in features. A face does not have to store one number. A face stores a
**distribution over direction**, and the number of directions it is cut into is decided the
same way everything else in this renderer is decided — by how many pixels the thing covers.
So:

| Effect | Where it lives now | Where it lives after |
|---|---|---|
| Diffuse / GI | face cache (irradiance) | face, irradiance term |
| Sun and shadow | **traced per pixel**, cached per face | face, traced in the face pass |
| Emissive lamps | per-pixel NEE draw | face, traced in the face pass |
| Rough reflection | 24 direction bins, read per pixel | face, direction bins |
| Sharp reflection | **traced per pixel**, one in sixteen | face, more bins — count from pixel coverage |
| Refraction / glass | **per-pixel loop of four marches** | face, transmitted bins |
| Translucency | **per-pixel scatter-into loop** | face, transmitted term |
| Dispersion | not built | face, hero wavelength per face sample |
| Caustics | not built | face, photon channel (as `04-rendering.md` §3 always said) |
| Smooth normals on curves | per-pixel gradient of occupancy | **deleted** — a face is flat, and detail comes from real smaller voxels |
| Level dither | per-pixel blue noise | **deleted** — the composite blends two levels deterministically |

Nothing on that list is lost. Four things move off the pixel, two things are deleted because
real geometry replaces them, and two things get built that were never built.

**What stays per pixel, and why it is not a violation.** Exposure, tone mapping, bloom, motion
blur and the lens are *film*, not transport — `pt_post.glsl` already draws that line in its own
header, and it is the right line. Light transport answers "how much light arrives"; film
answers "what should that look like on a screen". Film is allowed to be per pixel because a
screen is per pixel. Transport is not.

---

## 2. Chunks come out of the renderer

### What a chunk is doing today

`ChunkCoord` is load-bearing in nine places: the wrapped GPU grid, chunk records, brick masks,
popcount prefixes, contiguous brick-slot runs, the five coarse occupancy grids, the summary
octree's blocks, the thumbnail caches' own wrapped grids, and the feedback buffer's entries.
The marcher has **four different addressing schemes** glued end to end — chunk grid, brick
mask, brick mips, summary tiers — and every seam between them has produced a bug with its own
decision-log entry (D133, D137, D147, D148, D151, D155).

The seams are not accidents. They exist because a chunk is a fixed 8 m box in a renderer whose
entire premise is that nothing has a fixed size.

### What replaces it: one node pool

A single hash-addressed, pointered sparse octree, holding nodes at every scale from a 3.125 cm
voxel upward, with no chunk anywhere in it.

```
NodeEntry                                              (32 bytes + variable payload)
  key            u64x2   (level, node coordinate at that level), probe-verified
  child_slots    u32     base index of this node's eight children, or kNone
  child_mask     u8      which of the eight exist — the empty-space test, one byte
  face_coverage  u8[6]   how much of this node is matter as seen along each face direction
  colour         u32     rgba8 filtered over the subtree (D139: never rounds to nothing)
  flags          u8      leaf · uniform · has_emissive · has_transmissive
  last_wanted    u32     LRU, by the frame something asked for it
  payload        ptr     leaf only: the voxel data, from a size-classed pool
```

The eight children of a node are **contiguous**, so a node holds one base index rather than
eight. A ray therefore pays **one hash probe per ray**, to find its entry node, and then walks
by direct indexing all the way down. Today it pays two dependent loads per chunk entered — up
to thirty-two times along each axis a ray crosses — plus separate coarse-grid fetches per skip.
This is strictly less memory traffic, which on the Steam Deck is the only currency that matters.

The descent is the one in `04-rendering.md` §1, finally as written: test the child mask, skip
empty children with no memory traffic, descend while the node's projected footprint exceeds the
pixel footprint, stop.

**What disappears entirely:** `residency.hpp`'s wrapped chunk grid, chunk records, brick masks,
prefixes, slot runs, the five coarse occupancy grids and their window rule, `summary_tree.*`,
`thumbnail.*`, `thumb_cache.*` and all eight thumbnail tiers. The world-occupancy oracle
disappears too, and with it the entire class of fault D133 and D146 describe — because there is
no second structure that can disagree with the first about whether something exists. A parent
node's `child_mask` is the only answer, and a parent is always resident before its child is
reached.

**What survives, and is the one interpretation in this plan:** a chunk stays as a **page** on
disk and on the wire. `06-multiplayer.md` §4 reconciles per chunk, `05-simulation.md` wakes and
sleeps bricks, authority regions are 64 m cells, and `.wsworld` writes chunk-sized records.
None of that is rendering, and none of it benefits from being torn up now. So: **chunks vanish
from the renderer, from GPU residency and from streaming; they remain a storage grouping the
renderer never sees.** Removing them from storage as well is a separate change with multiplayer
consequences and no rendering payoff, and it is not in this plan.

---

## 3. Streaming is what the pixels asked for, and nothing else

### The rule, stated exactly

1. A ray descends only while a node's projected footprint exceeds the pixel footprint. **A node
   smaller than a pixel is never fetched, never uploaded, never shaded, and does not exist.**
2. A node a ray wanted and did not find is reported as `(key, level)`. That is the only reason
   anything is ever loaded.
3. A node nothing has asked for in N frames is evicted, cheapest-to-regain first.
4. **The one exception the user named:** a proximity radius around the player is held resident
   regardless of visibility, at full voxel detail, because collision, physics and editing need
   to touch what is behind you and under your feet. **Twenty metres**, chosen by the user so a
   thing can be carved without turning round to look at it first.

Twenty metres is affordable, and it is affordable *because* of the node pool rather than by
luck. A sphere of that radius is 2.1 million bricks, and under the old chunk system a camera
underground would have claimed a slot for every one of them — which is precisely the failure
D128 records, where uniform bricks cost eight bytes of payload and a whole slot each and the
slot pool ran dry at 8 MB of a gigabyte. A node pool folds a uniform subtree into **one node**,
so standing inside a mountain costs a handful of entries rather than two million. What is
actually paid for is the surfaces: about 6 MB through flat ground and 30–60 MB through dense
built geometry.

### Why pull alone works now, when it did not before

D140 introduced a push tier — thumbnails held by CPU radius — because pull had deadlocked and
frozen the world. The deadlock was always the same shape: the structure that answered "does the
world have something here" was a *different* structure from the one the ray was walking, so the
two could disagree, and a disagreement meant either a phantom request repeated for ever (D133)
or a real chunk never requested at all (D147).

With one pool there is no second structure. A ray reaches a node only through its parent, the
parent's `child_mask` says whether the child exists, and the parent is resident by construction
because the ray got to it. **A ray can therefore never fail to report something that exists,
and can never report something that does not.** The deadlock is not fixed, it is unrepresentable.

So the push tier goes, and with it the 15% of video memory D145 gave it, the radius, the work
list, the rescan interval, and the three bugs (D142, D144, D147) that came out of maintaining a
second residency system with the opposite polarity to the first.

### Resolution drives memory, which is the point

Because the descent stops at the pixel footprint, the resident set is bounded by **screen
resolution**, not by world size and not by camera position. Halve the resolution and every ray
stops one level earlier: a quarter of the nodes, a quarter of the bandwidth, a quarter of the
upload. That is a real dynamic-resolution lever, and it is the one `19-auto-quality.md` records
as carried-but-not-applied. Here it is applied to everything at once, for free, because it is
the same number the marcher was already using.

---

## 4. The face store

### One entry per face, with a payload

The current table holds up to four separate entries per face — irradiance at the pixel's level,
six ancestor levels above it, a shadow entry at level 0, a shadow parent at level 6, and up to
ninety-six radiance bins — all as 32-byte records in one 256 MB table that is measurably
refusing slots. It is a table fighting itself.

The rewrite gives a face **one** entry with a variable payload, from a size-classed pool —
exactly the arrangement `gpu_brick.hpp` already uses for brick payloads, and for the same
reason: most faces are cheap and a few are not.

```
FaceEntry                                              (32 bytes + variable payload)
  key         u64x2   (node key, face direction) — the same key the marcher stopped on
  irradiance  rgb9e5  what arrives over the hemisphere: everything a diffuse surface needs
  photons     rgb9e5  caustic energy deposited by the photon pass
  visibility  u8      the sun, as a fraction, with its penumbra resolved over the samples
  samples     u16
  variance    f16     for prioritising, and for how far the denoiser reaches
  bins        ptr     null for a matte face; otherwise the directional payload
  last_wanted u32

Directional payload, allocated only where it is read
  out[n]      rgb9e5  outgoing radiance per direction bin — reflection
  in[n]       rgb9e5  transmitted radiance per direction bin — refraction and translucency
```

**A matte stone wall — which is most of a world — allocates no payload at all.** A polished
floor allocates bins. A pane of glass allocates both halves. Nothing pays for a feature it does
not have, which is the same rule the brick encodings already follow.

### Bin count comes from pixel coverage

This is the part that makes "everything on faces" work for a mirror.

A direction bin is an average over a cone, and a mirror has no cone — which is exactly why
D183 gave up and traced near-mirrors per pixel. But the width of the cone is a *choice*, and
the honest thing to choose it from is the same quantity everything else here is chosen from:

```
bins = f(roughness, pixels this face covered last frame)
```

A mirror across the room, covering forty pixels, gets twenty-four bins and looks like a mirror
across a room. The same mirror filling your screen gets several hundred, because it is worth
several hundred, and the cost is paid by exactly the surface that earned it. Angular resolution
tracks screen coverage the way spatial resolution already does. There is no roughness threshold
anywhere, no `kSpecularSharp`, no `kSpecularBlurry`, no `cache_share` ramp, and no per-pixel
escape hatch.

**Honest limit, stated once:** a perfectly flat mirror at grazing incidence will always be
angularly quantised, because a face's stored function has finite resolution. What that costs is
that a reflection is soft where the surface is polished and the view is glancing. It does not
pop, it does not swim with the camera, and it does not cost a ray per pixel. Compared with
today — where a mirror is either a 24-bin smear or one traced ray in sixteen with the rest
reading the smear — it is better in every case, and it is the same everywhere on the screen.

### Refraction and translucency, per face

A face flagged transmissive is shaded twice by the face pass: once for what it reflects, once
for what comes through it.

The transmitted half is one ray in the face pass — refract by the material's index, march to
the exit face, read *that* face's outgoing bin, apply Beer-Lambert over the exact voxel distance
crossed, deposit into the entry face's `in[]` bin for the incoming direction. Total internal
reflection and Fresnel are decided at the face, once, with samples, instead of per pixel per
frame. Dispersion is a hero wavelength per face sample, reconstructed by the face's own
accumulation — which is cheaper here than it was per pixel, because a face has hundreds of
samples where a pixel had one.

The composite then does what it does for everything else: reads the bin the view direction
points down, and adds it. **A pane of glass costs the composite one extra fetch.**

---

## 5. The frame

```
1  Streaming        serve the nodes the last frame asked for, evict by LRU     (transfer)
2  Visibility       one ray per pixel: node key, level, face, depth, coverage  (compute)
3  Face select      compact the requests, prioritise, set each face's bin count(compute)
4  Face shade       B faces × M paths, budgeted                                (compute)
5  Photons          caustic deposit into the face photon channel               (compute)
6  Face denoise     a-trous over the face lattice + temporal EMA               (compute)
7  Composite        one face lookup per pixel, two levels blended, media, sky  (compute)
8  Post             exposure · bloom · motion blur · tone map                  (compute)
9  UI               tools and overlay                                          (compute)
```

Steps 3–6 are **budgeted and independent of resolution**. Steps 2, 7 and 8 are the only ones
that scale with the screen.

### Two producers feed the face request buffer

Visibility writes the faces the eye can see. **The face pass also writes the faces its bounces
land on**, so a wall behind you that lights the wall in front of you gets shaded even though no
pixel sees it. Light therefore propagates one face-hop per frame, which is the mechanism the
current tracer already relies on (D163) and which is what makes bounce depth unbounded for
nothing. Priority favours primary requests; eviction is by *last wanted*, not by last seen, so
off-screen contributors survive.

### Level blending replaces the dither

The composite reads the face at level `floor(L)` and at `floor(L)+1` and mixes by the fraction.
Two 32-byte reads instead of one, no randomness, and detail is a genuinely continuous function
of distance rather than a stochastic approximation to one.

The ordered dither exists today because a marcher cannot cheaply evaluate two levels at once. A
deferred composite can, because it is reading a table rather than walking a tree. Removing it
removes a documented noise source: D132 records that the dither is what scattered the
dotted-line artefact across a whole surface instead of confining it to one distance band.

### Edge anti-aliasing without a history buffer

`face_coverage[6]` in the node is coverage **as seen along a face direction**, folded from
children at build time. That is the quantity Stage 4 needed and did not have — it tried
volumetric fill fraction as an alpha, which is why distant window frames dithered into the sky.
Per-direction coverage is the correct one, it is exact, and it costs six bytes a node.

The visibility ray records up to three partial hits, and the composite blends them front to
back. Real analytic anti-aliasing, no temporal component, no ghosting, no history buffer, and
no reprojection to get wrong.

**Consequence worth stating plainly:** with the level dither gone, per-pixel stochastic shading
gone, and edge AA analytic, **there is no per-pixel random number left in the renderer.** Two
identical frames from an identical camera are bit-identical. Noise, speckle and smear are not
filtered better — they have nowhere left to come from. What remains is the face store's own
convergence, which is spatially smooth by construction and bounded by the sample budget.

---

## 6. Why this is faster, with the arithmetic

### Lighting stops scaling with resolution

| | rays per frame, 1440p | rays per frame, 4K |
|---|---|---|
| Today: primary | 3.7 M | 8.3 M |
| Today: secondary (stride 4, two marches) | 1.85 M | 4.15 M |
| Today: total | **5.55 M** | **12.5 M** |
| After: primary | 3.7 M | 8.3 M |
| After: face pass (300k faces × 4 paths × 2 rays) | 2.4 M | **2.4 M** |
| After: total | **6.1 M** | **10.7 M** |

The face figure does not move between the two columns, and that is the whole result. Going from
1440p to 4K costs the primary ray and nothing else.

Face count grows sub-linearly with resolution rather than proportionally, and the reason is
arithmetic: at a 90° lens and 1440 lines a voxel already covers a whole pixel at 22.5 m, so
everything nearer than that is at level 0 and gains nothing from more pixels. At 4K the crossover
moves to 33.8 m, so only the 22.5–33.8 m band gains faces at all. (In infinite-detail mode this
stops being true and face count tracks pixels — see §7 — but it is capped by the budget, and a
cap costs convergence latency, never framerate.)

### The enclosed room stops being a special case

This plan was first written against the figures in `19-auto-quality.md` — 2.495 ms outdoors and
19.988 ms enclosed. R0 withdrew both: the pass they were read from was not being timed at all
(D201, D203). The measured numbers, quality 7 on the RTX 5060 Ti, means over sixty frames:

| | 1280×800 | 1440p | 4K |
|---|---|---|---|
| path traced, outdoor | 12.25 ms | 45.94 ms | — |
| path traced, enclosed | 39.89 ms | 148.67 ms | 346.84 ms |
| real time, enclosed | 1.81 ms | 5.93 ms | 13.09 ms |

Two things change in the argument, and the second is stronger than what it replaces.

**The enclosed/outdoor ratio is 3.25×, not 8×** — 3.26 at 1280×800 and 3.24 at 1440p. The eight
came from comparing two numbers taken at different times.

**The tracer is slightly *super*-linear in pixels.** 1280×800 → 1440p is 3.6× the pixels and
3.73× the time; 1440p → 4K is 2.25× the pixels and 2.33× the time. So it is not fixed cost plus
per-pixel work — it is per-pixel work with a penalty on top, and that is the cleanest evidence
for the rewrite there is.

The reason for both is structural. In a room a secondary ray never escapes to sky; it marches
until it hits a wall, and every refining pixel pays for that. The cost is *per pixel × how
enclosed you are*, and both terms are unbounded. After: secondary rays number `B × M × 2`, fixed,
whatever the room looks like. Enclosure makes each of those rays somewhat longer and makes
nothing else worse.

### Four smaller wins, each measurable on its own

- **One addressing scheme instead of four.** One hash per ray and direct indexing below it,
  against today's dependent-load-per-chunk plus per-skip coarse fetches. Bandwidth is the Deck's
  binding constraint (`09-performance-budgets.md` §1), and this is the largest single reduction
  in it.
- **Bloom is currently a full-resolution gather.** `gather_bloom` reads a disc of radius up to 12
  plus three rings — about 225 taps per pixel at 720p and **437 at 1440p**, straight out of the
  accumulation image. At 4K that is over three billion image loads a frame. It becomes a
  downsampled chain, which is what it should always have been, and the saving grows with exactly
  the resolution the user wants to raise.
- **The rgba32f accumulation buffer disappears**, and with it 32 bytes of read-modify-write per
  pixel per frame plus everything that reads it.
- **The `if (moved) trace_samples_ = 0` reset disappears** — there is nothing per-pixel left to
  reset. Motion stops costing anything at all, where today every frame in motion is a
  one-sample-per-pixel frame.

### Stability

D181 records that a fourth inlined `march` loses the device, and the current design is shaped
around that cliff — the lobe choice, the NEE coin, the specular stride and the glass loop all
exist to stay under it. The face pass is a *different shader* with one `march` inside a loop
over M paths, so the cliff is not near. The per-pixel shaders have exactly one march (visibility)
and none (composite).

The face store is written by **one invocation per face per frame**, which deletes at a stroke:
the halving compare-and-swap, the read-the-sum-twice-and-take-the-minimum hack, the
empty-then-publish key ordering, the eight-probe coldest-slot eviction, and every comment in
`pathtrace.comp` that begins "a thousand pixels stand on one face in the same dispatch". None of
those are bugs to be fixed; they are all consequences of the wrong thing owning the write.

---

## 7. Infinite detail — the experimental mode

`--infinite-detail`. Off by default until it is measured.

### The rule is the rule already there, unclamped

The marcher descends while a node's footprint exceeds a pixel and stops at level 0 because level
0 is where the data runs out. Remove the clamp and the same expression keeps going: a surface
covering more pixels resolves finer, continuously, with no band, no swap, no ladder and no
distance test anywhere. **It is not a level of detail scheme run backwards — it is the identical
line of arithmetic that already governs the coarse direction, with the floor taken off.**

Levels become signed. Level 0 is 3.125 cm; level −k is 3.125/2^k cm.

### Depth is unbounded in the format, and the limit is stated honestly

The user asked for infinite, so no number is written down anywhere, and the way to keep that
promise is to **stop using a fixed-width coordinate below level 0 at all**. A sub-voxel node is
identified by *(its parent's slot, which of the eight children it is)* — the tree is the
coordinate. Depth is then unbounded by construction: there is no field to overflow, and the
level counter is a `u8`, which reaches 3.125 cm / 2²⁵⁵ before it means anything.

The real limit is arithmetic, not storage, and it is the same limit D156 already documents at
the other end of the scale. A ray marches in 32-bit float, which carries about seven digits, so
about **twenty-four levels below a voxel — two nanometres — is where the ray stops being able to
tell two cells apart.** Long before that, nobody can get close enough to ask: at 4K with the
camera five centimetres from a wall, a pixel covers 0.046 mm, which is level −10. Fourteen levels
of headroom over the closest look anyone can physically take.

If that were ever reached, the fix is the one the world already uses for its largeness — a
floating origin, re-centred on the surface being examined — and it can be added later without
changing the format, because the format has no depth in it to change.

### Where the children come from

Three sources, in the order they are tried:

1. **The material's own field.** `src/forge/field.cpp` already evaluates a signed-distance and
   pattern field at any point at any resolution, and `20-clip-forge.md` §4 already notes that
   its node array is shaped to transliterate to a compute shader without changing. A voxel
   carrying a provenance reference to the pattern generator that made it is re-evaluated at half
   its size to produce its eight children. This is `03-voxel-data-model.md` §4's claim — "there
   is no sandstone block, only stone voxels arranged sandstone-ly" — finally taken all the way
   down: put your face against a sandstone wall and there are grains, because the field that
   decided the wall is still willing to answer.
2. **Hashed variation.** A voxel with no field gets children of its own material with the
   perturbation `20-clip-forge.md` §7 already specifies, hashed from position so it is identical
   on every machine and every run. Cheap, always available, never wrong-looking.
3. **Stored.** Once a player carves at sub-voxel scale, those children are real data in the same
   node pool at a negative level, and they persist.

### Why it is affordable

Sub-voxel nodes from sources 1 and 2 are **derived**, so eviction is free — throw them away and
regenerate on demand. Only carved ones are real. So resident memory is bounded by the screen
exactly as it is above level 0, and *persistent* memory is bounded by what the player actually
made. That is the difference between infinite detail and infinite storage.

The face store needs no change at all: a face key already carries a level, and the level is
signed. Face count per pixel is fixed by construction — a face is chosen where it is about a
pixel — so approaching a wall does not multiply the shading load, it moves it to smaller faces.
Standing 10 cm from a wall should cost within a third of standing 2 m from it, and that is the
stage's perf gate.

**Shadows get sharper as you approach, for free.** A shadow edge is quantised to the face size,
which near the camera today is a fixed 3 cm. In this mode the face size follows the pixel, so the
penumbra resolves as finely as the screen can show it. The two features turn out to need each
other.

### What does not descend

Simulation, physics, collision and the matter ledger stay at level 0. A sub-voxel node is
visual and editable, not simulated. Saying otherwise would put the 20 Hz tick and conservation
of matter into a hole neither can climb out of, and it is not what the mode is for. The
experimental flag is about what you can *see* and *carve*, not about water finding its level
between sand grains.

---

## 8. The stages, and how each one is built

Each stage builds, runs, and is measured on the fixed cameras in `tools/_grid.ps1` before the next
one starts. No stage closes on a regression it cannot name.

**Sub-steps are lettered and are the unit of work.** A stage is too large to hold in one sitting,
and a plan that only records the stage is a plan that has to be re-derived every time somebody
picks it up. Each sub-step below names the files it touches, what it must not break, and how it is
checked. Tick the ledger in §8.0 when one lands.

**The standing rules for every sub-step**, so they are not repeated ten times:

1. Build with `build.bat`, and run `build\bin\ws_tests.exe` — the whole suite, not a filter. A
   filter that silently skips the case you broke is worse than not running it.
2. Anything CPU-side is written and tested **headless first**, before the renderer touches it.
   That is how residency was built in Stage 2, and the reason is that a structure the renderer
   walks and nobody compares against the world is a renderer debugging a mirage.
3. New structures carry a `mirror_*` that walks them exactly as the shader will, asserted against
   the world. Two of the four bugs in R1a were caught by that and nothing else.
4. Measure with `tools\baseline.ps1 -Compare <the last csv>`. A number quoted from memory is how
   `19-auto-quality.md` came to hold two figures that no build in the tree could reproduce.
5. When a new thing sits *beside* an old thing, keep both until the diff is clean. `--chunk-marcher`
   exists so the two marchers can render one camera and be compared, not as a permanent option. It
   goes when the addressing behind it does, in R1e.

### 8.0 Where it stands

| | Sub-step | State |
|---|---|---|
| R0 | a. profiler, averages, warm-up | **done** — D201–D203 |
| R0 | b. `baseline.ps1`, `_grid.ps1`, `_measure.ps1`, image diff | **done** — D204 |
| R0 | c. debug view 11 + `facecount.ps1`, premise verified | **done** — D205 |
| R0 | d. record the full grid to `documentation/baselines/` | **done** — `r2-node-pool.csv`, the realtime grid at all seven cameras and three resolutions, with the node pool as the marcher and `--settle` so the runs are comparable. It was impossible before: a settled run cost 133 s and now costs a few. Previously **outstanding**|
| R1 | a. `NodePool` CPU structure + tests | **done** — D206–D213 |
| R1 | b. `node.glsl` descent + `node_visibility.comp` | **done, compiles** — D214–D218 |
| R1 | c. GPU buffers, pipeline, `--node-pool` | **done** — both marchers run, D219–D223 |
| R1 | d. diff against the old marcher, meet the gate | **done.** Faster on six views of seven and up to 3x on distance; 4.8 MB against 57.7; pictures agree to within one part in three hundred. One regression: the enclosed room, 1.108 ms against 0.699 |
| R1 | e. delete the old addressing | **done** — five slices, D521–D525. (1) One marcher: `--chunk-marcher`, `--node-pool`, F6 and `use_node_pool_` gone; `node_visibility.comp` renamed `visibility.comp`. (2) **`shaders/world.glsl` deleted** — 793 lines, no includer left. (3) **The summary octree and eight thumbnail tiers deleted** — `summary_tree.*`, `thumb_cache.*`, `thumbnail.*` and their three test files, 1,180 lines. (4) The ray clip box comes from `world_min_/world_max_` rather than `residency_.resident_bounds()`, which was the last thing the marcher read out of the chunk system. (5) **The rest of it**: `residency.*` (1,329 lines), `gpu/world_buffers.*`, `tests/test_residency.cpp`, the chunk marcher's orphaned descriptor set, the reference tracer's set with its 256 MB face cache and frame-statistics buffer, `rebuild_coarse_grids`, `--stream-frames` and `--stream-log`. `gpu/type_tables.*` is what is left of that layer: the two interned tables a voxel becomes a colour with, which the node pool does not hold. **Nothing had to be renumbered** — a Vulkan layout's bindings need not be contiguous, so the cloud pass keeps 13, 20 and 21 and the seventeen descriptors nothing declares are absent (D521). **Measured against a same-session control build**: device memory **970 MB → 112 MB**, warm start **505 → 340 ms**, and per edit **3.86 ms of coarse grids → 0.00** (D522). Picture unmoved at content `766f2fd63f1a01c4`, both pool audits and `--validation` clean, 505 tests. The gate is `documentation/baselines/r1e-chunks-gone.csv` and an interleaved pair, not the older file — see D523 and D524 |
| R1 | f. GPU mirror check for the node pool | **done** - `NodeBuffers::audit`, and it eliminated the upload as a suspect on its first run |
| R1 | g. make it the marcher the game launches with | **done** — D224–D226. The chunk marcher is behind `--chunk-marcher` until R1e |
| R1 | h. the enclosed-room regression | **done** — D227–D232. It was the descent, re-walking eleven levels every step; two cached ancestors fixed it. Finding it needed the harness fixed first |
| R1 | i. dirty-range uploads | **done** — D235–D236. The upload was 10 MB a frame while moving and eleven times over its budget; it is now 0.028 ms |
| R2 | d. draw the parent while waiting | **done, early** — D237. Taken out of order because an unstreamed region drawing as sky is what "it loads slowly" turned out to mean |
| R2 | eviction churn | **fixed** — D247–D250. The pool was throwing away the scene it was drawing every 600 frames, because a node is only marked wanted by a *miss*. D233 and D234 were both this |
| R2 | a. feedback drives everything | **done** — D251–D252. A ray reports what it USED, not only what it missed, and a request is served once however many times it is asked for |
| R2 | edit cost | **fixed** — D256–D258. One voxel used to cost the 512 m root; it now costs the brick |
| R2 | b. the sub-pixel rule | **done, with a stated limit** — D262–D269. Gate met where a pixel is coarser than a brick (far 1.17×, distant 0.68×, tolerance 1.30×); structurally unmeetable nearer, because a brick is the leaf and covers a pixel at 100 m. R8 is what lifts that floor | **half, and the half that is left is large** — D259–D261. A node finer than the pixel is never *requested*; making it never *stored* needs per-node residency, because eviction currently works at the 512 m root and the whole scene is one root. Gate measured at 3.2× over |
| R2 | residency hears what a ray READS | **fixed** — D426–D429. `touch_slot` was called only where the march returns a hit, so every brick a ray crosses on its way to one — most of a facade at a grazing angle, and every reveal, cornice and step — was read every frame and stamped never. Instrumented first, as D425 asked: **249,454 evictions on a settled static camera, 228,964 inside the frustum, 37,213 wanted again within two seconds, and 249,414 of them nodes no ray had EVER reported reading.** One report where the inner walk falls out takes primary-ray churn to **nought** and consecutive-frame flicker from 13/0/3/50 pixels to **0/0/0/0**, and it makes the outdoor camera reproducible: two runs of one build differ on **674 pixels against 12,484**. Costs nothing measurable. Then the same loop driven by light rays rather than primary ones — 28,695 of the remaining 29,017 rebuilds — closed by the symmetric half of D292's own sentence: a light ray may name the one cell that stopped it, so it may also say it is USING that cell. Rebuilds **29,017 → 4,660** at no measurable cost, once the reports are deduplicated on the card rather than sent per ray (1,538,219 against a 131,072 capacity, 1,407,147 dropped, before that). D430–D432 |
| R2 | c. proximity | **done** — D270–D272. Twenty metres at brick detail, asked of the world rather than of the volume, resumable and bounded, anchored two metres so walking cannot restart it forever. A background guarantee: standing still finishes it |
| R3 | b. the face store | **done** — `src/world/face_store.{hpp,cpp}` and twelve tests; `src/gpu/face_buffers.{hpp,cpp}` mirrors it to the card with dirty ranges and an identity audit that also reports what the card wrote (D296). Uploads by exact region, never by coalesced range: the record has two owners (D295) |
| R3 | a. split the frame | **done** — the marcher names the face each ray stopped on down the feedback buffer that already existed, AND resolves its slot into an R32_UINT image the composite reads (D290). The request lattice walks, or it samples the same 1/64 of the screen for ever (D291) |
| R3 | c. sun and lamps in the face pass | **sun done, and now visible** — `shaders/shade_faces.comp`, one jittered shadow ray per face per frame across the face and the sun's disc (D294), two counts rather than a mean (D293), no standing in for unbuilt cells (D292). Deck realtime cost: enclosed 2.382 → 2.500 ms, outdoor 1.709 → 1.892, close 2.841 → 3.009 — 3–11% for a shadow the real-time path never had, with speckle falling close 98.3 → 24.6 |
| R3 | c. lamps in the face pass | **done** — D401–D409. The path tracer's own next-event estimator, moved off the pixel: `kLampCandidates` fittings scored, one kept in proportion, one direction inside its cone, accumulated on the face as irradiance. **A face never loops over lights**, so a thousand sconces cost what one costs, and it converges at `kLampConverged` and then casts nothing — settled faces **2.613 → 3.075 ms**, +18% of the pass and inside its 4.40 ms budget, measured by interleaving the two builds because the machine drifts by more than the effect (D407). The picture moves by **21.78 of 255 over a quarter of the frame**: the portico, the one place in this building the sun never reaches, stops being lit by a constant. Instant response is the host's job, not the face's — `light_list_hash` gives the list an identity and one frame of `light_reset` reopens the store, so **73% of a lamp change is on screen on the next frame and 97.5% by frame fifteen**. Sky is R10's, and it landed first |
| R3 | shadow latency, stage one | **done** — D312–D315. `--cut` first, because a smooth camera measures the rate the store converges at and hides what it does when handed a whole screen: a 180° cut showed **five frames of a completely unshadowed room**. The mirror was uploaded above the line that claims faces, and the composite would not read a face under four samples while showing full sun instead. Both free to fix; five frames became two |
| R3 | e. claim on the card | **done** — D316–D318. A provisional table of stand-ins in the tail of the faces buffer, which the host never writes; the claim is one atomicCompSwap whose return value serves the pixel that lost it, so no fix-up pass was needed at all. The full-sun fallback is **0% of surface from the first frame after a 180° cut**, against 100% for two frames before it, and 0% at every frame of a cold start. Settled cost unchanged, `--validation` clean, two runs bit-identical |
| R3 | d. delete the per-pixel light path | **done** — D517, D518. `pathtrace.comp` (2,757 lines) and `pt_normals.glsl` (293) deleted, with the rgba32f accumulator, `--pathtrace`, F4 and every `path_trace_` branch: **3,297 lines out against 52 in**. The descriptor set is KEPT and renamed in comment only, because `clouds_.create` was always passed that layout. Warm start unchanged (538/555 ms against 551); the cost it was carrying was a cold driver shader cache, where the before-build read **8,053 ms then 551** for the same run. Picture untouched: content `1f4710eee4ee2585`, `--validation` clean, both pool audits clean, 548 tests. **`world.glsl` now has one includer left, `visibility.comp`, which is R1e's** — that is what doing R3 first bought (D278). Two things not closed: the gate's *no per-pixel random numbers* clause is **R5c's**, because the last one is `hash_u32` in the composite's ordered dither, and R3b's `GpuFace` split is still owed |
| R3 | faces are voxels | **done** — D298–D303. Every face in the store was a brick, so the finest shadow was 25 cm; now 416,261 level 0, 59,758 level 1, 1,603 level 3 on the close camera. Two collisions on zero and a shell that was transparent to occlusion came with it. Speckle enclosed 17.5 (no shadows) → 3.8 |
| R3 | the store recycles | **fixed** — D304–D306. `evict_cold` was written, tested and never called, so the store filled and then refused every face after it: the shadowed set froze and everything new was lit by the fallback. A slice a frame now, with the threshold halving when the table is full |
| R9 | d. coarse light for a face that has none | **done, early** — D308–D311. A face with no light of its own reads the face three levels above it, which 512 faces share and the request lattice cannot miss. Falling back to full sun: under 1% of the enclosed room at frame 30 against frame 78, and 2,978 wrong pixels against 283,291 at frame 40. GPU unchanged still and moving, settled picture bit-identical, store +3.0% |
| R10 | c. the polynomial over the face | **linear terms done** — D333–D335. AO was one number per face however close you stand; the first moments in the face's two axes make it a continuous field, from samples the pass already took and positions it was throwing away. No rays, no passes, no least squares — the Legendre basis is already orthogonal under the jitter. Adjacent pixels holding an identical value fall **48.7% → 23.2%** on a terrace patch; cost unchanged within noise. The self-occlusion gate is closed properly: 8,481 pixels that see open sky read a mean contact of **0.9966**. The quadratics were then built and **taken out again** (D336–D337): they moved the picture by less than the renderer's run-to-run noise, because since D298 a face IS a voxel and a voxel on a column is flat — curvature on a voxel cylinder lives *across* faces, not within one |
| R10 | b. near field and far field from one ray | **done** — D329–D331. R10a on its own showed nothing in a room and the claim that it did is withdrawn: an unbounded ray indoors always hits something, so sky visibility saturates at nought on every surface and the interior shifts by a constant. The ray's **first hit distance** through a metre falloff is what carries shape, and the enclosed near field spreads across the whole range where the far field had one bucket. 3.430 → **3.455 ms** enclosed: one ray, two answers. **Open:** the flat-wall self-occlusion gate is not cleanly verified |
| R10 | a. one ray into the hemisphere | **done, but not sufficient alone** — D325–D328. The ambient term had no visibility in it at all; it has one now, measured on the face by the pass that already traces. Enclosed mean sky visibility **1.00 → 0.019**, outdoor keeps 656 pixels at exactly 1.00. Card-only storage in `src/gpu/face_light.*`, which starts paying down R3d's debt rather than adding to it. Costs +22% enclosed, +41% close against a same-commit control, and the plan's prediction that enclosed would be the *cheapest* case was wrong — the cost is ray count, not ray length, because a face pointing away from the sun used to trace nothing. R10b and R10c still to come |
| R10 | ambient occlusion, per face and under it | **planned, not started.** The composite applies an ambient term with no occlusion in it at all, so the interior is lit as though it stood in the open. Same integral as the sun over a different domain; sub-voxel from a Legendre fit over the face that the existing jitter already pays for; converges once and then costs nothing. §8 R10 |
| R9 | i. the mechanism, second half | **done for the static case** — D341–D343. A shadow ray now reports the one cell that STOPPED it, which is the narrowing D292 always needed. Faces shadowed by a cell the pool has not built: **18,820 → 0** on a static camera, bricks 13,651 → 41,814, at +4% outdoor and +8% mid. **Still open:** after a 36M-voxel deletion the same count sits at 51,326 and does not converge, with feedback at 124,621 of 131,072 — requests made, nothing built, which is the D133 phantom signature. And speckle rose sharply (D342), which needs R5 |
| R9 | i. the geometry a shadow ray needs | **half done: the leak is stopped, the mechanism is not built.** A sealed room filled with sunlight as the pool shed — 0.0000 at frame 500, 0.0458 at 700, 0.0596 at 900, static camera, no edit — because a cold root cleared its node and entry and so read as EMPTY rather than WANTED. A cold root now sheds its subtree and stands as a shell: frame 900 goes 1,163 fully lit faces → **0**, mean 0.02 → **0.0000**, four of eight roots → **eight**, holding to frame 5,000, and the 42-run grid moves **+0.46%** with speckle identical in every cell against a same-commit control (D324). Still open: 9 faces read lit at frame 500 at full residency for an unrelated reason, and residency still does not count what a shadow ray reads. D322, D323, D324; §8 R9i |
| R9 | a. secondary faces are requested by the rays that need them | **done** — D526–D532. The ambient far ray names the one face it LANDED on, tagged `kFeedbackSecondary`, throttled by a phase on the slot and a period in frames. It builds nothing and streams nothing, so R9h's rule holds by construction and is measured: node requests **18,828,939 against 18,830,058** over a settled run. The picture does not move at either camera, because **nothing reads the set yet** — `may_cast` gates a face on a pixel having read it, so an off-screen face casts no rays and holds no light. The consumer is bounce, and it is the next change |
| R9 | b. a budget per bounce | **done, both halves** — D526, D527, D561–D568. The CLAIM half first: a cap of a quarter of the table, a DECLINE rather than a refusal past it, and a decline whenever the store is under pressure at all, so the class cannot push the table into refusing a face somebody is looking at. The part that was not obvious there: the SUN's ray budget was divided by the watermark, so the new class diluted the refresh rate of every face on screen and the faces pass **got cheaper — 1.16 ms against 0.96 — while convergence fell from 84 sun samples a face to 72**. A regression in an improvement's clothes. Divided by the on-screen population now: 85 both arms. **The RAY half was never spent and the class therefore held nothing** — 229,413 records at nought samples, read straight back out by the rays that asked for them (D561). It has its own share of `kFacesPerFrame` now, its own stride out of its own population, and its own card-owned stamp (`face_gathered`) so the two are counted apart rather than merely told apart |
| R9 | e. an instrument for the set itself | **done** — D529–D531. `the set on the card` reports both classes and their samples per face, `the off-screen set` reports what was offered, claimed, declined and promoted, and the audit that prints them no longer refuses to run when an upload is pending — which is always, while moving, and is the case that costs. Two things came out of it that were nobody's plan: the card's own provisional stand-ins are counted for the first time (**8,254 a frame**, each taking a fresh unbounded ray and lamp burst every frame), and the card is **up to 434,838 records behind the store** while flying, which is what the face pass's moving cost is actually a function of |
| R9 | bounce — the fourth term, off the pixel and onto the face | **done** — D533–D538. The ambient far ray already went out unbounded and cosine-weighted about the normal; it now returns what it FOUND, which is the sky where it escaped and the landed-on face's own outgoing radiance where it did not. `kIndirectFloor` — the constant that lit every interior in this game — is **deleted**, along with `kGroundBounce`, which was added to every surface in the world unconditionally: there is no minimum light anywhere now, and `clips/sealed_dark.clip` with `tools\darkroom.ps1` is the gate that says so (D541–D543). **+7% of the light pass and +5% of the frame** at the enclosed camera over three interleaved rounds; flying, the arms overlap. The picture moves by **16.998 of 255 over three quarters of the frame** and is **quieter**: speckle 17.62 with 9 fireflies against 21.22 with 81. It is multi-bounce for nothing, because a face's own bounce is part of what it gives off. Three traps came with it and all three are the same shape — a layout constant declared twice (D534), a per-face threshold where a blend belongs (D535), and a host struct that is not the shape of the push block it fills (D537) |
| R9 | f. coarse faces outlive their children, and a gathering ray may read them | **done, in the half that needed no fold** — D554–D560. The coarse pyramid is what everything else is rebuilt from and it was the FIRST thing the store threw away: a stand-in is stamped only when a fine face under it is new, so a settled camera never touches one again and after `cold_frames` it is evicted with every child still live. Measured on the close camera, the control arm holds **0 stand-ins of 711,000 faces and nothing at all above level 1**, having evicted 21,796; with the rule it holds **21,794 and gives up none**. A gathering ray that finds no light now walks up to the first ancestor with an answer, which recovers **30.7% of what found nothing** settled and 21.1% flying. The converged picture is brighter where the store forgets most and **quieter everywhere**: close mean pixel 133.5 → 140.0 with speckle 45.5 → 38.5, enclosed 126.4 → 127.6 with fireflies 36 → 9, outdoor unmoved at 0.214 of 255. Three frames after walking back into a room, the card's own provisional stand-ins — the most expensive face there is — go **3,137 → 99**, speckle 34.35 → 19.58. Gates: `darkroom.ps1` BLACK clear and with fog, the 42-run grid **+0.17%** with speckle −6.9%, flying arms overlapping, 519 tests. **What is NOT done is the FOLD**: a coarse face still measures itself with its own rays rather than averaging its children, and R9f's other clause — a ray that reaches an *unbuilt* region getting light — is blocked on the marcher, which does not name a face for an ignorance stop (D558) |
| R9 | the off-screen set | **half, and the off-screen set now carries light rather than merely existing.** R9a, R9b, R9e and R9f are in; R9c (the halo) and R9g–R9h are not. What the bounce integrates and finds black has gone from a third to **just under a quarter**: rays landing on a lit face 18.7% → 29.9%, rays landing on a face in the store with nothing measured 12.4% → **1.7%**, on the close camera settled (D561). The enclosed room's mean pixel goes **127.5 → 150.2** with fireflies 9 → 0. **What is left is entirely the other bucket — 21.9% landing on a surface with no face at all — and that is R9c's and R9g's.** Prerequisite for R4c/R4d being worth measuring |
| R9 | light from what is not loaded | **started at R9f; R9g–R9h planned, not started.** Light outlives its children now, so eviction has stopped being a lighting decision for anything the camera has already seen. What remains is the fold itself, the emitter list persisting per region and loading with the index rather than the voxels, and the analytic fallback past the last node |
| R4–R8 | | not started |
| **the large edit** | **an edit announced a volume to a tree that is not stored by volume** | **cause found and fixed — D515, D516.** The same shape as the paste one level down. `announce_world_change` named every brick in the edited box and the pool walked up from each; on a 36-million-voxel delete that is **1,573,269 bricks announced** to produce **13,325 refolds and no rebuilt leaves**, at **718 ms in one frame** — gather 457, descend 257, fold **4**. The pool holds the tree, so it now takes the **box** and descends from its own roots, pruning children that miss it, post-order so the fold ordering falls out instead of being sorted for. **718 → 7 ms**, and the edit frame is no longer the worst frame in the run (node-pool CPU worst 890.7 on the edit frame → 26.2 at startup). A one-voxel chisel is unchanged. `NodePool::stale_masks` is the audit that had to exist first, because a child mask is invisible to both the GPU mirror and `stale_leaves`, and both ways of getting it wrong are silent. **What is left in that frame is the undo capture: 194 ms into 538,169 inverse ops**, which is now the largest part of a large edit |
| **the paste, which is what a player feels** | **the region paste blocked the main thread for seconds, and it was not the paste** | **cause found and fixed — D511–D514.** Handover §5 has opened with *slice the paste across frames* since it was written, against measured stalls of 1.4 to 14.1 s. Splitting the one timing figure into its three parts said the replay and the announcement are **0 ms** and all of it is `paste_clip` — and then said something the plan had no room for: paste time does not track the paste. The same 991-brick region went in **146 ms and in 7,076**. It tracked the **sample running beside it**, because `parallel_for` queues a take-LOOP rather than a slice, so the background sampler owned every worker of the pool the paste was also given, and `wait()` handed the main thread the sampler's jobs. Foreground and background now have separate pools: worst paste **7,282 → 92 ms**, the twelve pastes of a cold load **34,697 → 719 ms**, frames drawn before settle **453 → 5,439**, sample +1–3%, and the same content hash `1f4710eee4ee2585` in both arms. `--no-paste-pool` is the control arm and `JobSystem::submitter_collisions()` is the instrument that makes the next one loud. **Slicing is not done and is no longer first**: what is left is 31–92 ms, and the premise it was sized against has moved by 79× |
| **the light while moving** | **the card's copy of the face table ran hundreds of thousands of records behind the store, and the light was not late — it was not being computed** | **cause found and fixed — D544–D546.** The face pass shades what the CARD holds, and an upload that ran out of staging cleared **nothing** and restaged the whole dirty set next frame, so it ran out in the same place for ever: measured flying at 1440p, **434,838 records behind** with the upload exhausted on 165 frames of 400. A partial upload marks clean exactly the runs it staged now (`DirtySet::clear_range`, six cases in `tests/test_dirty_set.cpp`), and the card is **0 records ahead against 80,211** with **1 exhausted frame against 253**. **What it uncovered is worth more than the fix**: the backlog was not making the pass shade too much, it was making it shade almost nothing — `seen on the card: 0 of 721,911` in the control arm, because the card's bucket table was too far behind for a pixel to find its own face, so `may_cast` was false everywhere and the frame was drawn from **8,255 throwaway provisional stand-ins against 723**, with **0 read-reports reaching the host against 22,702**. The faces pass therefore reads **6.8 ms against 2.0** flying and none of it is new work; the picture goes from hard-edged black and white blocks on the balustrade and cornice to lit stone, **44.90 of 255 over 2.76 M pixels of 3.69 M**, speckle **23.86 with 2,720 fireflies → 19.92 with 944**. That is D502's reported picture through a **third** distinct cause, so read `the card is N records ahead of the store` and `seen on the card` before theorising about the store. `--whole-set-retry` is the control arm. **What is left is a budget question and it is R5's**: the pass is over its 4.40 ms flying figure honestly for the first time |
| **the cold load, measured** | **what a world with no cache actually costs, and what the one lever that already exists buys** | **not a fault — a measurement.** Reported as the new streaming not being in use, and **withdrawn by the reporter once they were running the current build**: it is using it and it is working. What is below is kept because it is the only measurement of this case anybody has taken, and because two of the numbers in it were wrong the first time they were quoted. Reported as *it should load in half a second cold start no cache ... resolution based ... without chunks*. Facility, no cache, warm shaders, time to ready against `--clip-coarse`: **4 → 4,049 ms** (today's default), **8 → 1,784**, **16 → 1,369**, **32 → 808**, **64 → 2,059** (worse: scaling 64× on paste fills 1.36 **billion** voxels). At 32 the sampling is **130 ms** and the remaining 655 ms is paste 232 + upload 232 + pipelines 191, none of which is the field. **One correction to an earlier figure in this row: the first measurement here said 15,197 ms, and about ten seconds of that was the driver compiling shaders on a cold cache, not world building.** A number that includes somebody else's compiler is not a number about this program. Two things follow. The cheap one: the coarse rung is a lever that already exists and takes a cold uncached load to **0.81 s**, at the cost of a first frame sampled at metre 1 and scaled 32× — playable, recognisably the building, visibly blocky until the ladder catches up. The real one is unchanged: half a second with a *sharp* first frame means nothing is sampled up front at all, which is R8c (`forge/field.cpp` already answers at any resolution) with R1e removing the addressing that keeps a chunk world necessary. **The node pool being the marcher is not the same as the new streaming being in use, and this row exists because that was confused three times in one session** |

#### What a player was actually waiting for, which none of the above was

The grid says this marcher is faster than the one it replaces on every camera. The person playing it
reported lag, slow loading, and doubted it would ever be as good. Both were true, and the overlay
settled it in one line: **GPU 0.92 ms, frame 247.51 ms, 99th percentile 2,234 ms.**

Two things came out of looking at that instead of at the grid.

**The pool's own upload was the largest thing in the frame while moving** (D235). It copied every
array's whole used prefix whenever anything changed — free once the tree is converged and quiet,
which is precisely the state a fixed camera measures, and 10 MB a frame while walking. The `nodes`
pass measured 2.725 ms mean and 8.915 ms worst against a 0.80 ms budget. Sending only what changed
takes it to 0.028 and 0.257.

**And the hitching is not the renderer at all** (D239). The scene sharpens region by region; the
sampling is threaded and the paste is not, so the main thread blocks for **twelve to fourteen seconds
at a time**. That is the 6,282 ms worst frame. Both marchers suffer it identically, it predates the
rewrite, and D74 already names the fix — slice the paste across frames — while assigning it to
Stage 16. It is now the largest single thing standing between this engine and being judged fairly,
and it is not in this plan.

**Half of that is now paid once rather than every launch** (D241–D246). The sharpening was thrown
away at the end of every run, because the cache was written only when the last region landed and the
last region never lands — a box behind a wall is skipped for as long as the camera stands there, so
the facility settles at 14 of 18 and stops. The cache is now written at that fixed point, carrying
the list of which boxes are sharp, and a later run standing somewhere else carries on from it. The
default camera goes from **133.3 s to 6.6 s** on its second run; two runs from different cameras
finish the building, and every run after that loads a complete world in five to seven seconds.

It does not touch the stall itself — the first run still pastes for seventeen seconds at a time, and
slicing the paste across frames is still the fix and still outstanding. What it removes is having to
pay that first run again on every launch and on every one of R0d's forty-two grid runs.

**Two things follow that anyone measuring has to know.** Runs are now compared against a world that
converges across runs rather than being rebuilt per run, so the `scene:` line carries the world's
**content hash** (D243) and two figures are comparable when the hashes match. And the picture a run
draws depends on whether it watched the world sharpen: cold and warm runs of one binary, on a world
proven identical by that hash, differ on 87,357 pixels, because the node pool evicts nothing and the
cold run holds nodes built from geometry that has since been replaced (D244). **Figures taken before
this change are not comparable with figures taken after it.**

#### And what a player is waiting for now, which §6 above was measuring on the wrong half

§6 claims lighting stops scaling with resolution and shows the arithmetic for it. The claim is about
the *settled* store, and until D410 there was no measurement of any other state — `tools/baseline.ps1`
starts its window at refinement's fixed point, so every figure in this plan is taken with every face
converged and nothing casting a ray.

`tools/_flybench.ps1` measures the other state. Close camera, `--fly 0,0,3,15`, 2560×1440, quality 7,
200 measured frames: the faces pass is **11.75 ms mean against its 4.40 ms budget and against 1.11 ms
standing still**, in a frame of 18.6 ms it therefore owns 63% of. About 6.3 M rays a frame, from
280,000 faces still bursting.

That does not overturn §6 — the number still does not grow with pixels the way a per-pixel tracer's
does — but it does say where the stage is unfinished. **The convergence transient is the cost now,
not the steady state**, and R5's denoise is not what pays it: the ranked fixes are traversal ones and
they are listed at the end of `13-decision-log.md`. D410–D412.

#### R1h — the enclosed room, and why the answer took longer than the fix

R1d left one camera where the pool lost, and the handover made settling it the gate before R1e,
because once the old marcher is deleted there is nothing left to compare against.

**The cause was one measurement away and nobody had taken it.** The visibility buffer has carried a
per-pixel step count since Stage 2. Read out on the enclosed camera it says the node pool takes
**9.12 steps a pixel against the chunk marcher's 31.27** — three and a half times fewer — while
costing half as much again. A marcher doing less work for more money is paying inside the step, and
the step is `node_locate`, which walked all eleven levels from the 512 m root every time because
only the root was cached. Caching two more ancestors, at levels 8 and 5, in named scalars fixes it.

**But the harness could not have told anyone whether it worked.** See D229–D232: the facility never
finishes sharpening from a fixed camera, its clip cache is therefore never written, and every run
was measured against however much world had been rebuilt by the time it reached its screenshot
frame — which is *fewer* voxels the faster the build under test is. Two runs of one binary differed
on 52,292 pixels. `--settle` starts the window at refinement's fixed point instead, every timing now
prints the scene it was taken against, and only then do two builds mean anything to each other.

Measured settled, 1280×800, quality 7, visibility pass only, scenes verified identical per view:

| view | no cache | cached | change |
|---|---|---|---|
| enclosed | 1.082 | **0.803** | −26% |
| close | 1.486 | **0.990** | −33% |
| far | 0.515 | **0.504** | −2% |
| distant | 0.569 | **0.555** | −2% |
| outdoor | 0.601 | 0.642 | +7% |
| mid | 0.221 | 0.242 | +10% |
| sky | 0.484 | 0.759 | *see below* |

It pays where a ray marches fine cells through geometry, which is where the cost is, and is flat to
slightly negative where a ray skips empty space and never consults it — the shape the mechanism
predicts, since a cached ancestor is only reachable once a descent has gone below level 8. One tier
instead of two was measured and is worse where it counts: enclosed 0.974 and close 1.405, against
0.803 and 0.990 for both.

**The sky row is not a regression, and the way that was established matters more than the row.** It
first read as +57%, which on the floor view would be serious. Run three times on one build against
one scene it gives **0.481, 0.763, 0.472 — a 51% spread**, so the two builds had simply landed in
different modes of a bimodal number. The bottom two rows of the table are single samples on views
with the same character and should be read as "no effect measured" rather than as small costs.

The bimodality is not mysterious and is not the shader's: a ray is clipped to
`residency_.resident_bounds`, the resident set is what a run converges to, and D233 records that it
does not converge to the same thing twice. So the empty-space views inherit the pool's own
irreproducibility, and they will keep doing so until R2 settles it.

#### Against the marcher it replaces, on a harness that can be trusted

Same conditions, same scenes, `--chunk-marcher` for the old one. This is the first version of this
comparison taken with `--settle`, and it supersedes R1d's table above rather than confirming it —
R1d measured two builds of different speed against worlds that had been rebuilt to different extents.

| view | chunk marcher | node pool | change |
|---|---|---|---|
| enclosed | 0.886 | **0.803** | −9% |
| outdoor | 1.602 | **0.642** | −60% |
| close | 1.882 | **0.990** | −47% |
| mid | 0.870 | **0.242** | −72% |
| far | 1.506 | **0.504** | −67% |
| distant | 1.105 | **0.555** | −50% |
| sky | 2.377 | **0.472–0.763** | −68% to −80% |

Faster on all seven, the enclosed room included, which is what R1d could not say. The margin there
is the smallest of the seven and that is expected: it is the view with the least empty space to
skip, so it is the one a descent helps least, and it took the ancestor cache to turn it from a loss
into a win at all. 424 tests, 18.0 M assertions, passing.

**What the game runs now.** The node pool marches by default. The chunk system has not left the
build and is not idle: `pathtrace.comp` still includes `world.glsl`, so chunk residency, the coarse
grids and the thumbnail tiers are all still maintained every frame, fed by the node marcher's own
reports translated back to chunk coordinates. That is a double cost, and it is the cost R1e removes
by porting the path tracer to `node.glsl` — which is why R1e is a sub-step of its own rather than a
deletion.

---

### R0 — The baseline and the instruments · S

Nothing changes visually. The point is that every later claim has a number behind it.

- **R0a — make the profiler stop lying.** A stack, slots claimed at open, means over a window with
  warm-up discarded. `src/core/pass_ledger.*`, `src/gpu/profiler.*`, `tests/test_pass_ledger.cpp`.
- **R0b — one measuring tool.** `tools/baseline.ps1` over a fixed grid; cameras in
  `tools/_grid.ps1` so no two tools measure different things; speckle and image-difference in
  `tools/_measure.ps1`.
- **R0c — count faces rather than assume.** Debug view 11 writes the face key as four exact bytes;
  `tools/facecount.ps1` counts the distinct ones. This tests R3's premise before R3 is built.
- **R0d — record the grid.** `tools\baseline.ps1 -Out documentation\baselines\r0-before-rewrite.csv`.
  *Gate: rerun it and every figure reproduces within 3%.*

### R1 — The node pool · XL

- **R1a — the structure, headless.** `src/world/node_pool.{hpp,cpp}`, `tests/test_node_pool.cpp`.
  Node record, entry hash, contiguous children, fold-from-children, leaf array, payload staging
  through `BlockPool`, LRU, `mirror_voxel` asserted against the world.
  *Check: the whole suite; `mirror_voxel` equals `world.get` over a scene with a slab, a one-voxel
  wall and an isolated voxel, including negative coordinates.*
- **R1b — the descent, in the shader.** `shaders/node.glsl` (buffers, entry lookup, ancestor
  stack, three-way `Found`, leaf march), `shaders/node_visibility.comp` writing the **existing**
  visibility format so `resolve.comp` is untouched.
  *Check: it compiles; the hash and the level constants match `node_pool.hpp` line for line.*
- **R1c — wire it up.** Upload `nodes/entries/leaves/occupancy/payload` in `gpu/world_buffers.*`;
  a second pipeline in `main.cpp`; `--node-pool` to choose the marcher; feed feedback entries back
  as `NodePool::request`. Keep the old path intact.
  *Check: the game runs on both paths and neither crashes.*
- **R1d — prove it.** Render every camera in the grid on both marchers and diff.
  *Gate: `Measure-ImageDiff` worst ≤ 2 on every view; reference camera ≤0.77 ms at 1440p; 3 km
  ≤0.545 ms; 700 m ≤1.05 ms — all with feedback-driven residency only and no push tier.*
- **R1e — delete the old.** `residency.*`, `thumb_cache.*`, `thumbnail.*`, `summary_tree.*`,
  `world.glsl`'s chunk walk, the coarse grids, the eight thumbnail tiers, and their tests.
  *Check: the suite still passes with those tests gone rather than disabled, and the grid does not
  move.*

#### R1d, measured across the grid

1280x800, quality 7, 300 frames, both marchers in one binary. Run-to-run noise is 0.073 mean.

| view | old ms | node ms | image diff, mean | pixels 8+ |
|---|---|---|---|---|
| enclosed | 0.699 | **1.108** | 0.006 | 0.01% |
| outdoor | 1.574 | **1.047** | 0.841 | 2.10% |
| close | 1.685 | **1.505** | 0.383 | 1.48% |
| mid | 0.870 | **0.677** | 0.465 | 1.13% |
| far | 1.501 | **0.510** | 0.315 | 0.73% |
| distant | 1.100 | **0.557** | 0.442 | 1.32% |
| sky | 2.381 | **0.769** | 0.014 | 0.02% |

Faster on six views of seven, and by three times where distance dominates - which is the case the
four coarse occupancy grids and the eight thumbnail tiers existed to serve. Memory is 4.8 MB
against 57.7. The one regression is the enclosed room at 1.108 against 0.699, and it is the view
with the least empty space to skip, so it is the view a descent helps least.

The pictures agree. Two views are at the noise floor; the rest differ by under one part in three
hundred, and that residue is the coarse-level colour rule rather than an error: the old marcher
takes the first occupied descendant's brick colour, the node pool takes the coverage-weighted fold
of the subtree, which is what `04-rendering.md` answer D11 asks for and what D149 and D152 settled.

#### The fault that cost the most, and how it hid

**The node pipeline's output images were never bound.** Its descriptor set was assembled where the
buffers are written, and the two storage images are bound somewhere else entirely - in
`create_render_target`, because images are the only descriptors that change on resize. The node
set was added to the first place and forgotten in the second.

So the pipeline ran, did every bit of its work, and stored the result into an unwritten
descriptor. The visibility image kept whatever was in it, and `resolve` drew that.

The signature, in hindsight, was unmistakable and I read it five times before recognising it:
**every change to the traversal moved the timing and none of them moved the image, by a single
part in a thousand.** Five separate edits - the ancestor stack, the skip condition, the ordered
dither, the level clamp, the coverage source - each produced bit-identical output. A shader whose
cost responds to what you write and whose output does not is a shader talking to nothing.

Two things would have caught it immediately and neither existed: a validation-layer run (the
descriptor is undefined rather than illegal, so this needs `--validation` and an eye on the
warnings), and the habit of asking *"is this pass's output actually connected"* before asking
*"is this pass's algorithm right"*. The GPU mirror check built in R1f could not see it either -
it audits the buffers the marcher reads, not the image it writes.

#### Getting the tree to fill in

Three faults, all one mistake: **the sampling rate did not follow the unit being reported.**

A chunk is eight metres, so neighbouring rays wanted the same one and reporting one pixel in
sixty-four missed nothing. A node at the level a pixel resolves is a *brick* — twenty-five
centimetres, under two pixels across at sixty metres — so one report in sixty-four asks for one
path in sixty-four and the other sixty-three are never built. The tree converged at 765 leaves
for a building needing hundreds of thousands and then went quiet, because the pixels that would
have asked for the rest were not allowed to speak.

- Report at the level the pixel **wants**, not the coarsest level that is missing. `refine` walks
  the whole path in one call, so the old rule threw away ten of the eleven levels it was going to
  build anyway and took eleven frames per path instead of one.
- One pixel in four rather than one in sixty-four. It costs nothing at rest: a report only
  happens on a miss, and misses go to zero as the tree fills in.
- Dilate to the six face neighbours on the CPU, which is what the chunk path already did and for
  the same reason — otherwise only the nodes some ray happened to land on are ever built, and the
  edges of what has streamed show notches.

765 leaves to 21,408, and the marcher got *faster* rather than slower: 0.951 ms against 0.963,
because a ray that finds a node stops, where a ray that finds a shell keeps stepping.

#### How the speed was found, because the route matters more than the number

Three hypotheses, two wrong, and the wrong ones were only cheap because each was falsifiable:

1. **The ancestor stack is in scratch memory.** A twelve-element array indexed by a run-time level
   does go to scratch, and that is a real cliff — but removing it changed 11.52 ms to 11.89 ms.
   Wrong. The simpler code was kept anyway.
2. **The empty-cell skip re-seeds the DDA when an ordinary step would do.** True, and worth
   fixing, and worth nothing: 12.09 ms.
3. **The rays never terminate.** Capping the step budget at 512, 128 and 32 gave 12.03, 3.31 and
   0.995 ms — almost exactly linear. A marcher whose cost is proportional to its step budget is a
   marcher that is not stopping, and the cause was in this repository's own decision log:
   `node_march` had no ray clip. D148 records that widening the clip on the old marcher measured
   **23.7 ms against 0.78**.

**And the first run of that experiment was invalid**, which is worth writing down. It reported
12.03, 12.03, 12.04 — perfectly flat, which read as "the budget is irrelevant" and pointed away
from the answer. PowerShell's `Set-Content -Encoding utf8` writes a byte-order mark, glslc
rejected the file, and every run used a stale `.spv`. The build output was piped to `Out-Null`,
so nothing said so. Two rules fall out and both are now in `tools/` practice: never discard build
output during a measurement, and write shader files with a writer that does not add a BOM.

### R2 — Pixel-driven residency · L

- **R2a — feedback drives everything.** Read back `(key, level)` and request it; drop the radius
  push tier entirely.
- **R2b — the sub-pixel rule.** A node smaller than a pixel is never requested, uploaded or
  stored. This is where resolution becomes a real memory lever.
- **R2c — proximity.** 20 m at full detail regardless of visibility, for collision, physics and
  editing (D199).
- **R2d — what to draw while waiting.** The gap R1b left: a wanted-not-built node currently skips
  like empty space, so a region draws as sky for a frame. Draw the parent instead — but never a
  level coarser than the pixel resolves, or a two-kilometre block containing ground a mile away
  draws as a blob a few metres away (D151).
  *Gate: resident bytes at half resolution within 30% of a quarter of the full-resolution figure;
  cold start converges from any camera in any scene with zero phantom reports; no deadlock.*

### R3 — The face pass · XL

- **R3a — split the frame.** visibility → face select → face shade → composite. Composite reads
  the face store and does no tracing.
- **R3b — the face store, written by one invocation per face.** `src/world/face_store.*`,
  `shaders/face_select.comp`, `shaders/shade_faces.comp`. Key is the node the marcher stopped on —
  one level, from the descent, used by both passes.
- **R3c — sun and lamps in the face pass. Done.** Shadow rays and next-event estimation move off the
  pixel entirely. The sun landed first (D290–D303); the lamps are D401–D409 and are the same shape:
  one invocation per voxel face, `kLampCandidates` fittings scored by what each would deliver
  unshadowed, one kept in proportion to that score, one shadow ray inside its cone, accumulated into
  the face's own record as irradiance. A face never loops over lights.

  Three things in it are worth carrying forward rather than rediscovering:

  - **The estimator is the reference tracer's**, expression for expression, because two renderers
    computing the same light differently light the room one way and aim the rays another. D204's
    rule about one constant in one place, applied to a formula.
  - **"Instantly" is the host's job.** A converged face stops reading its own record, so nothing it
    can measure will ever tell it a lamp went out. The emitter list has an identity
    (`light_list_hash`), a change bumps a version, and one frame of `light_reset` reopens the store;
    each face then compares the version its samples were taken under and decides for itself. A
    reopened face keeps its mean and drops its confidence to eight samples, so the picture moves on
    the next frame instead of exploding into noise — 73% of the change at edit+1, 97.5% by edit+15.
  - **A converged face must touch nothing, and that includes the load that finds out** — the idle
    flag lives in `photons` for exactly this reason and the first version of this block did not use
    it. Kept on that argument and **not** on a number: the 0.89 ms it first appeared to save was two
    builds compared at different convergence states, and re-measured by interleaving it is inside
    its own spread. D406, which is worth reading for how the wrong number was produced.

  *Still R3c's in name only: the sky, which R10 built first and better.*
- **R3e — a face is claimed on the card, in the frame a ray first lands on it. Done — D316–D318,
  and the fix-up pass in step 3 below turned out not to be needed.** Stage two of shadow latency;
  stage one was D312–D315 and took a hard 180° cut from five frames of a completely unshadowed room
  to two. Stage two takes it to **nought, from the first frame**, at no measurable cost.

  What the design below got wrong, which is the useful part: it assumed the pixel that loses the
  claim race has to be served by a second pass, because it cannot read what the winner wrote. It
  does not have to read anything. If the slot IS the bucket, `atomicCompSwap` *hands the loser the
  winner's slot as its return value* — so there is no allocator, no append list, no indirect
  dispatch and no second pass. Step 3 exists only as a record of the more complicated thing that
  was not built. **The two that remain are the feedback round trip and nothing else**: a
  face is reported by the visibility pass, the CPU reads that report two frames later because
  `kFramesInFlight` is 2 and that is when the frame it was written in retires, and claims it then.
  No arrangement of CPU code shortens it. Reading the buffer a frame earlier is a fence wait.

  The design, in the order the pieces have to exist:

  1. **A provisional table the CPU never touches.** Records in the *tail* of the existing faces
     buffer, above `max_faces`, so the composite reads them with no tag bit and no second array and
     `resolve.comp` does not change at all. Buckets in their own small SSBO, because the main
     bucket array is uploaded from the CPU and would overwrite anything the card wrote — which is
     D295, exactly, and the reason this table is separate rather than shared. A ring allocator with
     a generation stamp: an entry older than a few frames is free, so nothing is ever deleted and
     the CPU's own claim two frames later simply takes over.
  2. **The claim itself**, in the visibility pass where the key already is: probe the main table,
     and on a miss `atomicCompSwap` the provisional bucket from empty to a slot taken from the ring.
     Racing pixels of the same face compute the same bucket, so the loser finds the winner's key
     rather than allocating a second slot.
  3. **The pixels that lost the race still have no slot this frame**, because nothing orders two
     workgroups. So the misses go in a compact append list — pixel and node slot, two words — and a
     small pass after the visibility pass, **dispatched indirectly over that list**, redoes the
     lookup and writes `out_face` for those pixels. Its cost is proportional to the number of
     pixels with no face, which is zero at rest and one screen for one frame after a cut. That is
     the whole reason it is a list and not a full-screen pass.
  4. **`shade_faces` covers the ring** as well as the store, so a face claimed in the visibility
     pass is shaded later in the *same* command buffer and read by the composite after it — the
     pass order already allows this, which is what makes zero frames reachable rather than one.

  *Gate: `--cut` at the enclosed camera, cut+1 under 1% of surface on the fallback — against 100%
  today. Settled cost unchanged on the grid, still and turning; the provisional table adds no CPU
  work and no upload; two settled frames stay bit-identical (D194).*

  **What this must not become**: a per-pixel shadow ray as the fallback. It is the obvious
  alternative, it is one line, and it is the per-pixel escape hatch §1 exists to forbid — 1M rays on
  the reveal frame, a spike measured in milliseconds, on the exact frame the frame time is already
  worst. The whole point of a face store is that the answer is looked up.

- **R3d — delete the per-pixel light path.** Bounce, shadow, NEE coin, lobe choice, specular
  stride, glass segment loop, `pt_normals.glsl`, the rgba32f accumulator and its reset.
  *Gate: enclosed room within 30% of outdoor at the same resolution; face-pass time within 10%
  between 1080p and 4K; no per-pixel random numbers remain.*

### R4 — Directional faces · L

- **R4a — the variable payload**, allocated only where it is read; matte stone allocates none.
- **R4b — bins from measured pixel coverage and roughness** (D186). No roughness threshold.
- **R4c — outgoing bins**: reflection, read by the composite.
- **R4d — incoming bins**: refraction and translucency, one face-pass ray through the medium,
  Beer-Lambert over the exact voxel path, dispersion by hero wavelength per face sample.
  *Gate: a mirror wall shows a recognisable image with no per-pixel ray; glass and water read as
  glass and water; frame time within 15% of R3 on a scene with no reflective material in it.*

### R9 — The off-screen set, so light is world space and not screen space · L

**Where this comes from.** The one rule says every ray starts on a voxel face, and R3 makes the
face store the place light lives. But a face only gets *into* the store when a primary ray lands on
it — so the store holds exactly what the camera can see, and the light in it is a screen-space set
wearing world-space clothes. Everything the eye can see behaves. Everything the eye cannot see, but
which the eye's surfaces depend on, does not exist.

**What is already world space, and must not be re-solved here.** Occlusion is. A shadow ray marches
the node pool, which is the world, so a caster the player cannot see still casts (D302 made a shell
opaque to that ray, which is the same principle). This stage is not about the geometry a secondary
ray *crosses*. It is about the faces a secondary ray *lands on*, which is where light has to be
read from rather than merely blocked by.

**What breaks without it, concretely.**

| | today | why |
|---|---|---|
| a mirror facing a wall behind the camera | the wall reflects as nothing | the wall has no face, so R4c has no bins to read |
| a red wall bouncing light onto a white one | no bounce, whatever R3c does | the red wall is off screen; nothing carries its outgoing radiance |
| glass with the room behind it | refraction shows unlit geometry | R4d's ray lands on faces with no light |
| a lamp in the next room through a doorway | the doorway is dark | the lit floor inside has no face to be lit |
| turning around | the world relights over a few frames | every face just claimed is at zero samples |

The last row is the honest cost of the current design and the one a player feels: light has to be
rediscovered every time the camera moves, because the set is defined by the camera.

**The rule this stage adds.**

> The face set is the transitive closure of what the screen sees, one bounce at a time, bounded by
> a budget per bounce. A face is claimed because some face already in the set needs to read it, not
> only because a pixel landed on it.

**R9a — secondary faces are requested by the rays that need them. Done — D526–D532.** The face pass already marches
rays that stop on faces: the shadow ray stops on a caster, and R4's rays stop on whatever is
reflected or refracted. Each of those hits knows its face key by the same arithmetic
`node_face_hit` already uses. It reports it down the same feedback channel, tagged with a bounce
depth. This is deliberately the *opposite* of the rule the shadow ray follows today — "a shadow ray
must not drag streaming towards whatever it happens to cross" — and the distinction is that a ray
now names what it **landed on**, which is one face, rather than everything it passed through.

**R9b — a budget per bounce, and the reason it is per bounce. Done, both halves — D526, D527,
D561–D568.** One shared budget lets the off-screen set starve the on-screen one, and the on-screen
set is what the player is looking at. Three classes: primary (a pixel landed on it), secondary (one
bounce from a primary), tertiary and beyond (folded into one class with the smallest share). Each has
its own share of the face-shading budget `kFacesPerFrame` and its own share of the claim rate. A class
that overruns degrades its own refresh rate and nothing else's.

The CLAIM rate landed first. It is a cap of a quarter of the table, and a claim past it is **declined
rather than refused** — a distinction that has to exist, because a refusal is a visible surface with
no light of its own (D502) and a decline is one gathering ray reading a coarse stand-in. The store
also declines the whole class while it is under pressure at all, so this can never be what pushes the
table into refusing a face somebody is looking at. Building it walked into the fault that keeps
recurring: the SUN's budget was divided by the WATERMARK, so the moment R9a claimed anything every
face on screen was refreshed less often and the faces pass got *cheaper* while convergence fell from
84 sun samples a face to 72 (D527).

**The RAY share is the half that had never been spent, and without it the class was a claim with no
consequence.** `may_cast` is `node_face_recently_seen`, a stamp written by the visibility pass, which
runs only on pixels — so an off-screen face was refused a ray for its whole life. Measured on the
close camera settled: **229,413 off-screen faces at nought sun samples and nought finished ambient
terms**, and 12.4% of every gathering ray in the frame reading one of those empty records straight
back out. The shape of the fix is the same shape as everything else here:

- **a face is worth a ray when something is INTEGRATING it**, and for an off-screen face that
  something is the gathering ray that landed on it. `face_gathered` is a card-owned word a slot,
  stamped in `bounce_radiance` — `face_seen` for the pixel, `node_seen` for the light's geometry, and
  now this for the light's *faces*;
- **it has to be a second array, not a second meaning for the first.** `face_seen`'s population is
  what the sun's budget is divided by, so a shared stamp is D527 and D557 a fourth time;
- **the class's stride comes out of its own population and its own share** (`secondary_light_stride`,
  `--secondary-light-share N`, default 8; `--no-secondary-light` is the control arm);
- **and the sun's stride must not compose with it**, or the class is visited and then given no sun
  ray, for ever, with every audit line reading as though it worked (D564).

**What it is worth, and where** (D561): rays landing on a lit face **18.7% → 29.9%**, rays landing on
a face with nothing measured **12.4% → 1.7%**, the enclosed room's mean pixel **127.5 → 150.2** with
speckle 16.1 → 12.8 and fireflies 9 → 0, the close camera 139.8 → 143.1 with fireflies 108 → 27, and
the outdoor camera unmoved at 0.86 of 255 — because outdoors a gathering ray reaches sky and nothing
was missing. Both `darkroom.ps1` arms still BLACK at 0 of 255.

**Two things to know before touching the budget.** Its cost is a **tail and not a rate** — 35 faces
shaded cost 0.85 ms and the next 2,335 cost 0.94, so a smaller budget gives up most of the win for a
tenth of the cost, and three explanations for that were priced and rejected (D566). And the **moving
case pays most of the cost for a tenth of the win**: flying, the faces pass goes 6.69 → 7.91 ms on a
pass already over its 4.40 ms budget, for one point of what standing still gains eleven (D568).

**R9c — the halo, which is the "reprojection" half. Not started, and R9b's ray share had to land
first or it would have changed no pixel** — it claims *off-screen* faces, and until D561 an
off-screen face held nothing. **It is now the whole of what is left in that measurement**: with the
class lit, the bucket R9c addresses is unchanged at **21.9% of gathering rays landing on a surface
with no face in the store at all**, and it is the largest remaining source of black in the bounce.

Faces do not leave the store the moment they leave the screen: they leave when they go cold, and
`cold_frames` is already 600. That is most of what is wanted from reprojection and it exists. What is
missing is the *entry* side — a face just off the edge of the screen is never claimed until it comes
on, so a pan reveals unlit geometry that then lights over several frames. The primary pass therefore
claims over a frustum widened by a margin, at a coarser request lattice than the on-screen one:
geometry arrives already lit, and the cost is a fraction of a pass that is already sparse. Margin from
the camera's angular velocity, so standing still costs nothing.

One caution, from what R9b's ray share measured: a halo face is an off-screen face and inherits that
class's cost shape, which is a tail rather than a rate. Claiming a margin is cheap; *lighting* it is
priced by D566 and not by how many faces the margin holds.

**R9d — coarse light for a face that has none. Done, early — D308–D309.** A face that has just
entered any of these sets has zero samples, and the composite falls back to full sun on it, which
reads as a flash indoors and as no shadow behind things after the camera moves. This is R2d's rule
applied to light rather than to colour, and R5b already calls for parent seeding at claim time; R9
is what makes the parent reliably present.

Built ahead of the rest of R9, because it does not need the rest of R9: the face the camera can see
is already in the store, and the only thing missing was something for it to read while it waits.
**Three levels up rather than one**, which is the one thing this paragraph had wrong — what matters
is not how much coarser the stand-in is but how many faces share it, and 512:1 is what makes it
immune to the request lattice that the fine face waits on. Claimed CPU-side by shifting the fine
key, so it costs no feedback traffic, and only when the fine face is new, so it costs no repeat
probes. Measured: under 1% of surface falling back at frame 30 rather than frame 78, GPU unchanged,
settled picture bit-identical.

What is still R9's to do here is the other half — seeding a new face's *counters* from its stand-in
at claim time, so it starts at a plausible answer rather than at zero samples. That needs R9's sets
to decide which faces are worth seeding and is where R5b's parent seeding lands.

**R9e — a debug view for the set itself. The counting half is done — D529–D531.** Which class each
visible face belongs to, and how many faces each class holds. Without it "the reflection is dark" and
"the reflection is not in the set" are the same picture, which is the trap D296 was written about.

What exists is the counting, which is the half that could be built before anything reads the set: `the
set on the card` gives both classes with their samples per face, and `the off-screen set` gives what
light rays offered, what was claimed, what the cap declined and what a pixel later promoted. The
**view** waits for the consumer, because a screen-space view of an off-screen set has nothing to draw
until a face that arrived through it is being read.

Building it turned up two things that were nobody's plan and are worth more than the counters. The
audit printing all of this **refused to run at all while an upload was pending**, which a moving
camera always has — so the numbers were missing from the one case that costs (D529). And with it
running, the moving cost turned out to be a function of how many live records the CARD holds, which
is **up to 434,838 more than the store does** (D531). That is a fault of its own, it predates this
stage, and it is now the largest single thing measured in the moving case.

#### Light from places that are not loaded

R9a–R9e extend the set to geometry the camera cannot see. This half extends it to geometry the
*engine* does not have — which is a different problem, because there is nothing to march and
nothing to shade, and the answer must not be to go and fetch it.

**"Not loaded" is three different things here, and only the third is hard.**

| | what the engine holds | what a light ray can do |
|---|---|---|
| a **shell** — the world says occupied, children never built | the node, its folded colour, its per-direction coverage | read it; the data is already there |
| in `World` but not in the node pool | CPU chunk data, no GPU node | nothing today |
| not in `World` at all — never generated, or evicted | a region index entry at most | nothing today |

**The rule that makes all three cheap.**

> Light folds up the tree exactly as colour does, and eviction must never take light with it. A
> gathering ray reads the coarsest node that has an answer, and a node that has an answer keeps it
> after its children are gone.

That is the whole mechanism, and it needs no new structure: a face is already keyed by *(node,
direction)* at **any** level, so a coarse node's light is a face at a coarse level. The face store
holds it, the face pass shades it, the composite reads it. What has to be added is the fold and the
promise that it survives.

**R9f — coarse faces carry folded light, and outlive their children. The OUTLIVE half is done
(D554–D560); the FOLD is not, and the measurement below says why it was not needed first.**

This sub-step was written as one change and is two. *Outliving* is a rule in the store and a walk in
the gathering ray, and it is what a player feels; *folding* is an accuracy improvement on top of an
answer that already exists, because a coarse face is not an empty record waiting to be filled — it is
claimed as a stand-in, it is shaded by the same pass as everything else, and it measures itself with
its own rays at its own scale. R9d already measured that self-measurement as about a tenth too bright
and sharpening. So the fold buys accuracy on a number that is there, and outliving buys the number
existing at all when the camera comes back. Outliving was therefore first, and it was the whole of the
win: **the store held 0 coarse faces of 711,000 on a settled camera** before it, having evicted every
one it ever claimed.

The fold also carries a hazard the outlive half does not, which is why it wants its own change and its
own measurement: D191's *one invocation owns each face* is the property that removed the halving
compare-and-swap, the read-twice-take-the-minimum and the eight-probe eviction from this pass, and 512
children pushing into one parent record is exactly the arrangement it removed. The shape that keeps it
is a PULL — a coarse face reading the four child faces under it at the same direction, which is four
lookups on a record that is already being visited, one writer, no atomics.

What the fold was to be: when the face pass shades a face, it also folds its result into its parent
face at the same direction, coverage-weighted — the same fold `node_face_coverage` already performs
for colour. A 512 m node has six faces; the whole coarse pyramid over a scene is thousands of records,
not millions, so this is O(coarse nodes) and independent of both the screen and the world size. Two
consequences worth stating separately because they are what the stage is *for*:

- **A ray that reaches an unbuilt region gets light rather than nothing.** It stops at the coarsest
  node that exists — which for a shell is the shell itself — and reads a colour that was folded
  from that region's own children the last time they were resident.

  **Blocked, and on the marcher rather than on the fold** (D558). A ray stopped by a cell the pool
  has not built carries no face key at all: `node_face_hit` runs at the leaf hit and nowhere else, so
  `face_level` is `kNoFaceLevel` and there is nothing to look a face up with. The probe word that was
  to price this could not be filled for the same reason. Any attempt at this clause changes
  `node_march` first, and it inherits D302's whole argument about what a shell means to a ray.

- **Evicting geometry stops costing light. Done — D554.** The sentence this was written under said
  *"today the coarse ancestor survives eviction and its light does not"*, and the truth was worse:
  the coarse ancestor did not survive either. `FaceStore::last_read_` is stamped by a CLAIM, a
  stand-in is claimed only when a fine face under it is new, so a settled camera stamps it once and
  never again — it is the coldest record in the store by construction and the first thing evicted,
  with every child still live. Measured on the close camera: **0 stand-ins live of 711,000 faces**,
  21,796 evicted over the run, and nothing at all in the store above level 1. It is kept while there
  is room to keep it and spent with everything else under pressure, and walking away from a room and
  back now finds it lit — three frames after coming back, the card's own provisional stand-ins go
  **3,137 → 99** and the speckle 34.35 → 19.58.

**R9g — the emitter list stops being camera-centric and stops needing voxels.** `src/world/light_list.*`
already merges emissive voxels into fittings, and it is built from resident `World` chunks and then
**sorted nearest-first around the camera and capped** — so a lamp in a region that is not loaded
does not exist, and one just past the cap blinks out. Both are the same fault as the face set's:
the light is defined by where the camera is rather than by where the light is.

The fix is to persist the fittings, not to load the voxels. A region's emitters are a few dozen
`LightSource` records — position, radiance, size — and they are written into the world cache beside
the region index when the region is generated or sharpened. They load with the **index**, which is
already always resident, not with the region's voxel data, which is not. Kilobytes for a world,
against megabytes for one chunk of it.

Sampling stays constant-cost per face: one fitting per face per frame, chosen by estimated
contribution — radiance times solid angle over distance squared — and accumulated into the same two
counters D293 introduced. **A face never loops over lights**, however many there are, and a scene
with a thousand lamps costs a face exactly what a scene with one does. Occlusion for a distant
fitting is tested at coarse levels only, with a shell counting as opaque (D302), which is bounded
in steps and conservative in the right direction.

**R9h — the fallback, for where there is genuinely nothing.** Past the last node and the last
region record, the answer is the analytic sky and the coarsest folded colour on the path. It is
never black, never a stall, and never a request. That last clause is a rule, not a preference:

> **No light path may cause streaming.** A ray that gathers light reports nothing and asks for
> nothing.

D292 states the narrow version of this for shadow rays — a shadow ray must not drag residency
towards whatever it crosses. R9g and R9f widen it to every gathering ray, and R9a is the single
exception, deliberately: it reports the face a ray *landed on*, which is one face and one claim,
never the geometry along the way. Without that line, GI becomes a streaming amplifier — every
surface asks for the world behind every other surface — and the cost is unbounded in exactly the
way this rewrite exists to stop.

**R9i — geometry a shadow ray needs is not geometry the camera can see, and residency currently
only knows about the second.** The one measured fault in this stage rather than a predicted one, and
the biggest open fault in the renderer: **a sealed room fills with sunlight as the node pool sheds**,
on a static camera with nothing edited. Against the pool's own node count on the enclosed view —
frame 500, 442,968 nodes, mean sun visibility 0.0000; frame 660, 266,840 nodes, 0.0000; frame 700,
**6,972 nodes, 0.0458**; frame 900, **0.0596 and still climbing**. Every correct answer in that room
is nought (D302: 93,741 of 93,745 faces fully shadowed). Reproduced before any of the shadow-latency
work: 0.0266 at `f902a00`. D322, D323.

The shedding is R2 doing exactly what §3 tells it to — *if you cannot see it, it is not processed
and does not exist*. The roof over your head and the outer wall behind you are not on screen, so
they go. **They are also the only reason the room is dark.** So the rule in §3 is right for what a
pixel needs and incomplete for what a *ray* needs, and this sub-step is where that gets stated
properly rather than discovered again:

> Residency is driven by what the renderer READS, and a shadow ray reads geometry. What a pixel can
> see decides what is *drawn*; it cannot be the whole of what is *kept*.

Two candidate shapes, and they are not equivalent:

1. **An evicted subtree keeps reading as WANTED to an occlusion ray.** Cheap, and it fails safe:
   D302 already says a shell is opaque, and this is the same sentence applied to a node the pool has
   given up rather than never built. A room whose walls have been evicted goes *dark*, which is
   wrong in the harmless direction. It does not fix the light — it stops the leak.
2. **A shadow ray's occluders count as use.** Correct, and it is the one that costs: D292 forbids a
   shadow ray from dragging streaming, for the good reason that it would fetch the world along every
   ray. The narrow version — *a ray that was STOPPED by a node has used that node* — reports one node
   per ray rather than everything it crossed, which is the same shape as R9a's single exception and
   the same argument for why it is affordable.

They compose: (1) is the floor, (2) is the answer. Do (1) first, because a leak that brightens is
worse than a leak that darkens, and because it is a rule and not a mechanism.

*Gate: enclosed camera, static, no edit — mean sun visibility stays at 0.0000 from frame 500 to
frame 5,000 while the pool sheds to its resident minimum; the reveal case (`--cut`) still reads 0%
on the fallback at cut+1; the grid does not move.*

**(1) is done, and it was not a rule in the shader — it was a bug in eviction.** The shader already
said the right thing; the pool was not leaving it a shell to say it about. A cold root freed its
subtree *and* cleared its node, its entry and its `live_` record, so a whole 512 m block stopped
reading as WANTED and started reading as EMPTY, which occlusion treats as open sky. A cold root now
sheds only `children` and stands as a shell. **D324** has the reasoning, the entry-table probe-chain
fault it also repairs, and the full table; the gate reads:

| enclosed, static, no edit | frame 500 | frame 900 | frame 5,000 |
|---|---|---|---|
| mean sun visibility, before | 0.00 | **0.02** | — |
| mean sun visibility, after | 0.0002 | **0.0000** | **0.0000** |
| fully lit faces, before → after | 9 → 9 | **1,163 → 0** | — → **0** |
| roots at level 14, before → after | 8 → 8 | **4 → 8** | — → **8** |

Two of the three clauses are met outright: 0.0000 holds at 900 and at 5,000 while the pool sheds to
its resident minimum of 7,168 nodes and holds there with `built 0 evicted 0`, and the `--cut` reveal
reads 0% on the fallback at cut+1. The third — **the grid does not move** — is met: the whole 42-run
grid was measured on this build and again on a control built from the same commit with the change
stashed out, and the total across all 42 cells moved 1,672.5 ms → 1,680.3 ms, **+0.46%**. Speckle is
identical to two decimals in every one of the 42 cells, which is the stronger half of that result:
the pictures are the same pictures. Three cells moved more than 5% on a single pass and all three
were machine noise, shown by re-running them: realtime/sky/high 11.162 → 15.274 → **11.183** on a
repeat, pathtrace/close/deck 41.418 → 45.555 → **40.803**, and realtime/distant/high moved 6.6% the
*other* way. **The "from frame 500" wording is not met at four decimals**: the
frame-500 shot reads 0.0002 with 9 faces in full sun, at *full* residency, identically in the
control. That residual is not eviction and this change does not touch it. It is the next thing to
find. One qualification on the reveal clause, recorded because it reads better than it is: at cut+1
out of a fully shed pool there is no revealed surface yet — the shot is sky — so 0% is true and
uninformative. The room is back and pixel-identical to the settled shot by cut+30.

**(2) is still open** and is still the answer: a shadow ray's occluders counting as use is what makes
the room *right* rather than merely dark.

**Why this is the performant shape and not merely the correct one.** Every term above is bounded by
something that is not the size of the world: coarse faces by the pyramid, lamp sampling by one
fitting per face per frame, occlusion by the coarse levels, and all of it by R9b's per-class budget.
Nothing here scales with how much world exists, how much of it is loaded, or how many lamps are in
it. What it costs is **convergence**, paid in frames, which is the currency §6 already chose.

*Gate: a mirror wall reflects a surface that is behind the camera, with light on it, and the
reflection does not change when that surface comes into view; a 180° turn shows no relighting;
faces in the off-screen classes stay within their budgets on the enclosed camera; frame time
within 15% of R3 with the classes enabled and no reflective material in the scene.*

*Gate, for the unloaded half: a lamp 200 m away in a region whose voxels are not resident lights
what it should, and **the picture does not change when that region streams in**; walking out of a
lit room and back finds it lit, with no relight; a scene with a thousand fittings costs a face what
a scene with one costs; **streaming requests per frame do not move when GI is switched on**, which
is the measurement that proves the no-streaming rule rather than asserting it.*

**Sizing and order.** R9a and R9d are small and pay immediately: they are what stops a shadow
caster's own surfaces going dark and what removes the relighting flash on a turn, and they can land
straight after R3c. R9b is the machinery that keeps the rest affordable. R9c is independent and
cheap. R9e comes first in practice, as instruments always have. **R9f is the one to build before
R9g**: folded light on coarse faces is what a distant lamp's *effect* is read from, and it is also
what stops eviction being a lighting decision, which is a bug today rather than a feature tomorrow.
**R9 as a whole is a prerequisite for R4c and R4d being worth measuring** — a reflection of an
empty set is a black mirror, and no amount of bin resolution improves it.

**The risk, stated plainly.** This is the stage where the face count stops being bounded by the
screen. §6's whole argument is that faces grow sub-linearly with resolution because a voxel already
covers a pixel at 22.5 m; that argument says nothing about a set which now includes surfaces no
pixel is looking at. The budget per bounce is what holds it, and the thing to watch is not frame
time — the budget fixes that by construction — but **convergence**: a store spread too thin gives
every face too few samples and the picture gets noisier everywhere rather than slower anywhere.
The measurement that decides it is samples-per-face per class, and it should be reported from the
first commit of R9a.

**The second risk, which belongs to the unloaded half.** Folded light is a lie that gets more
convincing as it gets coarser: a 512 m node's six faces stand for a square kilometre of surface,
and any lighting read from them is an average over things that do not look alike. That is
acceptable for a contribution arriving from far away and unacceptable for one arriving from the
next room, so the fold must never be read at a level coarser than the *distance* justifies — the
same footprint rule the primary ray already uses, applied to the gathering ray. If it is read too
coarsely the failure is not noise but **flatness**: interiors lit by a plausible average that does
not respond to what is actually in them, and no counter moves. The debug view in R9e must therefore
show which *level* a face's light came from, not only which class.

### R10 — Ambient occlusion, per face and under it · L

**Where this comes from.** `resolve.comp` already applies an ambient term to every surface in the
frame, and there is nothing anywhere that stops it:

```glsl
float sees_sky = kSkyAmbient * (0.5 + 0.5 * normal.y) + kGroundBounce;
```

*That line is history: R10 replaced the orientation guess with measured visibility, and D541 deleted
both `kGroundBounce` and the `kIndirectFloor` that stood beside it. It is quoted because it is what
this stage was written against.*

That is how much of the sky a surface sees, decided from which way it points and from nothing else.
A wall at the back of a corridor, a floor under a ceiling, the inside of the facility — every one of
them receives the whole dome. The building is lit as though it were a cut-out standing in the open,
which is exactly what it looks like, and it is why the interior reads as a model of a room rather
than as a room even now that the sun casts real shadows.

So ambient occlusion is not an effect being added here. **It is the missing visibility on a term
that is already being applied**, and the reason it has never been there is that visibility used to
mean a per-pixel gather, which this renderer does not do and will not do.

**What it is, exactly, and why it is one machine and not two.** The quantity is the cosine-weighted
fraction of the hemisphere above a face that reaches somewhere else:

```
V(x) = (1/π) ∫_Ω v(x, ω) cos θ dω
```

The sun term R3c already computes is that integral over a domain of one direction with a disc
around it. Ambient occlusion is the same integral over the whole hemisphere. Same face, same one
writer, same two counts, same round robin, same accumulation that made the penumbra resolve over
samples instead of over pixels — **a different set of directions and nothing else**. Anything in
this stage that needs its own pass, its own budget or its own buffer is a sign of having got it
wrong.

**The rule this stage adds.**

> Ambient occlusion is the visibility factor of the ambient integral, and there is exactly one of
> it. It is measured on the face by the rays the face pass already traces, it is stored where the
> face's other light is stored, and nothing in the composite multiplies by it twice.

That last clause is not pedantry. AO applied *as well as* a real sky-visibility term — which is
what R3c and R9 are going to produce — double-darkens every crease in the building, and the failure
looks like a taste problem rather than like a bug. One quantity, one consumer.

**What this stage is not, stated so it is never argued about again.**

- **Not screen space.** SSAO is a second gather per pixel, so it scales with resolution — the one
  thing this whole rewrite exists to stop — and it is view-dependent, so it swims when the camera
  moves and vanishes at screen edges. It also cannot see the occluder standing behind you, which is
  precisely the occluder that darkens the wall you are looking at. It fails the one rule in §1 on
  its first line.
- **Not baked.** A lightmap is an answer to a world that does not change, and this world is editable
  at 3 cm with a chisel.
- **Not a normal-map trick.** D195 already deleted derived smooth normals: detail comes from real
  smaller voxels. AO here is measured against the geometry that is actually there.

#### The three things it has to deliver

1. **Per face.** One occlusion answer per voxel face, from real rays against real voxels — so a
   corner is dark because a corner *is* dark, not because a heuristic said corners are dark.
2. **Under the face.** Occlusion that varies *within* a single voxel face, continuously. At level 0
   a face is 3.125 cm; a pixel at arm's length covers a fraction of one. Without this, AO is
   flat-shaded per voxel and the picture gets *blockier* the closer you stand — the exact failure
   D298 fixed for shadows, arriving again through a different door.
3. **Aware of corners, planes, curvature and shape.** Not as four heuristics. As one consequence:
   the thing being measured is the real visibility field, and the thing being stored is a low-order
   *fit* of that field over the face, whose terms are literally its value, its gradient and its
   curvature.

#### R10a — one ray into the hemisphere, and the constant comes out of the composite

Cosine-weighted direction about the face normal, from the same per-slot hash `shade_faces.comp`
already uses for the sun disc; one ray per due face per frame; accumulated with `face_accumulate`'s
two counts, for the reason D293 recorded — a running mean in eight bits cannot converge. The
composite reads it in place of `sees_sky`.

**Where it is stored, and this is the sub-step's real decision.** Not in `GpuFace`. That record has
two owners, and D295 already cost a session to the fact: the uploader sent the CPU's zeroed bytes
over what the card had written. AO is written by the card, read by the card, and never looked at by
the CPU at all — so it goes in a **card-only array, one entry per face slot, allocated with the
store and never uploaded, never mirrored, never in a dirty range**. `src/gpu/face_light.*`. This
also starts paying down R3d's standing debt (*split `GpuFace` so the CPU's half and the card's half
are never in one copy*) rather than adding to it: the sun's counters move there when R3d lands.

*Files: `shaders/shade_faces.comp`, `shaders/resolve.comp`, `src/gpu/face_light.{hpp,cpp}`,
`src/world/face_store.*` (slot lifetime only).*
*Gate: against a brute-force reference — see the gate at the end of the stage. The enclosed room's
mean sky visibility must fall from 1.00 to the reference's answer, and outdoor faces pointing at
open sky must stay at 1.00, because a change that darkens everything is indistinguishable from a
change that darkens nothing but the exposure.*

#### R10b — the near field and the far field are two answers from one ray

They are different quantities and they must not be averaged into one:

- **far field — sky visibility.** Unbounded. Multiplies sky radiance. Physical, and the thing the
  ambient term is actually missing.
- **near field — contact.** The same ray's *first hit distance*, put through a falloff over about a
  metre. This is the crease darkening that reads as shape, and it is what carries the sub-voxel
  detail in R10c because it is the part that varies quickly across a face.

One ray answers both: a hit at 0.3 m says "not sky" and "contact", a ray that escapes says "sky" and
"no contact". The falloff radius is **in metres and not in voxels**, so a coarse face at 200 m and a
level-0 face at arm's length darken over the same physical distance and a dolly-in shows no
transition.

**The cost inverts, which is worth noticing.** An AO ray dies at the first thing it meets, so the
enclosed room — the case that costs the most in every other pass, the one the whole plan is gated
on — is the *cheapest* case here: every ray terminates in a metre. Outdoors the rays escape, and
escaping is what the node pool's empty-child skip is best at. There is no camera where this is the
expensive term.

*Gate: the contact term alone, in the debug view, on a flat wall in the open, must be zero
everywhere — a falloff that darkens a plane against itself is the classic self-occlusion bug and it
is invisible once the two terms are multiplied together.*

#### R10c — the polynomial over the face, which is where "under the voxel" comes from

The face pass **already** picks a point on the face and throws its position away:

```glsl
const vec3 on_face = corner + vec3(size * 0.5) +
                     across * ((jitter.x - 0.5) * span) +
                     down   * ((jitter.y - 0.5) * span);
```

Every sample therefore knows *where on the face* it was taken. Keeping only the count is what makes
AO flat over a face. Keep the first and second moments in (u, w) instead and the face carries a
continuous field — **at no extra rays, no extra passes and no fitting step**.

**Why there is no fitting step, which is the whole reason this is affordable.** Use Legendre
polynomials on [−1, 1]: `P₀ = 1`, `P₁ = u`, `P₂ = (3u² − 1)/2`. The jitter is uniform over the face,
so ⟨PᵢPⱼ⟩ = δᵢⱼ/(2i+1) — the basis is *already orthogonal under the sampling distribution*.
Each coefficient is then an independent running mean:

```
ĉ_kl = (2k+1)(2l+1) · E[ v · P_k(u) P_l(w) ]
```

No normal equations, no matrix, no least-squares solve, no second pass over the samples. Each
coefficient accumulates exactly the way the scalar does now, with the sample weighted by two cheap
polynomials. Six terms are kept — `{1, u, w, uw, u², w²}` — and the cross-quadratics are dropped
because nothing in a voxel world produces them at a scale a face can see.

**What each term is, in the picture:**

| term | what it holds | what it looks like |
|---|---|---|
| `1` | the mean | today's flat per-face answer |
| `u`, `w` | the gradient | darkening *towards* the edge that meets a wall — a corner, resolved continuously across the face instead of at its boundary |
| `uw` | the saddle | the inside corner where two walls meet: dark in one quadrant, not in the others |
| `u²`, `w²` | the curvature | the *rate* at which occlusion changes, which is what makes a voxel cylinder read as a cylinder rather than as a faceted prism |

That is the answer to "corner, plane, curvature and shape aware", and it is one answer rather than
four: the field being fitted is the real visibility integral, so a plane fits to a constant, a
corner to a gradient, a crease to a saddle and a curved surface to a quadratic, without anything in
the code knowing which of those it is looking at.

**Read in the composite for six multiply-adds.** The pixel's (u, w) on the face comes from the world
position it already reconstructs and the face key it already has — two subtractions and a multiply.
No extra buffer, no extra fetch: the record is the one it is already reading.

**Where the noise goes, honestly.** The variance of ĉ_k is about (2k+1) times the mean's, so the
gradient terms need roughly three times the samples and the curvature terms about five, for the same
absolute error. Three consequences, all of them design rather than apology: the higher terms are
**allocated only where they earn it** (a face whose measured gradient is inside its own noise stores
nothing but the mean, and most faces in the open are that face); the evaluated quadratic is
**clamped to [0,1] over the face**, which for a quadratic is a closed-form bound on three points per
axis rather than a per-pixel saturate; and R5a's a-trous over the face lattice is what the residual
is left to.

**The trap that will bite, named in advance.** `span` is half the face today, and deliberately: a
ray starting at the exact edge of a face slips through the shared edge of two solid cells — the DDA
crosses both boundaries on one step — and about five per cent of them came out the other side
indoors. A quadratic fitted from the middle half and *extrapolated* to the face edge is worst
exactly where AO matters most, which is the edge. So this sub-step has to sample the full face and
fix the leak at its cause, and it has to prove the fix with the measurement that found it: the share
of rays escaping a fully enclosed room, which must stay at nought.

**Continuity across the face boundary** is not enforced and does not need to be: neighbouring faces
fit the same continuous integral over adjoining domains, so their polynomials agree at the shared
edge to within their noise. That is a claim, so it is gated: no face boundary may be findable in the
AO channel by an edge detector.

**And it survives R8.** A quadratic restricted to a quadrant of its own domain is still a quadratic,
by a fixed linear map — so when infinite detail subdivides a face, the children inherit their part
of the parent's field exactly and there is no pop at the subdivision. AO is the one quantity in this
renderer that gets *continuously* finer rather than in steps, which is what §1 has been claiming for
everything else.

#### R10d — the bent normal, where "shape aware" stops being a figure of speech

The l=1 moment of the same visibility function: the mean unoccluded direction, accumulated as a
vector sum from the same rays, with the length of that sum giving a cone half-angle. Three more
accumulators.

What it buys, none of which a scalar can:

- **the sky is read along it, not along the geometric normal.** A wall beside a doorway is lit from
  the doorway, which is where its light actually comes from. This is the difference between a room
  that is uniformly dimmer and a room that is lit.
- **specular occlusion for R4.** A bin whose direction lies outside the visibility cone is occluded,
  and R4c gets that for free instead of reflecting a room the face cannot see.
- **a gathering direction for R9's bounce**, so the first bounce starts pointed at where the light
  is rather than at where the surface faces.

#### R10e — the shape prior, so a face is never wrong-bright

A newly claimed face has no samples, and the composite's fallback is *no occlusion*, which is the
same failure D308 fixed for the sun and would arrive here in the same clothes: geometry revealed by
a camera move flashing bright before it darkens. Three layers, in the order they arrive:

1. **Instant, from the tree and no rays at all.** The node's child mask and the six coverage bytes
   of its neighbours already say how enclosed this face is — the classic voxel-vertex occlusion
   count, which is cheap, wrong in the third decimal and right about which faces are in a corner.
   As a *seed* it is exactly what is wanted; as an answer it is what everyone else ships.
2. **A few frames later, the coarse stand-in** — D308's machinery, unchanged, because AO folds up
   the tree as light does (R10g).
3. **Converged, the traced integral**, which replaces both.

*Gate: the R9d instrument, unchanged — the share of surface pixels on the fallback, from a cold
store — must not regress, and no camera move may produce a frame in which a revealed surface is
brighter than its converged answer.*

#### R10f — it converges, and then it stops. This is the performance claim

**Ambient occlusion of static geometry is a constant.** The sun moves and its visibility must be
re-traced for ever; AO must not be. A face whose AO has met its variance target leaves the shading
rotation **entirely** — not "refreshes rarely", stops — and the steady-state cost of this stage in a
world nobody is editing is *zero rays per frame*. That is the whole reason it can afford to be good:
the budget is spent on convergence, once, and then returned.

What that requires, and it is the only new machinery in the stage:

- **A convergence test per face**, from the counts already kept — with the higher-order terms
  allowed to keep sampling after the mean has stopped, since they need the samples.
- **More rays while unsettled, none when settled.** A face gets several AO rays a frame while it is
  converging, which costs nothing in the enclosed case because they all die in a metre. The
  arithmetic: a binomial estimator holds ±2% at the worst case p = 0.5 after about 600 samples, so
  at 8 rays a frame a face converges in **under eighty frames** and then costs nothing for ever.
- **Invalidation, which is the part that will be forgotten.** An edit already tells the node pool;
  it must tell the face store to reopen AO within a radius of the edit, or a carved doorway leaves
  the wall beside it dark for the rest of the session. Same shape as `kEditWindow` (D74) and the
  same radius argument as D256–D258: the cost of an edit is the brick, not the neighbourhood.

*Gate: standing still, AO rays a frame fall to nought within N frames and stay there; carving a hole
in a wall re-lights the room within M frames; an edit's own cost does not move.*

#### R10g — fold up the tree, and outlive the face that measured it

A coarse face's AO is the coverage-weighted mean of its children's, and its bent normal is the
normalised sum of theirs. This is R9f's rule — light folds up the tree exactly as colour does —
applied to the one quantity that folds *perfectly*, because it is scalar, geometric and bounded.

Three things fall out at once: distant geometry has AO with no rays traced for it; D308's stand-in
has a real value to hand out rather than an unshaded one; and a face re-claimed after eviction
starts converged instead of starting again. Optionally — and only once R10f's invalidation is
trustworthy — AO persists into the world cache beside the refinement state (D241), because it
depends on geometry alone, so a reload starts lit rather than converging in front of the player.

#### R10h — the debug view, before any of the above is believed

The AO channel on its own; the contact and sky terms separable; **which of the three sources this
face's value came from** (seed, stand-in, traced) in distinguishable colours; and face boundaries
drawn on request, since "no seam is visible" is a claim this stage has to prove rather than assert.
D310 is the reason this is a numbered sub-step and not a line in R10a: an instrument where two
different answers share a colour will produce a wrong number, and it will be the number that was
gone there for.

#### The arithmetic

**Rays.** One to eight per face per frame while converging, zero after. The march is short by
construction — AO's ray dies at the first occluder — where the sun's may cross the whole scene, so
an AO ray is a fraction of the cost of the shadow ray already measured at 0.185 ms for 19k faces and
0.477 ms for 639k (D300). The transient is bounded by the claim rate, which R9b already budgets.

**Memory.** Sixteen bytes a face in the card-only array — counts, mean, contact, bent normal — which
is 7.6 MB at the 477,622 faces the close camera claims and 16 MB at the million-face cap. Plus a
sixteen-byte field block from the size-classed payload pool **only for faces whose measured gradient
is larger than its own noise**, which is creases and corners rather than the flat majority. Stated
plainly because it is the largest single number in the stage: this is not free, it is *bounded*, and
it is bounded by the same face count everything else here is.

**Composite.** Six multiply-adds and a dot product, on a record already being read. No new fetch.

**Resolution.** None of the above is a function of pixels, which is the point.

#### The gate for the stage

- **Against a reference, not against taste.** `--ao-reference N` traces N (default 1024)
  cosine-weighted rays per face in one dispatch and writes the result; the incremental answer must
  match it within 2% mean absolute error over the enclosed camera, and the reference itself is
  checked by doubling N and finding it does not move.
- **Cost:** face pass within +15% while converging, within noise once settled; composite within
  noise; the whole stage within 10% between 800p and 4K, which is what per-face means.
- **Look, measured:** no face boundary findable by an edge detector in the AO channel; a voxel
  sphere's shading is continuous across face boundaries to second order; the enclosed room's mean
  ambient visibility matches the reference rather than 1.0.
- **Behaviour:** two settled frames from one camera stay bit-identical (D194); no revealed surface
  is ever brighter than its converged answer; AO rays a frame reach nought on a still camera.

#### Where it sits

After R3c's sky, because they are the same integral and building them apart guarantees they will
disagree; after R9d, whose stand-in R10e reads. R10a–R10b can land the moment the sky term does.
R10c is the largest sub-step and the one worth its own measurement session. R10d feeds R4c's
specular occlusion and R9's gathering direction, so it wants to be in before R4 closes. R10f is what
makes the whole thing free, and R10g is what makes it survive eviction.

### R5 — Face denoise and the composite · M

- **R5a — a-trous over the face lattice**, kept wholesale from the current tracer; it is the one
  part worth keeping.
- **R5b — temporal EMA per face and parent seeding at claim time.**
- **R5c — deterministic two-level blend** in the composite; delete the ordered dither.
- **R5d — analytic edge AA** from `face_coverage[6]`, compositing up to three partial hits.
  *Gate: speckle down 4× against the R0 baseline on the enclosed-room camera; two identical frames
  are bit-identical; a slow dolly-out shows no transition.*

### R6 — Post, rebuilt for high resolution · M

- **R6a — bloom to a downsampled chain** (today: up to 437 taps per pixel at 1440p, straight out
  of the accumulation image).
- **R6b — motion blur off the composited image.**
- **R6c — one renderer.** Merge `resolve.comp` away, delete the `path_trace_` branch and the F4
  toggle.
  *Gate: post ≤1.0 ms at 4K.*

### R7 — The primary ray · L

The only pass left that scales with resolution, so it gets its own stage.

- **R7a — the beam pre-pass** at 1/8 resolution (`04-rendering.md` §1, carried since Stage 3 and
  never built).
- **R7b — temporal start distance**, validated against the coarse pass so it can never produce a
  wrong hit.
- **R7c — step count in the descent.**
  *Gate: primary visibility at 4K ≤ 1.6× its cost at 1440p.*

### R8 — Infinite detail, experimental · XL

- **R8a — signed levels** through `NodeKey`, the descent and the face key.
- **R8b — hashed variation** as the always-available child source.
- **R8c — field-driven subdivision** from `forge/field.cpp`, which already answers at any
  resolution and whose node array transliterates to a compute shader (`20-clip-forge.md` §4).
- **R8d — derived nodes are evictable; carved ones persist.** That is the whole difference between
  infinite detail and infinite storage.
- **R8e — `--infinite-detail`,** off by default.
  *Gate: standing 10 cm from a wall costs within 30% of standing 2 m from it; resident bytes stay
  bounded by resolution; a carved sub-voxel edit survives a save and reload.*

---

## 9. What is written and what is deleted

**Deleted:** `shaders/pathtrace.comp` (2,728 lines), `shaders/resolve.comp`,
`shaders/pt_normals.glsl`, `src/world/thumb_cache.*`, `src/world/thumbnail.*`,
`src/world/summary_tree.*`, and the whole `path_trace_` branch in `main.cpp`.

**Rewritten:** `src/world/residency.*` → `src/world/node_pool.*`; `shaders/world.glsl` →
`shaders/node.glsl`; `shaders/visibility.comp`; `src/game/quality.hpp`'s knobs become faces per
frame, paths per face and resolution scale.

**New:** `shaders/face_select.comp`, `shaders/shade_faces.comp`, `shaders/photons.comp`,
`shaders/face_denoise.comp`, `shaders/composite.comp`, `shaders/subdivide.glsl`,
`src/world/face_store.*`, `src/gpu/face_light.*` — the card-only half of a face's light, which
exists so the record with two owners never grows another field (D295, and R3d's standing debt).

**Kept nearly as they are:** `pt_sky.glsl`, `pt_clouds.glsl`, `pt_media.glsl`, `pt_sampling.glsl`,
`preview.glsl`, `ui.glsl`, the exposure meter and tone curve in `pt_post.glsl`, the whole world
data model below the chunk, `forge/`, and every test.

Net: roughly 3,500 lines deleted against roughly 2,200 written.

---

## 10. Risks, and what each one would look like

| Risk | How it would show | What is in the plan for it |
|---|---|---|
| Face-pass load imbalance — faces cost wildly different amounts | one workgroup holds the whole dispatch; frame time spikes with no counter moving | persistent threads over an atomic work queue, not one invocation per face |
| A coarse face pools light across a curved or busy surface | a column shades flat; a cornice loses its gradient | keep the `surface_curve` and `shape_complexity` damping, moved into face selection |
| Bin quantisation on a large polished surface | a reflection in visible angular blocks | bins from measured pixel coverage; measure it on a mirror wall in R4 before believing it |
| Analytic edge AA is not enough without any temporal filter | stair-stepping on distant silhouettes | three partial hits composited; if it is short, the fallback is camera-jitter TAA, which is cheap here because lighting is already stable |
| Light latency after a large edit | a room takes a visible moment to relight | the short-window trick that already works (`kEditWindow`), applied to face selection priority rather than to sample weighting |
| Infinite detail multiplies face count | face budget saturates and light stops converging near surfaces | budget is a cap on *convergence*, never on framerate; measure the face count per pixel, which theory says is constant |
| One hash probe per ray is slower than a wrapped-grid fetch | primary visibility regresses in R1 | R0 records the baseline precisely so R1 cannot close on a guess |
| AO noise reads as blotches on a large flat wall | a settled interior that looks dirty rather than dark | the mean converges as √N and the higher terms are allocated only where they beat their own noise; R5a's a-trous over the face lattice is what the residual is for; and R10f can spend more rays because it stops spending them afterwards |
| The face polynomial overshoots near the face edge | a bright or black rim at every voxel boundary, which reads as a wireframe | a quadratic's extremum over a square is a closed form — clamp the coefficients so the evaluated field stays in [0,1] over the whole face, not per pixel |
| AO folded up the tree makes distance too dark | mid and far cameras darken against the reference while the close one matches | fold coverage-weighted, and gate the fold on the mid/far/distant cameras rather than only on the one the feature was built at |
| AO applied on top of a real sky-visibility or bounce term | every crease double-darkened, and it looks like a taste problem rather than a bug | the rule in R10: one visibility factor for the ambient integral, one consumer, checked when R3c's sky and R9's bounce land |
| An edit does not reopen AO near it | a carved doorway leaves the wall beside it dark for the rest of the session | R10f's invalidation radius, and a test that carves and re-measures rather than a screenshot |

---

## 11. Decisions this plan proposes

Numbered from D184, folded into `13-decision-log.md` as each stage lands. The log itself has run
past this range while the rewrite was being built, so the lettered entries at the end are R10's
proposals and take their real numbers when they land.

| # | Decision |
|---|---|
| D184 | **Every ray in the renderer starts on a voxel face.** A pixel gets one visibility ray and one lookup, for ever. |
| D185 | **A face stores a distribution over direction, not a single value**, so reflection, refraction and translucency are face data rather than per-pixel rays. |
| D186 | **A face's bin count is chosen from its measured screen coverage and its roughness**, so angular resolution follows pixels the way spatial resolution already does. There is no roughness threshold and no per-pixel escape hatch. |
| D187 | **One node pool replaces the chunk grid, brick masks, brick mips, the coarse occupancy grids, the summary octree and the thumbnail tiers.** One structure cannot disagree with itself, which is what made pull-only streaming deadlock. |
| D188 | **Chunks leave the renderer and stay on disk.** They are a storage and network page, not a rendering unit. |
| D189 | **Streaming is pull-only**, with one exception: a small proximity radius held for collision, physics and editing. |
| D190 | **A node smaller than a pixel is never fetched, uploaded, shaded or stored.** |
| D191 | **The face store is written by exactly one invocation per face per frame**, which removes every race the current table is built around. |
| D192 | **Coverage is stored per face direction**, folded from children — the quantity Stage 4 needed when volumetric fill fraction failed as an alpha. |
| D193 | **The stochastic level dither is replaced by a deterministic two-level blend in the composite.** A deferred composite can read two levels; a marcher could not. |
| D194 | **No per-pixel random numbers remain in the renderer.** Two identical frames from an identical camera are bit-identical. |
| D195 | **Smooth normals derived from the occupancy gradient are deleted.** A face is flat (D12); detail comes from real smaller voxels, not from lying about a normal. |
| D196 | **Detail continues below the voxel by the same unclamped expression that governs it above.** Levels are signed. |
| D197 | **Sub-voxel nodes are derived and evictable**; only carved ones persist. That is what separates infinite detail from infinite storage. |
| D198 | **Simulation does not descend below level 0.** Sub-voxel matter is visual and editable, not simulated. |
| D199 | **The proximity radius is twenty metres**, at full voxel detail, held regardless of visibility. Affordable because a node pool folds a uniform subtree into one node, which is what stops standing underground costing two million brick slots. |
| D200 | **Sub-voxel depth has no number in it.** A sub-voxel node is identified by its parent and which of eight children it is, so the tree is the coordinate and there is no field to overflow. The practical floor is 32-bit float precision at roughly twenty-four levels down, which is fourteen levels past the closest look anyone can take. |
| D200a | **Ambient occlusion is the visibility factor of the ambient integral, and there is exactly one of it.** The same rays, the same face, the same accumulation as the sun, over the hemisphere instead of over the sun's disc — and nothing in the composite multiplies by it twice. Screen space is excluded on the same line as everything else: it is a second gather per pixel. |
| D200b | **Occlusion varies within a face, and it is stored as a low-order fit rather than as a map.** The face pass already jitters its sample across the face and throws the position away; keeping the Legendre moments of it costs no rays, no pass and no solve, and the terms it produces *are* the value, the gradient and the curvature of the real visibility field. |
| D200c | **AO of static geometry converges and then stops being traced at all**, which is what makes it affordable to be good. The sun cannot do this because the sun moves. Edits reopen it within a radius. |

---

## 12. Settled

| Question | Answer |
|---|---|
| Proximity radius | **20 m** (user) |
| How far below a voxel | **No cap.** Unbounded in the format; float precision is the stated practical floor (user) |
| Reflections of reflections | Kept, and they deepen by one step per frame. Not a choice — a thing to expect rather than report |
| The enclosed-room gate | **30%** to start, because the project's own rule is to measure rather than guess. It will likely land far under, and if it does not, the gap is a measurement to work against rather than a target picked in advance |
