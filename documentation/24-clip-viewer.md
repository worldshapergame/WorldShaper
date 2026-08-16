# 24 — The clip viewer

**<https://worldshapergame.github.io/WorldShaper/>**

Every clip in the repository, drawn in a browser. Pick one, turn it round, drag a slider to cut it
in half and look inside, or switch to walking and go in through the door.

It exists for two reasons and both are about a phone. The game runs a path tracer on a desktop
graphics card, so the only way to look at a clip has been to be at that desk. And the facility is
now built by many hands at once — a fragment changes, gets committed, and nobody sees it until
somebody opens the game. This site closes both: it is rasterised, so it runs on anything, and it
rebuilds itself from the clip files on every push, so what it shows is what is on `main` a few
minutes ago rather than what somebody last screenshotted.

## 1. What it is made of

```
clips/*.clip  ->  ws_bake_web  ->  web/data/*.wsc  ->  web/js  ->  a page
                  (C++, CI)        (binary)            (WebGL2)
```

| | |
|---|---|
| `tools/bake_web.cpp` | samples every clip, meshes it, bakes its light, writes one file each |
| `web/js/format.js` | reads that file — a header, four typed-array views, nothing decoded |
| `web/js/gl.js` | the rasteriser: instanced quads, one Cook-Torrance lobe, a stencil cap |
| `web/js/controls.js` | orbit, and a body that walks, crouches, jumps and flies |
| `web/js/app.js` | the page, and the loop that watches the index for changes |
| `.github/workflows/pages.yml` | bakes on every push that can change a clip, and publishes |

### It is the game's own sampler

Nothing in the baker re-reads a clip file. `forge::load_clip_script` parses it, `forge::sample`
turns the field into voxels, `forge::despeckle` cleans it — the same three calls the game makes.
That is deliberate and it is D204's rule: **two things deriving one world from one description is
the failure mode**, and the only way a viewer like this can be trusted is if it is not a second
reading of the language. What the site can differ from the game about is the shading, and it says
so on the page.

The baker builds on Linux with no Vulkan SDK and no SDL, because `-DWS_TOOLS_ONLY=ON` configures
only `ws_core`, `ws_world`, `ws_game` and `ws_forge` and the tools on them. Same sources, same
warnings-as-errors bar; a tool that builds there builds in the game.

## 2. What is in a `.wsc`

A 208-byte header, then the blocks, in the layout the card wants so that loading a clip is a fetch
and a handful of `subarray` views. Every offset is written down in both `tools/bake_web.cpp` and
`web/js/format.js`, and the file carries a magic and a version so a mismatch says so.

**The version is 3.** Version 1's header was 192 bytes with nothing spare, so adding the cutter pool
of §4a moved every block offset; there is no reading one as the other. `reuse` in the baker refuses
an older file and rebakes it, and `parseClip` throws on one rather than drawing a wrong picture —
a cached `web/data` from before the change is a real state and it has to say so.
<!-- >>> lights -->
Version 3 spends the header's last eight spare bytes on a **chunk directory**, so that everything
added after it is found by a four-character code rather than by moving every offset again. §4c.
<!-- <<< lights -->

**Materials.** Every `VisualRecord` used, verbatim, sixteen bytes each. Colour, opacity,
roughness, metallic, index of refraction, emission and its tint, Beer-Lambert absorption,
translucency, the brush-grain flags, clearcoat and sheen. Nothing is quantised on the way out —
*rasterised* is a statement about the light transport and not about the matter, and a viewer that
threw away everything but a colour would not be able to show what the materials in these clips
actually are.

**Quads.** The surface, greedy-meshed, sixteen bytes each: the voxel, the merged extent, the
material, and four two-bit corner occlusions. Grouped by which of the six directions they face, so
the viewer draws six ranges and sorts nothing. The whole facility at 16 voxels to the metre is
409,000 quads.

