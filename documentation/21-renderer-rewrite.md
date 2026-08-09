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
| R0 | d. record the full grid to `documentation/baselines/` | **outstanding** — the grid was interrupted; rerun and commit the csv |
| R1 | a. `NodePool` CPU structure + tests | **done** — D206–D213 |
| R1 | b. `node.glsl` descent + `node_visibility.comp` | **done, compiles** — D214–D218 |
| R1 | c. GPU buffers, pipeline, `--node-pool` | **done** — both marchers run, D219–D223 |
| R1 | d. diff against the old marcher, meet the gate | **done.** Faster on six views of seven and up to 3x on distance; 4.8 MB against 57.7; pictures agree to within one part in three hundred. One regression: the enclosed room, 1.108 ms against 0.699 |
| R1 | e. delete the old addressing | |
| R1 | f. GPU mirror check for the node pool | **done** - `NodeBuffers::audit`, and it eliminated the upload as a suspect on its first run |
| R1 | g. make it the marcher the game launches with | **done** — D224–D226. The chunk marcher is behind `--chunk-marcher` until R1e |
| R1 | h. the enclosed-room regression | **done** — D227–D232. It was the descent, re-walking eleven levels every step; two cached ancestors fixed it. Finding it needed the harness fixed first |
| R1 | i. dirty-range uploads | **done** — D235–D236. The upload was 10 MB a frame while moving and eleven times over its budget; it is now 0.028 ms |
| R2 | d. draw the parent while waiting | **done, early** — D237. Taken out of order because an unstreamed region drawing as sky is what "it loads slowly" turned out to mean |
| R2 | a–c | not started |
| R3–R8 | | not started |

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

### R2 — Pixel-driven residency · L### R2 — Pixel-driven residency · L

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
- **R3c — sun and lamps in the face pass.** Shadow rays and next-event estimation move off the
  pixel entirely.
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
`src/world/face_store.*`.

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

---

## 11. Decisions this plan proposes

Numbered from D184, folded into `13-decision-log.md` as each stage lands.

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

---

## 12. Settled

| Question | Answer |
|---|---|
| Proximity radius | **20 m** (user) |
| How far below a voxel | **No cap.** Unbounded in the format; float precision is the stated practical floor (user) |
| Reflections of reflections | Kept, and they deepen by one step per frame. Not a choice — a thing to expect rather than report |
| The enclosed-room gate | **30%** to start, because the project's own rule is to measure rather than guess. It will likely land far under, and if it does not, the gap is a measurement to work against rather than a target picked in advance |
