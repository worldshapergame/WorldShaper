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

## 5. Publishing

`.github/workflows/pages.yml`, on every push to `main` that touches `clips/`, the libraries under
the baker, or `web/`. It builds `ws_bake_web`, bakes all forty clips (about three minutes),
compresses each file beside itself, and deploys to GitHub Pages.

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