**A light grid.** A lattice of points 0.4 m apart, each holding how much of the sun and how much of
the sky reaches it, ray cast in the baker against a coarse copy of the clip. In the browser it is
one `RG8` volume texture and one trilinear fetch.

**Occupancy.** One bit per 12.5 cm cell. It is what the walker collides with, and it is why you
cannot walk through a wall.

### Why the light is a lattice and not vertex colour

The obvious place for baked light is the vertex, and it does not survive greedy meshing: two faces
may merge only when everything about them agrees, so a smooth gradient of sky visibility across a
wall makes every voxel face its own quad and the mesh stops being a mesh. Corner occlusion takes
four values, so it merges. Sky visibility is a gradient and does not.

So they are split by that property — the sharp quantised term into the quad, the smooth coarse term
into a volume — and a wall stays one quad while still getting darker as it goes into a room.

**Light leaks through walls and the number that controls it is a half.** A lattice point buried in
stone has no light of its own, and it is read by the surfaces on both sides of it, because a
trilinear fetch near a wall blends the air in front with the stone behind. So buried points are
filled in from their brightest neighbour, twice, at **half** strength each time. It was three
quarters first, and every soffit in the halls had a pale band across it where the sky above the
roof reached through 0.45 m of masonry.

### Resolution is a budget, not a list

A clip says how finely it wants to be sampled; the facility says 32 voxels to the metre, which is
582 million cells and minutes. So the baker **halves** the authored resolution until the cell count
fits `--budget` (80 million in CI, which lands the facility on 16/m and small clips on their own
32/m). Halving rather than stepping keeps the coarse lattice a subset of the fine one, so a wall
does not move by half a voxel between two resolutions. And it needs no per-clip table that somebody
has to remember to edit when a fragment grows.

## 3. Fragments are baked out of the manifest

`clips/facility/dome.clip` declares `part_dome` and no `solid` — the manifest says what the
building is. So a fragment is baked from **`facility.clip`'s parse**, with its own part as the
root, for two reasons:

- a fragment does not include `_order.clip`, so parsed alone it does not know what a dentil is and
  fails on its first use of one;
- a part taken out of the assembled building carries the paint the building gives it, including the
  weathering coats `surface.clip` lays over everything.

A fragment the manifest does not include yet still gets its own parse, so a brand new file is
visible before the three lines that add it are written.

**The parser could be made to end the process, and now cannot.** Baking fragments on their own hit
a SIGSEGV inside `parse_clip_script`, with gdb showing hundreds of frames of
`block → expression → call → block` and nothing else: that cycle recurses once per `{` and nothing
bounded it, so a file whose braces have desynchronised is read through a stack as deep as the rest
of the file is long. It does not reproduce from the same file in a fresh process, so the entry in
the log says what was seen rather than claiming a diagnosis — but an unbounded recursion in a
parser fed files a player writes is a fault whether or not that particular run can be repeated.
`kMaxBlockDepth` is 64 and `tests/test_clip_script.cpp` holds it there.

**`origin` moves the solid and the paint rules and not the names.** `apply_origin` wraps the solid
in a translate and shifts every paint rule; the nodes the file bound along the way are left where
they were, because nothing had ever asked for one afterwards. The facility shifts by 3.50 m, so
`part_dome` came out sampled in a box 3.5 m below its own matter — a twelve-metre saucer four
fifths of a metre tall with one material on it instead of six. The baker moves a part by the same
vector before sampling it. Anything else that reaches for a part by name has the same trap waiting.

## 4. The viewer

**Slicing.** A clip plane, and a clip plane through a voxel mesh leaves an open shell — a wall cut
in half looks like a sheet of paper, because a surface has no inside. So the cut is filled: the
geometry *behind* the plane is drawn with the stencil buffer inverting on every fragment, which
leaves odd parity exactly where the plane is inside matter, and a quad on the plane is drawn
through that stencil. One extra pass over the mesh, and it is the difference between a hollow
building and a stone one. It is skipped when the eye is on the discarded side, where the parity is
counted from the wrong end.

