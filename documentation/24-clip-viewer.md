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

A 192-byte header, then four blocks, in the layout the card wants so that loading a clip is a fetch
and four `subarray` views. Every offset is written down in both `tools/bake_web.cpp` and
`web/js/format.js`, and the file carries a magic and a version so a mismatch says so.

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
no resolution at all. A shape a `difference` takes away is drawn in red, because the hole somebody
cut is as much a part of the description as the stone it was cut from and is usually the thing you
came to look at.

The baker walks the field from the solid and carries three things down: a 3×4 matrix mapping world
to each node's own space — which is exactly what `eval` does to the point as it descends, so it is
accumulated rather than inverted — the factor `Op::Scale` multiplies a distance by, and whether an
odd number of `difference` subtrahends is above it. `mirror` folds space rather than moving it, so
it emits its child twice; `repeat` is expanded to a cap; `twist`, `bend` and `revolve` have no
honest affine placement for their children and are left out.

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