The slice is honoured by the **body** as well as the eye: the walker's collision box is clamped to
the kept side, so cutting the front off a building lets you walk in through the cut.

**Walking.** 1.62 m eye height, a 0.6 m body, gravity, a jump of about a metre, and a step-up of
three of the building's own 0.18 m risers so stairs are walked rather than climbed. Tap the up
button twice to fly, which switches gravity and the body off and turns both buttons into
straight up and down.

**Speed.** One instanced draw per face direction — twelve for a clip with glass in it, twenty-four
while the slider is cutting. There is no per-frame work proportional to the size of the clip, which
is what "no matter how big the clip is" means here. If the frame time still goes over 22 ms the
page renders fewer pixels rather than fewer frames, down to 55%.

**Watching.** `data/index.json` is re-read every five seconds and every clip in it carries a hash
of its own baked bytes. When the hash of the clip on screen changes it is refetched and swapped in
**with the camera exactly where it was**, which is the only way to see what actually changed.

## 4a. The clip before it was voxels

The ◉ button draws the clip **as it was written**: every shape the author typed, ray-marched, with
no resolution at all. It shows the shapes **as the clip resolves them** — a box a `difference` has
cut has a real hole through it, at infinite resolution.

The baker walks the field from the solid and carries three things down: a 3×4 matrix mapping world
to each node's own space — which is exactly what `eval` does to the point as it descends, so it is
accumulated rather than inverted — the factor `Op::Scale` multiplies a distance by, and **the
cutters in scope**. `mirror` folds space rather than moving it, so it emits its child twice;
`repeat` is expanded to a cap; `twist`, `bend` and `revolve` have no honest affine placement for
their children and are left out.

### It used to draw the ingredients, and now it draws the result

It flattened the tree into independent leaves, each with a `sign` of +1 or −1, and marched every
leaf **alone** inside its own box. So a `difference` was not a hole: the subtrahend was drawn as a
solid, in red, standing in front of the stone it was supposed to go through. On `sampler.clip` that
was a red rectangle floating on the face of the left box where a doorway should be, and a red disc
on the middle box where a cylinder should be bored into it. Every overlap anybody had ever written
was on screen at once as raw overlapping primitives. Reported as *"make the sdf raw view mode be the
processed sdfs already cut so that it doesn't show a bunch of overlapping sdfs that some cut another
etc"*, which is the whole of it.

The fix is **not** to evaluate the whole CSG tree per march step. The facility is 15,190 shapes and
no phone will walk that at every step of every ray. Instead it is **local CSG by scope**:

- on `Op::Difference` and `Op::SmoothDifference`, child 0 is walked with `inherited` **plus every
  leaf of children 1..n, flattened with its placement**, and children 1..n are **not emitted as
  shapes of their own**;
- a leaf's cutters are therefore exactly the subtrahends of every `difference` above it, which is
  the correct scope and is what stops two unrelated shapes that merely happen to overlap in space
  from cutting each other;
- they are then filtered to the ones whose **world box actually overlaps** the leaf's, which on a
  real clip leaves almost every leaf with none, one or two;
- the survivors go into a **flat pool** and the shape carries `cut_start` and `cut_count` into it.
  Identical runs are shared, so every leaf of one wall points at that wall's one run of windows.

In the shader that is `d = max(d_self, -d_cutter)` for each cutter in the shape's range, at each
march step. That is exact subtraction. The normal is a world-space gradient of the same combined
distance, because the surface under the ray may belong to a cutter rather than to the shape — the
jamb of a doorway has the doorway's normal, not the wall's.

WebGL2 has no storage buffers, so the pool is an **RGBA32F texture** read with `texelFetch`: a
cutter is its op, its `a[8]`, its 3×4 matrix and its scale — 22 floats padded to 24, so six texels.
`cut_start` and `cut_count` are instanced attributes, and the loop has a constant upper bound as
well as the dynamic one because GLSL ES wants one.

**Nothing is drawn red any more.** Red only ever meant "this one is a hole"; once holes are holes it
has nothing left to say, and every shape is one opaque stone colour.

### The cap, and the one place this is not exact

**Sixteen cutters to a shape.** Over that, the ones with the biggest box overlap are kept — a wall
keeps its doorway and loses a corner bead rather than the other way round — and the baker
`WS_LOG_WARN`s with the count and the clip and prints a summary line in the per-clip output. A
silent truncation reads as "it worked". Measured on the facility fragments, one or two shapes per
fragment are over it: a wall slab in `facility/walls` wants at least 62 and one in
`facility/vestibule` wants 25, and everything else in those clips wants nothing or a handful.
(The reported worst is itself bounded by the collection guard at four times the cap, so a number at
or near 64 means "at least that many".) The cap is 16 because 64 was measured and costs three times
the frame on `facility/walls` for the sake of those one or two shapes.

**Flattening a subtrahend subtree treats it as a UNION of its leaves.** That is exact when the
subtrahend is a union, which is nearly always what a clip's `difference` takes away, and it
**over-cuts** when the subtrahend is itself an `intersection` (a union of the parts is bigger than
their intersection, so more is taken away than the clip takes) or a nested `difference` (whose own
subtrahend should be putting matter back and instead joins the union that removes it). Both are
counted while baking and printed per clip. On the fragments looked at: `facility/windows` has 18
subtrahend subtrees containing an intersection, `facility/halls` has 2, and `walls`, `vestibule` and
`portico` have one nested difference each. This is a known inexactness in the *viewer*, not in the
clip — the voxels are sampled by the game's own `forge::sample` and are unaffected.

Three things it got wrong first, all of them visible immediately and none of them a crash:

- **The op codes must not be the enum's.** `src/forge/field.hpp` comes from whichever branch is
  being grafted, and `Op` is a plain enum whose values shift the moment anybody inserts a solid
  into the middle of the list — which is exactly what the branch that added `arc` did. The format
  carries the baker's own numbering, assigned by `web_op`.
- **A fragment's shapes are the fragment's.** After the intersection with the building's solid the
  root is `intersection { part, the whole building }`, and a walk descends into both: the portico
  came out with 15,927 shapes against the building's own 15,190. The walk starts from the part
  before it is intersected.
- **Nothing may claim a box bigger than the clip.** A `plane` is a half space, its bounds are
  everywhere, and a two-billion-metre impostor flattens the depth buffer for everything else on
  screen. There are sixty-five planes in the facility.

## 4b. A clip that has not changed is not baked again

Every baked file carries, in two words of its header, the key of what produced it: the spliced
source of the program that made it, the resolution settings, and a hash of the sampler's own code.
A clip whose key still holds is read back rather than rebuilt, and it needs no sidecar and no JSON
to parse because everything the index wants is already in the header — the file is the record of
itself. CI keeps `web/data` in a cache between runs.

    nothing changed          0.07 s      forty reused
    one test clip edited     0.6  s      thirty-nine reused
    one facility fragment   83    s      the building and its parts
    cold, all of it        193    s

The fragment case is the honest dependency rather than a missed optimisation: a part is intersected
with the building's own solid and painted with the building's own stack, so when the manifest moves
every part really does change. It is keyed on the manifest's splice for that reason.

**Resolution is split.** A whole clip is baked at the resolution it was authored at, 32 voxels to
the metre; a clip baked as one *part* of a manifest gets half that. Everything at 32 is about two
and a half hours of a runner, because a fragment costs like a small facility and there are
twenty-eight of them:

| | time | quads |
|---|---|---|
| 8 / metre | 21.7 s | 196,076 |
| 16 / metre | 131.2 s | 562,008 |
| 32 / metre | ~13 min | ~1,700,000 |

**A whole clip can cost more than the building.** `estate/campanile` is 1002 root shapes in a tower
88 x 288 x 88 at eight voxels to the metre, and it is baked whole, so it gets 32. Timed on four
cores:

| | time |
|---|---|
| 2 / metre | 4.9 s |
| 4 / metre | 22.4 s |
| 8 / metre | 81.2 s |

About 3.6x per doubling -- sub-linear in voxels, because the field evaluation and the light rays
dominate -- which puts 32 at roughly seventeen minutes for that one clip. A shard holding two like
it is an hour, and that is why `bake` has three hours rather than ninety minutes, and why `index`
runs even when a shard did not.

<!-- >>> lights -->
## 4c. Emissive geometry, as real lights

**The version is 3, and bytes 200..207 are now a chunk directory.** A `u32` offset at 200 and a
`u32` count at 204 point at a table of 16-byte entries — four characters of code, an offset, a size
and a spare word — appended after the blocks. A block can be added to the format without every
reader knowing what it is, which is what lets several people add one to the same file at once.
`parseClip` hands them back as `clip.chunks`, a `Map` from the code to a `Uint8Array`; a code nobody
understands costs nothing. The light list's code is `LGTS`.

The viewer used to treat an emitter as bright paint and nothing else. `clips/many_lamps.clip` is a
sealed hall with no sky and no sun in it, lit by thirty-six fittings, and it drew as **thirty-six
white rectangles on walls the colour of an ambient constant** — everything the clip exists to test
was invisible. `clips/facility/fittings.clip` says the same in its own header: two of the halls have
no window at all and everything past the first bay arrives from a sconce or from nothing.

So `tools/bake/lights.hpp` flood fills the emissive voxels into fittings and reduces each to a
light: a position, an extent, a colour, and an intensity **derived from the emitting surface area**.
`fittings.clip` is explicit that its bowls are 0.08 m² in a hall of 143 m² and that they are small on
purpose — "a big soft area light makes a room easy and tests nothing" — so area is carried rather
than thrown away, and a light knows the radius of the sphere that stands for it. `web/js/gl.js`
shades two lobes with them: diffuse falling off with the square of the distance, and a **GGX
highlight widened by the angle the fitting subtends**. The highlight is why the list exists at all.
A baked irradiance volume knows how much light arrives at a point and not from *where*, so it can
never put a reflection of a sconce on a bronze arm.

### The emission curve is the VIEWER's, not the game's

The path tracer reads a `VisualRecord` as `tint * (emissive/255)² * 64`. The viewer paints an
emissive surface with `glow * emissive * 6.0` — linear, and 6.0 rather than 64. Deriving the *light*
from the game's curve and leaving the *paint* on the viewer's makes a lamp eight times brighter than
the thing it comes out of: many_lamps came out with **every wall clipped to white and the sconces on
it visibly darker than the pools they cast**. A source dimmer than what it lights is the one
lighting error nobody has to be told to see. So the light list uses the viewer's own constant and
the two are one number. Whoever reconciles that 6.0 with the game's 64 changes both together.

### Big fittings are cut up, and dark ones are counted

A cluster whose box is longer than a metre on any axis is cut into pieces at that pitch. It is not
thrift: the corona lucis is a hoop 3.60 m across, and one sphere at the centre of a hoop is a light
in the one place the fitting has no matter at all. Cut up it is a ring of lights, which is what it
is. A cluster with no face touching air emits nowhere and is dropped with a count, not silently.

### The cap, and where it is wrong

**Sixteen lamps a draw.** Ranked by what each delivers at the camera — intensity over distance
squared, the same rank `src/world/light_list.cpp` uses for the game's own list — and the baker keeps
at most 256 in the file. The count, how many a draw is shading, and whether the cap bit are printed
per clip by the baker and logged once per clip by the viewer, and they are on `renderer.lights`.
many_lamps has 36 and the cap bites; `facility/fittings` has 25 and it does not.

The honest limitation is that **the rank is by the eye and not by the surface**: a lamp behind the
camera is scored as if it lit what the camera is looking at. It is visible in an orbit view of
many_lamps from outside, where all thirty-six lamps are the same distance away and which sixteen
survive is arbitrary. Inside the building, where the viewer is actually used, the nearest lamps win
and it does not show.

### Shadowing: a cube of baked distances per light, in one atlas

No rays at run time, and an **unshadowed point light leaks through walls so obviously that it is
worse than no light at all** — many_lamps is built out of exactly that case, four quarters walled
off from each other with a lamp bolted to both sides of every partition.

A per-lattice-point visibility mask on the light grid was considered first and rejected on size: it
costs a bit per light per lattice point, so it grows with the **volume** of the clip, and the
facility's lattice is half a million points. A shadow cube costs the same per light whatever size
the building is. Six faces of 48 texels, one byte a texel holding the distance to the first blocker
along that direction; all of them in one R8 atlas the shader reads with **one bilinear fetch per
lamp per pixel**. Up to 32 lights get one; 48 texels a face is 1.9° a texel, about 13 cm at four
metres, which is about the clip's own voxel.

Four things about the cast are not obvious and three of them were wrong first:

- **The lamp is inside its own matter.** A ray from the centre of a fitting starts in solid stone
  and comes back blocked at the first step. The first atlas was black and every light went out. A
  ray now ignores blockers until it has left the fitting's own box, grown by a voxel.
- **A clip is not a closed room.** A ray that walks out of the sampled box has hit nothing; saying
  "blocked" there puts a black shadow on the far side of every exterior lamp.
- **The rays walk the clip's own voxels**, not a coarse occupancy grid. Half a metre a cell is
  bigger than a whole sconce, and the distance such a grid reports is a cell out — which forces a
  shadow bias larger than the walls this exists to stop light passing through.
- **The stored byte is rounded up, never down.** Short of the real blocker puts the surface *at* the
  blocker inside its own shadow, which is acne on every wall a lamp is bolted to. Long by up to one
  quantum leaks light that far behind an occluder instead, and one quantum is 12 cm on the largest
  clip here. The viewer's bias is then only paying for the ray's own step: 2.5 voxels.

The atlas is filtered `LINEAR`, and that is what makes the shadow soft — the stored value is a
*distance*, so a filtered one is a distance partway between two directions and the comparison lands
partway through a texel rather than on its edge. Each tile's fetch is clamped half a texel inside
its own edge, because the four texels a bilinear fetch reads at a tile boundary belong to a
different direction of a different light.

**Lights past the shadow budget are lit unshadowed.** They are the weakest in the clip by
construction and the last to survive the per-draw cap. What one looks like when it does reach the
screen is a faint wash that ignores a wall. The baker prints the number; the viewer carries it as
`renderer.lights.unshadowed`. On the clips looked at, many_lamps has **4 unshadowed of 36** and
`facility/fittings` has **none of 25**.

### Two arms, on the URL

`?nolamps` turns the whole term off and leaves everything else alone; `?noshadow` keeps the lamps
and drops their visibility, which lights many_lamps as one room instead of four.

### What it costs

| | many_lamps | facility/fittings |
|---|---|---|
| lights found | 36 from 40 clusters | 25 from 25 |
| shaded per draw | 16 (the cap bites) | 16 (the cap bites) |
| shadowed | 32 | 25 |
| emitting surface | 20.28 m² | 4.72 m² |
| atlas | 768 × 576, 432 kB | 768 × 480, 360 kB |
| added to the `.wsc` | 444 kB raw, **~62 kB gzipped** | 361 kB raw |
| shadow bake | 1.7 s | 18.3 s |

The atlas is nearly all "nothing between here and the range" and gzips about ten to one, and the
site serves `.wsc.gz`, so what a phone downloads is a fraction of the raw figure.

**A trap for whoever owns `pages.yml`:** its cache key is
`find src tools/bake_web.cpp`, which does **not** cover `tools/bake/lights.hpp`. An edit to that
file alone would leave every clip's key valid and serve stale bytes. The format version bump hides
it this time — a version 3 reader refuses a version 2 file and rebakes — but the next edit to it
will not have that. The find needs to be `find src tools`.

<!-- <<< lights -->

## 5. Publishing

`.github/workflows/pages.yml`. Several things about it were wrong in ways that made the site look
simply broken, and every one of them is worth knowing before touching it:

**It picks its own ref.** A push says *look again*; it does not say what to bake. Taking the pushed
branch means a push to `main` bakes `main` and a push to the viewer's branch bakes the viewer's
branch, and the new clips are on neither — the facility's overhaul sat on its own branch for a day
while the site showed `main`, faithfully, and read as out of date. The job takes the branch whose
last commit to `clips/` is newest.

**It grafts rather than checks out.** A branch that is only writing clips does not have this
workflow, does not have `tools/bake_web.cpp`, does not have a CMakeLists that knows what
`ws_bake_web` is, and does not have `web/` at all. The job stays on `main` for the machinery and
the site and takes `clips/` and `src/` from the branch — `src/` because the clips and the forge are
one thing, and `arc_test.clip` uses an `arc` that exists only on the branch that added it.

**There is no concurrency group, and the reason is not the one that was written here first.**
`cancel-in-progress: true` was the obvious cause — a commit landing on a branch and on `main` is two
push events and the second killed the first — and setting it to `false` did not fix anything. The
actual cause was in the run list all along: **runs 12 to 16 ended `cancelled` with zero jobs.** They
were never given a runner. A concurrency group holds at most *one* pending run, and the schedule,
then every twenty minutes, kept arriving and displacing the run that was queued. So the group is
gone entirely and the schedule is hourly. Nothing here needs serialising — two bakes of one commit
write the same bytes, and Pages serialises deployments itself.

The viewer's own branch is excluded from the push trigger for the same family of reason: it is
mirrored to `main` commit for commit, so its push was a second identical run and a second deploy
racing the first.

**Twelve runners, and the page before the clips.** A cold bake at full resolution is minutes per
clip and the building dominates, so the `bake` job is a matrix of twelve, each taking every twelfth
clip. Nothing shares state, so the wall clock is the slowest single clip rather than the sum —
sixteen seconds for the lightest shard, thirteen minutes for the one carrying the building.

Ahead of all of that, `early` publishes the page in about half a minute: it restores the last run's
clips, fills in anything missing from the site that is currently published, and runs the baker with
`--index-only`, which writes an index over the files on disk and samples nothing. So the site is up
and every clip that has ever been baked is walkable while the new ones are still being made; the
viewer re-reads `index.json` every five seconds, so they appear as they land without a reload.

That job may only ever **add**. It will not deploy an empty index, and it re-reads the published
commit just before uploading so it cannot put a copy it started from over a real bake that finished
underneath it. As first written it did neither, and on its first run — with no cache to restore — it
would have published a site with no clips on it over a live site that had some.

`web/data/` is **not** committed. It is derived, it is tens of megabytes of binary, and a second
copy of every clip in the history would be out of date the moment somebody edited a fragment.

To look at it locally:

```bash
cmake -S . -B build-web -DCMAKE_BUILD_TYPE=Release -DWS_TOOLS_ONLY=ON
cmake --build build-web --target ws_bake_web
./build-web/bin/ws_bake_web --budget 80000000 --max-metre 16
python3 -m http.server 8777 -d web
```

`--only <id>` bakes one clip, for working on it, and deliberately leaves `index.json` alone.

**The repository's Pages source has to be set to "GitHub Actions"** in Settings → Pages. The
workflow cannot do that for a repository that has never had Pages turned on, and until it is, the
deploy step is the one that fails.
