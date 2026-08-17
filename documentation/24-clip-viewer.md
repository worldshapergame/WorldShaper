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
| `web/js/features/shapeshade.js` | what a shape in the ◉ view is made of, shaded as the voxels are | <!-- // >>> shapeshade // <<< shapeshade -->
| `tools/bake/probes.hpp` | casts the reflection probes; `web/js/features/probes.js` samples them |
<!-- >>> brdf -->
| `web/js/gl.js` | the rasteriser: instanced quads, a stencil cap |
| `web/js/features/brdf.js` | the material model — every lobe a `VisualRecord` declares |
<!-- <<< brdf -->
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

<!-- >>> ao -->
**The version is 3, and the header's last eight bytes are a chunk directory.** Version 1's header
was 192 bytes with nothing spare, so version 2 — which adds the cutter pool of §4a — moved every
block offset; there is no reading one as the other. Version 3 does not move anything: bytes 200..207
became `chunkOffset` and `chunkCount`, and each 16-byte entry is a four-character name, an offset, a
size and a spare word. Everything in front of the directory is byte for byte what version 2 wrote,
which is the point — the format is being added to by many hands at once, and doing the version 2
move again for each of them is a chance to serve a file one reader agrees with and another does not.
A reader that does not know a chunk simply never asks for it.

`reuse` in the baker refuses a file of the wrong version and rebakes it, and `parseClip` throws on
one rather than drawing a wrong picture — a cached `web/data` from before a change is a real state
and it has to say so.

Chunks so far: `AOCC`, the ambient-occlusion atlas of §2a.
<!-- <<< ao -->

<!-- >>> gi -->
**The version is 3, and the last eight bytes of the header are now a chunk directory.** Everything
in front of byte 200 is exactly what version 2 held; 200 is `chunkOffset` and 204 is `chunkCount`,
and a chunk is sixteen bytes of table — a four-character tag, an offset, a size and a spare word —
pointing at a block appended after every block the fixed layout knows about. That exists because
the viewer is being extended by many hands at once and the alternative is everybody needing the
same spare word: a new baked block now costs a tag and no offset in front of it moves. A reader
that does not know a tag skips it, and `clip.chunk('GIRR')` in `web/js/format.js` hands back a view
of one without decoding anything — whoever bakes a chunk owns reading it.
<!-- <<< gi -->

<!-- >>> probes -->
**The version is 3.** Version 1's header was 192 bytes with nothing spare, so adding the cutter pool
of §4a moved every block offset; there is no reading one as the other. Version 3 spends the eight
bytes version 2 left over on a chunk directory, so it is the last time an addition moves anything.
`reuse` in the baker refuses an older file and rebakes it, and `parseClip` throws on one rather than
drawing a wrong picture — a cached `web/data` from before the change is a real state and it has to
say so.
<!-- <<< probes -->
**The version is 3.** Version 1's header was 192 bytes with nothing spare, so adding the cutter pool
of §4a moved every block offset; there is no reading one as the other. `reuse` in the baker refuses
an older file and rebakes it, and `parseClip` throws on one rather than drawing a wrong picture —
a cached `web/data` from before the change is a real state and it has to say so.
<!-- >>> lights -->
Version 3 spends the header's last eight spare bytes on a **chunk directory**, so that everything
added after it is found by a four-character code rather than by moving every offset again. §4c.
<!-- <<< lights -->
<!-- >>> matvol -->
**The version is 3.** Version 1's header was 192 bytes with nothing spare, so adding the cutter pool
of §4a moved every block offset; there is no reading one as the other. `reuse` in the baker refuses
an older file and rebakes it, and `parseClip` throws on one rather than drawing a wrong picture —
a cached `web/data` from before the change is a real state and it has to say so. Version 3 is the
last version bump that kind of change needs: the header's final word is now a **chunk directory**,
so a new block is appended and listed rather than inserted, and §6 is the first two entries in it.
<!-- <<< matvol -->

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

<!-- >>> gi -->
**An irradiance volume**, the `GIRR` chunk: a second lattice at 0.8 m holding the light that has
**bounced**, with the colour it bounced off — six RGB values a point, one per world axis. It is
what makes a white vault go warm over a porphyry floor, and it is the section below.
<!-- <<< gi -->

**Occupancy.** One bit per 12.5 cm cell. It is what the walker collides with, and it is why you
cannot walk through a wall.

<!-- >>> matvol -->
**A material volume and a thickness field**, on those same cells. What the stone at a point inside
a wall is made of, and how far a ray would travel through it. §6.
<!-- <<< matvol -->

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

<!-- >>> gi -->
### The bounce is a second lattice, in colour, and it is an ambient cube

The grid above is a **visibility** term. Two bytes cannot carry a colour, so until this existed
nothing in the viewer bounced light off a red floor onto a white vault — which is the single most
visible thing a path tracer does, and the thing `clips/facility/rotunda.clip` was built to put
under load ("if the vault comes back neutral white, the bounce is carrying luminance and not
spectrum"). So there is a second lattice, the `GIRR` chunk, holding **indirect radiance with its
colour**. The sun's direct term and the sky have not moved: they are still the two visibility
bytes, and this holds only light that has bounced, so nothing is counted twice — a gather ray that
reaches the sky contributes **nothing** here, because the sky's own arrival is already the sky byte.

**Six RGB values a point, one per world axis — the Half-Life 2 ambient cube — and here it is exact
rather than approximate.** Every surface this rasteriser draws is a merged voxel face, and a voxel
face's normal is one of ±X ±Y ±Z: it is `u_normal`, a *uniform*, set once per draw call. So the
face of the cube a fragment wants is known before any fragment runs, and the whole basis costs one
bound 3D texture per face pass and one trilinear fetch. Second-order spherical harmonics would be
nine RGB rather than six, would need reconstructing per fragment, and would ring — dark haloes on
exactly the surfaces this exists for. What a cube cannot do is hold a sharp directional lobe; that
is the right thing to give up for a term that is low-frequency by construction.

**Two bounces, by iterating one gather.** Every surface cell of a coarse copy of the clip takes its
direct light out of the grid already cast and radiates `albedo × E + emission`; every lattice point
gathers that over 64 Fibonacci directions with a 3D-DDA march; every surface then re-reads the
result along its own normal and radiates again; and the second gather is what is written. So a wall
lit by a floor lit by the sun is lit. **Emissive voxels are sources in the first pass** at the same
6× scale `web/js/gl.js` draws them at, so a sconce lights the wall behind it by the amount it is
seen to glow — which matters because the facility's halls are lit by nothing else.

The march is a DDA and not the visibility rays' fixed half-cell step, and the difference is not
speed: a fixed step crossing a cell corner diagonally walks through a one-cell wall, which costs a
visibility term one part in thirty-two of a sky fraction and costs this one a room the colour of
the room next door.

**Storage decided it, and the lattice is half the light grid's — 0.8 m.** Eighteen bytes a point
against the light grid's two is only affordable at an eighth of the points. Measured: the rotunda's
light lattice is 34 × 32 × 34 = 36,992 points, so a full-resolution ambient cube would be **666 KB
against a whole baked clip of 1.1 MB**. At 0.8 m it is 18 × 17 × 18 = 5,508 points and **97 KB, an
8.5% bigger file**; `facility/vestibule` pays 17 KB and `sampler` 25 KB. Indirect light does not
miss the resolution. Values are `L = (v/255)² × 4.0` — a square because it is one multiply on a
phone, a curve rather than a linear ramp because a linear 8-bit encoding of 0..4 steps by 0.0157,
which is a third of the whole indirect term in a dark interior and bands visibly across a vault.

Buried points are filled from their brightest neighbour at **half** strength, twice, exactly as the
visibility grid is and for the same reason. `tools/bake/irradiance.hpp` is the bake and
`web/js/features/gi.js` is the browser's half.

**The one thing it knowingly over-counts.** The fragment shader still adds its flat `ambient` term,
which was the crude stand-in for indirect light before this existed, so an interior is now lit by
both. Removing it is a change to the look of every clip and belongs in its own pass, not in the one
that adds the volume. Measured on `facility/rotunda` cut in half, the interior lifts by about 45 of
255 and the sky is byte-identical.
<!-- >>> ao -->
## 2a. Ambient occlusion, which is neither of the two things that were called that

Two terms already existed and neither of them is ambient occlusion:

- **corner occlusion**, four two-bit values on a quad, from the eight voxels round each corner. The
  classic Minecraft vertex darkening. It is **one voxel wide** and knows nothing outside its own
  cell.
- **sky visibility** in the light grid, on a **0.4 m** lattice. That is the room a surface stands
  in.

Between one voxel and forty centimetres is the entire middle scale, and it is the scale this
building is made of: 120 coffers in the dome, twenty-four flutes on every shaft, the dentils under
the cornice, the niches, the reveal of every window, the joint where a wall meets a floor.
`clips/facility/rotunda.clip` had already written down what the failure looks like — a coffer's
whole appearance is "a soft gradient from a bright lip to a dark pan", and "if ambient occlusion is
wrong, a coffer reads as a flat dark square". It did, and that was the acceptance test.

So the baker casts a **hemisphere of thirty-two rays about each exposed voxel face's own normal, out
to 0.45 m**, distance-weighted, against the clip's own voxels at the resolution it was sampled at.
`tools/bake/occlusion.hpp`, read by `web/js/features/ao.js`.

**It is an atlas, one texel per exposed voxel face, and not a volume.** The volume was the obvious
answer — the light grid is one — and it was rejected on three counts, of which the first is measured
and the second is the one that actually decides it:

- **A volume is n³ and a surface is n².** The rotunda fragment is a 12.6 × 11.6 × 12.6 m box. A
  0.1 m volume of it is 1.84 M cells; its whole exposed surface at the 16/m it is sampled at is
  **353 k texels, 0.34 MB**. The atlas is five times smaller *and* sharper — 6.25 cm against 10 cm —
  and the gap widens with every clip that is more air than stone, which is every clip.
- **A lattice point has no normal.** Hemisphere-sampled against the surface normal is what ambient
  occlusion *is*; a point in space can only carry sphere openness, which says the same thing about
  the floor and the ceiling of a 0.3 m recess. Every quad here is axis-aligned and knows which of
  six directions it faces, so the hemisphere is free.
- **A volume leaks through walls.** A trilinear fetch near a 0.15 m wall blends the open room in
  front with the stone behind — the fault the light grid needed its "half, twice" neighbour fill to
  survive. At a 0.1 m cell the leak is *worse*, because the fetch reaches further in voxels.

**It survives greedy meshing, and that is why it is a texture rather than a vertex.** Two faces
merge only when everything about them agrees, so a smooth gradient at the vertices makes every voxel
face its own quad and the mesh stops being a mesh — the same argument as the light grid above. A
texture is read per fragment and merging cannot see it, so a wall stays one quad and still has a
shadow in its corner.

**The file carries no per-quad UV.** Runs are allocated in the order the quads are written — every
opaque face group in face order, then every transparent one — so where a quad's run starts is the
prefix sum of `w * h` over the quads in front of it, and the viewer computes that from the quads it
already has. Four bytes a quad is 1.6 MB on the whole facility for a number that is a sum of two
fields sitting next to it. Two things deriving one layout from one description is what D204 is named
for, so the chunk writes down its total and the viewer throws if its walk does not land on the same
number.

Filtering is **four `texelFetch`es and a lerp by hand**, clamped inside the quad's own run. A run
wraps at the edge of the atlas and sits against its neighbour's with no border, so a hardware
bilinear fetch would blend a windowsill into whatever was meshed after it; and a one-texel border
round every quad is half as much memory again on a mesh with four hundred thousand of them.

What it costs, measured at `--budget 8000000`, which lands these fragments on 16 voxels to the metre:

| | quads | texels | added | file was | bake added |
|---|---|---|---|---|---|
| `facility/rotunda` | 54,739 | 352,942 | 0.34 MB (+30%) | 1.11 MB | 3.3 s onto 100 |
| `facility/dome` | 94,563 | 129,752 | 0.13 MB (+8%) | 1.56 MB | 0.6 s onto 55 |
| `facility/portico` | 15,327 | 110,764 | 0.11 MB (+28%) | 0.39 MB | 0.3 s onto 15 |
| `sampler` | 28,612 | 78,582 | 0.08 MB (+16%) | 0.47 MB | 1.9 s onto 1.4 |

That last row is the honest shape of the cost and not an outlier: the rays stop at the first
blocker, so a clip that is mostly open faces runs every ray to its full length and a clip full of
recesses does not. On a small clip the atlas can therefore cost more than the sampling did, in
absolute seconds that are still under two.

Per frame it is one `R8UI` texture, one instanced `uint` attribute and four `texelFetch`es in the
fragment shader — no extra pass, no extra draw and no per-frame work proportional to the clip.
**The number is not quoted because it was not measured.** The only harness available here is
SwiftShader, and the two arms of the control flag came out 145 ms and 1281 ms on the *same* arm at
the rotunda: the page's own resolution scaler is chasing the load and the box is shared. A figure
off that is worse than none.

**Corner occlusion is untouched.** It is a different term at a different scale and the two multiply:
one is the voxel's own shape, one is the recess it sits in.

**Where it does not show, said plainly: the flutes.** Twenty-four flutes on a 0.9 m shaft are
0.12 m apart and a couple of centimetres deep, which at 16 voxels to the metre is two voxels wide
and a third of a voxel deep — there is almost no geometry in the sampled clip for a hemisphere to
find. The portico's shafts gain a little and the coffers, the soffit panels, the capitals, the niche
heads and the wall-to-floor joints gain a lot. At 32 voxels to the metre the flutes would be real
and so would their occlusion.
<!-- <<< ao -->
<!-- >>> probes -->
**Reflection probes.** A sparse lattice of small octahedral maps of what the clip's open space can
see, cast in the baker and read at run time, in the `RPRB` chunk. §2a is the whole of it.

### The version is 3, and the last eight bytes of the header are a directory

Version 2 left bytes 200..207 spare. They now hold a **chunk directory** — `u32 chunkOffset`,
`u32 chunkCount`, then 16 bytes an entry (`char fourcc[4]`, `u32 offset`, `u32 size`, `u32
reserved`) — so that everything added to the format from here is a fourcc and a range rather than
another fixed offset every block below it has to move for. That is what made a version 1 file
unreadable as version 2, and it was not worth arranging twice. A reader that does not know a fourcc
skips it; two chunks added by two hands cannot collide.

## 2a. Reflection probes

Nothing in the viewer reflected anything, and the clips were built to. `salon.clip` and
`ballroom.clip` face walls of `mirror` (roughness 6, metal 252) at each other; `pavilion.clip`
stands in a basin of still water; the contract declares three golds a stop apart — `ormolu` 48,
`gilt` 64, `gold_leaf` 40 — precisely so that metal can be judged. All of it rendered as flat
diffuse, because a rasteriser's only opinion about what a surface can see is the analytic sky.

**A probe is a lattice point that is in air and can see matter.** Fourteen short rays, at least
three of which have to hit something within six metres: that puts probes in rooms and above floors
and keeps them out of the open air above a building, where a probe holds nothing the sky term does
not already draw better. The spacing is a budget rather than a number — it starts at two metres and
doubles until the atlas fits a megabyte, and the resolution goes to 64×64 only when 32×32 would
have left three quarters of that budget unspent.

| | probes | spacing | size | probe bytes | bake added |
|---|---|---|---|---|---|
| `mirror_test` | 20 | 4 m | 64² | 0.67 MB | 1.3 s on 0.3 s of sampling |
| `facility-salon` | 17 | 2 m | 64² | 0.67 MB | 1.8 s on 506 s of sampling |

**Octahedral, not a cubemap.** One 2D texture and one atlas, against six faces and six sets of seams
per probe with no way to pack many into one binding. The octahedral square's edges fold onto
themselves by a rule that a border of one texel satisfies exactly — and an atlas needs that border
anyway to stop one probe bleeding into the next, so the seam costs nothing that was not already
being paid. The mapping is **edge-aligned**: texel 0 sits at uv −1 and texel *size*−1 at +1, so the
boundary lands on texel centres and the fold is an integer reflection rather than a half-texel
guess.

**Pre-filtered into five levels**, laid out left to right inside a probe's tile, each half the width
of the one before. Level 0 is the raw cast; levels 1..4 are integrated from it with a lobe whose
width is the **larger** of what the roughness asks for and what one texel of that level subtends, so
a level is never sharper than the texels it is stored in. Roughness reaches a level through
`pow(roughness, 1/1.5) * 4`, which spreads the clips' own materials rather than bunching them: the
three golds land at 1.17, 1.31 and 1.59, and `mirror` at 0.33.

**Said plainly: a 32×32 probe is not a mirror.** Three degrees a texel is a soft reflection however
low the roughness goes. That is the division of labour with the screen-space pass — SSR is sharp and
sees only what is on screen, a probe is soft and sees everything, including what is behind the
camera, which is most of what a mirror facing you shows. The probe path is what SSR falls back to,
through one function:

```glsl
vec4 probeReflection(vec3 world, vec3 normal, vec3 reflectDir, float roughness);
vec3 probeFresnel(vec3 f0, float ndv, float roughness);
```

`.rgb` is linear radiance pre-tonemap at the sharpness `roughness` asked for; `.a` is how much of
the point's probe neighbourhood actually had probes in it, so 0 means fall back to the sky and the
caller decides how.

**It reuses the light grid's machinery, and one grid rather than the other.** The rays are a
two-level DDA — skip a coarse cell at a time, step voxel by voxel inside an occupied one — and what
a hit is *shaded* with is the light grid already cast at that point, so a probe agrees with the wall
it is looking at instead of being a second opinion about it. The acceleration grid is the
**conservative** collision grid and not the light grid's copy: the light grid fills a cell only when
a third of it is solid, and a `mirror` in these clips is a coat of paint one voxel thick that a ray
would walk straight through.

**Parallax-corrected against the clip's own matter box.** Without it a reflection slides across a
surface as the camera moves and it is the first thing anybody notices. Its limit is the one every
box-projected probe has: it is right when the geometry roughly *is* the box, which a room is and an
open field with four posts standing in the middle of it is not.

Two honest costs. A probe's tile is as tall as level 0 and levels 1..4 are half that and less, so
about two fifths of the atlas is never sampled — reclaiming it means packing the small levels under
each other and is worth about 0.25 MB a clip. And **the run-time cost is real**: up to eight lattice
corners, two roughness levels each, is sixteen texture fetches a pixel in the worst case. Measured
on SwiftShader — a software rasteriser with no texture cache, so an upper bound rather than a phone
number — `mirror_test` at 900×700 goes from 0.42 s to 1.96 s a frame with probes on.
<!-- <<< probes -->
<!-- >>> shadow -->
### The sun is no longer that lattice, and the number that says why is 3 cm

**A 0.4 m lattice cannot represent a 3 cm bar's shadow at all.** Not softly, not badly: the bar is
a thirteenth of one cell and there is no value in the volume that knows it is there. The facility
is built to catch exactly that — `portico.clip` is a hard sun shadow across shafts whose fillets
are 0.030 m, and `crypt.clip` puts three gratings of **0.030 m iron bars** over a floor and calls
the stencil they throw the highest-contrast small feature in the building.

So the sun now comes from a **shadow map**: orthographic down the sun, over the clip's own bounds,
rasterised from the same instanced quads the surface pass draws. Neither the clip nor the sun
moves, so it is rendered **once at load and never again**. `web/js/features/shadow.js`.

**The texel, measured in the running viewer at 2048, against a 0.030 m bar:**

| | metres across, in the sun's frame | cm per texel | a 3 cm bar is |
|---|---|---|---|
| `sampler` | 12.8 | 0.67 | 4.5 texels |
| `facility/portico` | 16.5 | 0.86 | 3.5 texels |
| `facility/crypt` | 27.5 | 1.39 | **2.2 texels** |
| `facility` whole | 45.8 | 2.28 | **1.3 texels** |
| the near cascade | 16.0 | 0.78 | 3.8 texels |

**One 2048 map resolves a 3 cm bar on every fragment and does NOT resolve it on the whole
building.** 1.3 texels is under Nyquist — the bar lands on one texel here and none there, and seven
bars in a row come out as five stripes and a smear, which is the failure `crypt.clip` exists to
catch. 4096 over the whole building is 2.6 texels, which resolves and aliases, and costs 64 MB of
depth on a phone. Hence a **near cascade**: a second 2048 map over a 16 m box round the eye,
re-rendered only when the eye has walked 4 m out of the box it was rendered for, and **not built at
all** for a clip whose own span is under 20 m — which is every fragment.

The honest limit is not the map. A fragment is baked at 16 voxels to the metre, so a voxel is
6.25 cm and **a 0.030 m bar is half of one**: the bar the map could carry is not in the mesh to
begin with. At the contract's own metre 32 it is one voxel and the map has three texels across it.

### The soft term and the sharp term are crossfaded, never added

Both answer the same question — what fraction of the sun's disc reaches this point — so **adding
them doubles the shadow and multiplying them squares it**. They are split by scale, and the number
that decides is the distance to the blocker, which one extra fetch of the map gives.

The sun is about half a degree across, so a penumbra is 0.0093 of the throw: 3 mm at a 0.3 m throw,
and **0.40 m at 43 m — which is the light grid's own cell**. So the grid is not a fallback, it is
the correct answer for a shadow thrown far enough.

- **throw under 5 m** — the map, its PCF kernel opened to exactly that penumbra width (0.6 to 3
  texels).
- **throw over 20 m** — the light grid, which *is* a 0.4 m blur of the same visibility.
- **between, and outside the map's box** — crossfaded.

Contact shadows are combined with a **minimum, not a product**: whatever the map found, a
short-range trace can only ever find more, and where the map already says nothing reaches this
point a product would darken it a second time. The **sky** byte is untouched — the leak above is
still the grid's and still a half. The sun byte's leak is gone wherever the map covers, because a
map has no concept of a buried point.

### Which artefacts were traded for which

The grid leaks light through walls. A shadow map has the opposite pair, and these are the ones now
on screen:

- **acne** — a lit face self-shadowing, because the depth it stores is its own. Bought off with a
  **normal** offset of 1.6 texels rather than a depth bias: a normal offset moves the sample off
  the surface instead of down the light, so it costs nothing extra at the grazing angles where a
  depth bias costs most.
- **peter-panning** — what that offset buys is a shadow that starts 1.6 texels late and detaches
  from the foot of what casts it: 1.4 cm on the portico, 3.7 cm on the whole building.

**That detachment is what the contact shadows are for.** A 0.30 m screen-space trace up the sun
ray, eight steps, against a depth pre-pass, is the scale *below* the map's texel — the join where a
bench meets a floor, the underside of a moulding, a baluster against its rail — and it closes
exactly the gap the normal offset opens. It is skipped outright wherever the map already says the
point is in shadow, which in a portico is most of it.

**What it costs.** The map is not a per-frame cost at all. Per frame there is one depth-only pass
over the opaque mesh for the trace to read, ten fetches of the map per lit pixel (one for the
blocker, nine for the kernel), and up to eight of the depth buffer where the trace runs. 16 MB of
depth per map. It could not be timed here: the only GL available in CI is SwiftShader, and its
control arm varied by 16% between two runs of *itself*, which is larger than the effect.
<!-- <<< shadow -->

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

**The parser could be made to end the process, and the cause is now known.** Baking fragments on
their own hit a SIGSEGV inside `parse_clip_script`, with gdb showing hundreds of frames of
`block → expression → call → block`. That cycle recurses once per `{` with nothing bounding it, so
`kMaxBlockDepth` = 64 went in as a bound, and this document said the crash "does not reproduce from
a fresh process" and recorded what was seen rather than a diagnosis.

**It is a heap read one past the last token, and the depth guard is what caused it** (D668). The
guard abandons a hopeless file by setting `at_ = tokens_.size()`; `block()` had already checked
`!done()` before calling `expression()`, and the next thing it does on failure is read `peek().text`
to say which token it did not like. `peek()` was the only accessor here that did not test `done()`
first. Because it is a heap read rather than a stack overflow, whether it crashes depends on what
sits after the token array — nesting 90, 95, 96, 100, 104, 110 and 1000 deep all segfaulted while
80, 120, 128 and 4000 came back clean, which is the whole of the "does not reproduce" mystery.
AddressSanitizer names it in one line. Fixed at the accessor, so every reader past the end sees an
empty token that no branch matches; the depth guard stays, because it was right.

It was found by `ws_tests_headless`, which is the reason that target exists: the suite could not be
run at all on a machine without a graphics stack, and every machine these clips are written from is
one of those.

**`origin` moves the solid and the paint rules and not the names.** `apply_origin` wraps the solid
in a translate and shifts every paint rule; the nodes the file bound along the way are left where
they were, because nothing had ever asked for one afterwards. The facility shifts by 3.50 m, so
`part_dome` came out sampled in a box 3.5 m below its own matter — a twelve-metre saucer four
fifths of a metre tall with one material on it instead of six. The baker moves a part by the same
vector before sampling it. Anything else that reaches for a part by name has the same trap waiting.

<!-- >>> brdf -->
## 3a. The material model

The viewer shaded one Cook-Torrance lobe. A `VisualRecord` carries six more things, every one of
them written by real clips, and each one it ignored was a material drawn as grey plastic: the three
golds of `_contract.clip` — `ormolu` at rough 48, `gilt` at 64, `gold_leaf` at 40, put a stop apart
so the question "is the metal right" becomes a comparison the eye can make inside one frame — were
three shades of the same mustard, and `velvet`, `silk`, `parquet` and `porcelain` were four matte
paints.

`web/js/features/brdf.js` is the game's own shading, ported. `shaders/pt_material.glsl`'s
`surface_response` is the reference for the lobes; `shaders/face_terms.glsl` and the composite in
`shaders/resolve.comp` are the reference for the half a rasteriser has to do differently. The file
itself carries the full list of what is matched term for term and what is approximated, and the
short version is:

- **`brush`** — the anisotropic base lobe at `kBrushStretch = 2.45`, narrow along the grain and
  wide across it, the grain being a **world axis** projected into the face. A face the grain runs
  straight out of has no grain, which is why the cut end of a brushed baluster has a round
  highlight and its sides have a stretched one.
- **`sheen`** — on the **diffuse** lobe, not the specular one, and Fresnel-shaped on the **half
  vector**, verbatim from `surface_response`. So a cloth goes bright where the eye and the light are
  far apart — side-on, backlit, round the edge of a fold — and `(1 − v·h)⁵` is nearly nought when you
  look straight down the beam. **This is not retro-reflection and neither is the game's.**
  `_contract.clip` calls velvet "brightest where you look along the light", which is what the
  material is and is not what either renderer draws; matching the game was the instruction.
- **`lacquer`** — a second lobe at a fixed roughness of 0.06 with its own dielectric Fresnel, and
  everything underneath **dimmed by what the coat sent back**. That last part is what makes it read
  as a coating rather than as a shinier material.
- **`metal`**, **`ior`**, **`emit`** and its tint — `f0 = mix(dielectric, albedo, metallic)`, no
  diffuse for a metal, and emission on the game's own `tint * emissive^2 * 64` curve.

### The factor of PI, which is why the metals were dark

The game writes a BRDF: `surface_response` returns `f * cos`, its diffuse is `albedo / PI`, and the
composite multiplies by an irradiance. This viewer has never had the `/ PI` — its diffuse is
`albedo * sunColour * n·l`, and its exposure, its sky colours and every screenshot ever taken of it
are tuned around that. Both are defensible and they differ by PI **on the diffuse alone**, so the
specular was PI times too weak relative to it. A factor of three on the specular of a metal is the
whole difference between metal and paint.

So the conversion is stated once, in `brdf.js`, and nothing else has to know it: **this viewer's
surface term is `PI * (the game's surface_response) * irradiance`.** The diffuse comes out exactly
what the shader drew before, so stone does not move, and the specular arrives at the strength the
game gives it. The old code also carried `n·l` twice on the specular; that went with the same
change.

The **environment** term is deliberately not multiplied by PI. A prefiltered environment is already
an outgoing radiance — `F(f0, n·v) * L_env` is what a mirror shows — and a mirror has to be exactly
as bright as the sky it is reflecting or the reflection is brighter than the thing reflected.

### Indoors, a surface reflects the room, and the room here is the ambient

The environment was the sky along the reflection attenuated by how much sky the baker's lattice
could find from that point — right outdoors, and nearly nothing four metres inside a state room, so
every specular in the salon and the ballroom went out and what was left was diffuse. The game has no
such problem because a face's lobe bins hold **the room**: what a surface reflects indoors is
measured, and it is the room. This viewer's one stand-in for the room is the ambient the light grid
already carries, so a surface now reflects the sky where the sky reaches it and that ambient where
it does not, **at the same occlusion the diffuse gets** — taking it unoccluded is a room with its
shadows washed out.

### And the environment is prefiltered by the lobe reading it, which is what separates two metals

That fallback lifts every specular in a room, gilt and plaster alike, and on its own it is **not a
metal fix**. What separates one metal from another is roughness, and roughness reached the picture
only through the sun's own highlight — so in an interior, where there is no sun on anything, a
mirror at roughness 6 and a bronze at 110 drew the same.

It cannot be fixed by blurring the sky along the reflection, and that is the interesting part. The
sky is two things added together and they prefilter differently: the **gradient** is smooth over the
whole hemisphere and survives any lobe, and the **sun's disc** is about four degrees across and is
spread out by the lobe until it is gone. Blurring their sum by one number leaves every metal in the
building "sharp", because every metal's lobe is a few degrees wide. So `ws_environment` spreads the
disc instead: a lobe of half-width *w* reads it over `(w / disc)²` times its solid angle, the peak
falls by exactly that, and the energy does not change. Below the disc's own width nothing happens at
all, which is what keeps a mirror showing the sun exactly as the sky draws it.

Measured on `facility-salon`, orbit at yaw 1.5708, pitch −0.02, distance 6.2, target
(7.2, 0.2, −4.8), 900×700 — mean sRGB of a fixed box, decoded from the screenshot, comparable down a
column only. Every arm but the first is `?lobes=` on the same build:

| | gilt panel | ceiling | pilaster | parquet | damask |
|---|---|---|---|---|---|
| | metal 225, r64 | plaster | pale, no coat | lacquer 10 | sheen 10 |
| before | 90, 81, 42 | 76, 81, 89 | 125,129,132 | 82, 69, 67 | 58, 51, 66 |
| this | 105, 95, 50 | 71, 75, 82 | 128,131,134 | 80, 71, 78 | 55, 48, 63 |
| …no lacquer | 105, 95, 50 | 71, 75, 82 | 127,130,131 | 71, 56, 51 | 53, 46, 61 |
| …no sheen | 105, 95, 50 | 71, 75, 82 | 127,131,133 | 80, 71, 78 | 54, 48, 63 |
| …no metal | 61, 56, 38 | 71, 75, 82 | 122,126,132 | 79, 71, 78 | 55, 48, 64 |

The gilt is up a sixth and warmer while the plaster beside it is down a fifteenth and the pilaster
has not moved — the metal picks out from the dielectric, which is the whole test. Turning metal off
now costs the gilt **42 per cent** and the plaster nothing. The lacquer is the sky in the floor:
parquet goes 71,56,51 without it to 80,71,78 with it, half again as much blue on a brown floor,
which is a low sky in a polished surface at a grazing angle. And the sheen is worth one unit here,
because it is `(1 − v·h)⁵` and lives where the eye and the light are far apart, which is what the
game does.

Taken on its own with `?lobes=-sheen`, on the salon's silk ceiling — `silk` is sheen 15, the
strongest in the building — the term is worth **1.0 per cent** looking down the room and **3.4 per
cent** with the camera up at the coving where the ceiling is most nearly edge-on, and **exactly
nothing** on the blue wall or the gilt beside it. It is quiet indoors for the reason it should be:
what a grazing lobe returns is proportional to the light arriving, and there is very little on that
ceiling. It is a real term correctly isolated, not a large one in these rooms.

Outdoors, on `mirror_test` at yaw 0, pitch −0.05, distance 5.0, target (0, 0.95, −1.2):

| | chrome r8 m250 | brushed r110 m220 | gold r40 m240 | red post r200 | floor r18 m30 |
|---|---|---|---|---|---|
| before | 174,187,209 | 141,153,174 | 162,142,63 | 141, 59, 66 | 237,237,239 |
| this | 180,192,213 | 116,128,150 | 165,146,67 | 136, 60, 64 | 234,233,236 |

A metal at roughness 8 and a metal at roughness 110 were 19 per cent apart and are now 35, and the
one that moved is the **rough** one — down an eighth, because a lobe that wide no longer reads a
sharp sky. The dielectric moves 3 per cent the other way, losing the share of its diffuse the
specular turns away. Stone is meant to sit still under this change and it does.

### What it costs, and the measurement that could not be made

Every lobe is behind a branch on the material's own bytes, and `coat` holds both the lacquer and the
sheen nibble, so a plain stone surface pays **one integer compare** for both and a second for the
brush. `?lobes=-sheen,-coat,-brush,-metal,-emit` in the page's URL compiles the named lobes out,
which is the control arm for measuring any of them.

**The per-lobe frame cost could not be measured on the machine this was written on, and the control
that says so is in the numbers.** The renderer's own draw, timed with a `readPixels` fence on each
end, fastest of seven runs of six draws, pinned at 640×480, on the salon camera above:

| | |
|---|---|
| all lobes | 424.5 ms |
| …without the lacquer | 393.6 |
| …without the sheen | 416.7 |
| …without the brush | 411.3 |
| …without the emission | 386.5 |
| **…without any of them** | **442.8** |
| all lobes, again | 426.5 |

Compiling every lobe out came back four per cent *slower* than leaving them all in. And on
`mirror_test` — which declares no lacquer, no sheen and no brush at all, so those arms are provably
doing nothing — the two arms simply interleave: 163.1, 156.6, 156.3 with everything on against
168.0, 149.2, 153.8 with everything off. The run-to-run spread of a software rasteriser sharing four
cores with a dozen other browsers is wider than anything being asked about.

**The fault in that table is its design and not only its machine, and fixing the design does give a
bound.** Eight arms measured one after another cannot tell a lobe from a busy minute — which is
exactly what it reported, with "no emission" as the slowest arm of all. Run only the two extreme
arms, *alternated*, six rounds each, and a slow patch of the machine hits both:

| | | best |
|---|---|---|
| all lobes | 383.9  272.8  265.9  431.6  441.2  399.6 | **265.9 ms** |
| none of them | 263.7  263.6  374.4  382.1  398.7  445.7 | **263.6 ms** |

All five lobes together are worth **0.9 per cent** of the fastest frame this box will produce, on
the salon, where every one of them is in use. The spread *within* one arm is 66 per cent, which is
why the best of many is the only statistic worth reading here and why the same script run again gave
`none of them` the slower best — it simply never caught a quiet patch. So: the lobes together cost
under about one per cent of a software frame, and per-lobe attribution is not resolvable on this
machine at any number of repeats. A phone with a real GPU is where that measurement lives.

What can be counted exactly is the work per pixel on a face that carries the lobe: **brush** is one
cross, two dots, an `inversesqrt` and about ten multiplies *replacing* the isotropic distribution's
five; **sheen** is two `pow(x, 5)` and about eight multiplies; **lacquer** is two `pow(x, 5)` and
about twenty-two — a second GGX, a second Smith, its own Fresnel twice, and the mix that dims what
is under it; **metal** is free, being a mix that was already there. Two additions are *not* behind a
branch, because every surface has an environment: the room fallback is one extra `mix`, and the
prefilter is a `max`, a divide, a multiply and a second `mix` — the two `pow` calls it needs were
already there, one of them now with a computed exponent instead of a literal.
<!-- <<< brdf -->

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

<!-- >>> ssr -->
### Reflections, and the offscreen target they needed

Nothing reflected anything. A `mirror` (rough 6, metal 252) and still water both came out the
colour of the sky, because the only thing the surface shader had to put in a specular lobe was
`sky_colour(R)` — a gradient with a sun in it, and no room, no floor and no posts. There are now
**screen-space reflections with a baked probe behind them**, in `web/js/features/ssr.js`.

**The march.** Along the reflection vector, twenty-four steps growing at 1.13 from a stride of four
thousandths of the clip's own size, then four halvings of the interval that straddles the surface.
A step is a **lerp and not a matrix multiply**: the projection is affine in world space, so
`viewProj * (origin + R·t)` is `viewProj·origin + t · viewProj·R`, two matrix multiplies for the
whole ray instead of one per step. Written the obvious way it cost three times as much for the same
picture.

It fades at the screen edge, and it fades where the ray points back at the camera — which is not a
tidying-up but the whole reason the second half exists. **A mirror facing you shows mostly what is
behind you, and that is never on the screen.** Everything a screen-space ray cannot answer falls
through to the baked probes:

```glsl
vec4 probeReflection(vec3 world, vec3 normal, vec3 reflectDir, float roughness);
```

Three things about that call are worth writing down because each of them is silent when it is
wrong. `reflectDir` goes in **uncorrected** — parallax correction happens inside, and doing it
twice bends a reflection off the wall it belongs to. `roughness` is the material's **own byte**,
not the GGX-clamped value the march blurs with. And the return is a `vec4` whose **`.a` is
coverage**: how much of that point's probe neighbourhood was actually baked. Zero does not mean
black, it means nobody looked there, and the answer is then the analytic sky — mixed rather than
branched, so the edge of a probe volume is a fade and not a seam.

Fresnel stays gl.js's own Schlick. `probeFresnel` exists and is deliberately **not** called; two of
them would multiply.

Two things bound what it costs on a phone. A surface rougher than 0.62 never marches, because a
pre-filtered probe says the same thing better and cheaper; and neither does one whose Fresnel is
under 0.055, which is a stone wall seen head-on. Grazing angles still march, because grazing is
where a floor stops being paint. Roughness is a **mip of the capture** rather than a spray of extra
rays: there is no temporal filter here to resolve noise into an image, so a blurred fetch is stable
where a stochastic one would crawl.

**Energy: the reflection replaces the specular lobe.** The diffuse is multiplied by `1 − F`. At a
grazing angle Schlick goes to one, and without that the water in `estate/pavilion` and every
polished floor would gain the whole room on top of everything they already scattered.

**The offscreen target — this is shared plumbing.** The viewer drew straight to the default
framebuffer, which has no readable depth and no readable colour. So `Renderer.captureScene` draws
the sky and the opaque surfaces **again**, into one framebuffer object holding an `RGBA8` colour
texture with a full mip chain and a `DEPTH_COMPONENT24` depth texture, at **half the canvas in each
axis** — a quarter of the pixels, which is the phone budget and is also why marching it is
affordable. It is the viewer's own two passes pointed somewhere else: the same programs, the same
uniforms, the same `drawFaces`. Not a second description of the scene, which is what D204 says the
failure mode is. `u_ssr` is left at zero inside it, and that is what holds a mirror inside a mirror
to one bounce.

**The capture is display-space and a reflection has to be added in radiance.** It is written by the
same fragment shader that writes the screen, so it comes out tone mapped and gamma encoded, and
`ws_capture_radiance` puts it back — the ACES fit in `tonemap` is a ratio of two quadratics and
inverts in closed form. An `RGBA16F` target would avoid that, and needs `EXT_color_buffer_float`,
which a phone may not have; it would buy precision only in the highlights that clipped to white
anyway. Those come back as 7.24, which is bright enough to read as a highlight, and it is the one
place this is approximate.

Anything else wanting a readable depth buffer should take **this** one rather than make a second:
`Ssr.colour`, `Ssr.depth`, `Ssr.width`, `Ssr.height`, the current camera, texture units 2 and 3.
A post chain wants something different — the *final* image at *full* size, with the glass in it —
and should build its own with the exported `makeTarget`.

`?ssr=0` turns the whole thing off, `?ssr=capture` captures and does not march, and `?ssr=full`
captures at full resolution. The three of them are how the cost is split without a rebuild.
<!-- <<< ssr -->
<!-- >>> refract -->
### Glass: what is behind it, bent, and what a metre of it takes out

`web/js/features/refract.js`, and three marked blocks of `web/js/gl.js`. Transparent matter used to
be a blended colour and nothing else, so the four things every clip has always declared about it —
`ior`, `absorb`, `translucent`, `opacity` — reached the file, reached the material texture, and were
read by nobody. Three of them are read now.

**Refraction is screen-space and it is an approximation.** A glass fragment samples the picture of
the scene with no glass in it at an offset, and the offset is the refracted vector carried across
the material's own thickness and projected back to the screen. A material that does not bend gives
an exit point further along the eye ray, which projects to the same pixel — so the offset is purely
the bend. It is what a phone can afford — no rays, no second view — and it is wrong in two ways
worth knowing rather than discovering:

- it can only show what is **on screen**, so a pane at the edge of the frame refracts what is beside
  it in the picture rather than what is beside it in the world (the sample is clamped, so the edge
  smears rather than tiles);
- the picture it samples has **no transparent surface in it**, so glass behind glass shows the
  stone behind both rather than the near pane's own tint.

The third fault a screen-space sample usually has — pulling in something that stands **in front** of
the refractor — is gone, because the capture carries depth: both are window-space depths in the same
projection, so one compare refuses the offset and the pixel straight behind is used instead.

**It does not own a target.** It takes the scene capture `features/ssr.js` already draws — sky and
opaque, this frame's camera, this frame's clip plane, colour and depth, at half the canvas — which
is exactly what belongs behind a pane, and means refraction costs no pass of its own. Where there is
no such capture it falls back to `copyTexSubImage2D` off whatever framebuffer is bound (on the canvas
that resolves the multisampling on the way and costs the picture no antialiasing), and then there is
no depth and the offset clamp of 6% of the screen is all that bounds the artefact. Either way it is
only done for a clip that has a material with both an `ior` and an `opacity`, asked once at load.

**One thing that capture costs, and it is visible.** It is half the canvas in each axis, which is
right for a reflection — read through a mip chain — and is not right for a look straight through
nearly-clear glass: on `glass_test` the wall seen through the clear pane has visibly staircased
edges that the same wall seen beside the pane does not. A full-resolution capture would fix it and
costs what it costs.

**And the capture is display-space, which changes where the tint goes on.** It is tonemapped and
gamma-encoded, because `RGBA16F` needs `EXT_color_buffer_float` and a phone may not have it. That is
fine for this, but transmittance attenuates *radiance*: the encoding has to come off before
Beer-Lambert goes on and back on afterwards, or a stained window over a sunlit wall comes out far
too saturated, because ACES has already compressed that wall towards white. `refract_scene_radiance`
inverts both in closed form — the same closed form as `ws_capture_radiance` in `ssr.js`, and the two
should become one function. The only cost of an 8-bit capture here is that a very bright background
seen through strongly absorbing glass can band; nothing in the facility does that.

**Absorption is not an approximation, and it is the half that is still owed a thickness field.**
`exp(-absorb * path)` over a real path length, in the game's own units — the byte is sixteenths per
metre, exactly what `shaders/node.glsl:node_medium_absorb` reads — and the path is the thickness
crossed at the **refracted** angle, so a slanted look through a pane is deeper in colour than a
square one. That is what makes a stained window a coloured **volume** rather than a coloured
surface, which `clips/facility/_contract.clip` says is the whole point of the three coloured
glasses. **The thickness is currently a constant** (0.12 m, the facility's own glazing). The
material-volume bake (fourcc `THCK`, `tools/bake/matvol.hpp` and `web/js/features/matvol.js`) is
what makes it real, and `refract_thickness` in `refract.js` is one line and one assumed signature —
`float matvol_thickness(vec3 world, vec3 direction)`, metres of matter along the direction. Until
it lands, every pane and every body of water in the viewer is 12 cm thick, so the *angle* half of
the coloured volume is demonstrated and the *depth* half is not.

**Translucency reads the light on the far side, which is the arrangement it exists for.** The one
line that was here added a wrap term scaled by `sunVisible` — the sun's visibility **in front** of
the surface, which is nought precisely when the sun is behind it. It now takes a second fetch of
the same baked light volume at a point **behind** the surface, and carries it through the thickness
with the game's own depth: six voxels at 32 to the metre, squared in the byte. Marble at 110 reaches
under a voxel and stays stone; alabaster at 210 reaches four and a thin panel of it lights up. It
is the sun and the sky that come through, because the sun and the sky are what the light grid holds
— **a lamp behind alabaster does not glow through it**, and cannot until something bakes local
light into that volume.
<!-- <<< refract -->

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
has nothing left to say.

<!-- // >>> shapeshade -->
### It is shaded with the clip's own materials

Every shape used to be one flat grey — `vec3(0.62, 0.60, 0.56)`, a fixed sun term of 0.7, no light
grid at all. That answers *what shape is this*. It cannot answer *will it be that colour in game*,
which is what the view was asked next. So the hit point asks which material it is and is shaded
with that record, and the rule the answer has to satisfy is: **switching between ◉ and the voxel
view changes the resolution and nothing else.**

Which means the same everything. `web/js/features/shapeshade.js` holds one GLSL chunk, spliced into
`SHAPE_FRAGMENT`, and every constant in it is the surface shader's own: the same four RGBA8 rows of
the `VisualRecord` — colour, opacity, roughness, metallic, index of refraction, emission and its
RGB565 tint, Beer-Lambert absorption, translucency, the brush-grain axis, clearcoat and sheen, all
of it, because §2 says nothing is quantised on the way out precisely so a viewer can show what
these materials are — the same Cook-Torrance lobe, the same sun and sky, the same ACES curve, the
same exposure, and the same light grid.

**Measured, with the material forced the same in both views.** `?shapemat=N` paints every shape in
the ◉ view with material N and nothing else, which is the only way to ask the question honestly:
put the same material on the same surface in both views and the pixels either agree or they do not.
Same camera, same pixel, sRGB out of the framebuffer:

| | voxel | ◉ |
|---|---|---|
| `glass_test` slab, top, material 0 | 238 237 235 | 238 237 235 |
| `glass_test` slab, rim, material 0 | 180 177 174 | 181 178 175 |
| `mirror_test` slab, top, material 6 | 237 236 235 | 237 236 235 |
| `mirror_test` slab, rim, material 6 | 183 181 180 | 184 182 181 |
| `mirror_test` slab, top, material 8 (glossy, part metal) | 237 237 239 | 238 238 239 |

**The bias, and it is the third time this trap has been walked into in this viewer.** A lattice
point buried in stone has no light of its own and is filled in from its brightest neighbour at half
strength, so a trilinear fetch *at* a surface blends the air in front with the stone behind and
comes out dark. The surface shader answers it with one whole light cell along the normal; the slice
cap answers it with half a voxel out of the cut; both were black first.

This view has that trap **and one more**, because its hit points are not the voxels'. The march
lands on the true analytic surface and the grid was cast against the voxelised copy, whose surface
is up to a voxel away and may be on either side — so a hit point can be *inside* the matter the grid
knows about, which is the darkest place in it. The bias is therefore `lightCell + half a voxel`:
0.4 m as the surface shader uses, plus the largest disagreement there can be between the two
surfaces. `sampler`, same pixel, the naive fetch against it:

| | bias 0 | bias `lightCell` + ½ voxel |
|---|---|---|
| a vertical box face | 131 131 133 | 203 200 194 |
| sunlit ground | 222 220 217 | 241 240 238 |
| inside the doorway | 68 76 92 | 68 77 93 |

The vertical face is the whole of it: at bias 0 it reads the wall it is the surface of and loses
half its light, and the doorway shows the extra bias is not simply brightening everything — what is
genuinely dark stays dark.

**The rim light is kept, at 0.18 where it was 0.5.** It is not light, the voxel view does not have
it, and anything the two views do not share is a thing that makes them disagree — but this view has
no ambient occlusion of any kind, because there are no voxel corners to take it from, so two walls
of the same marble meeting at a right angle have nothing whatever between them and read as one
lump. At 0.5 it was a fog that lifted the whole silhouette; at 0.18 it is an edge. Its albedo is
the shaded colour rather than a constant, so it brightens what is there instead of washing
everything towards grey.

**Glass is the one place one pass cannot reach.** The pane's own colour, its opacity, its
absorption tint and the glancing-angle Fresnel are the surface shader's lines unchanged, so a pane
is the same colour and the same opacity in both views. What differs is what is *behind* it: the
voxel view composites over the stone actually there, and this view has no second sorted pass to do
that with — sorting one instanced draw of ray-marched boxes would double the march on the biggest
clips — so what shows through is the sky along the refracted direction. Over open ground the two
are the same picture; over a wall the ◉ view shows sky where the voxel view shows the wall.

**Which material is at a hit point is `material_at(p, n)`**, from `web/js/features/paint.js` — the
clip's paint rules evaluated at a world point. Until that lands there is a stub in
`shapeshade.js` that hashes each shape's own placement into the material table, so several
materials are on screen and every field of a real record is exercised by something; the console
says which of the two is live on every load, because "the colours are wrong" and "the colours are a
hash" look identical in a screenshot.
<!-- // <<< shapeshade -->

<!-- >>> paintstack -->
### It is painted, and the paint is the clip's own stack of rules

Every shape used to be the same flat grey, and it had to be: **a shape has no material.** Colour in
this project is not a property of a shape, it is the result of a stack of rules evaluated at a
point — `20-clip-forge.md` §2, and the thing that lets one wall be stone, except where it is damp,
except where the damp has moss, except in the mortar: one shape and four rules rather than four
shapes.

So the marched hit point is given to that same stack. `web/js/features/paint.js` runs it, on
information that is strictly better than the sampler's: the point is exact rather than a voxel
centre, and the normal is the analytic gradient of the resolved distance rather than a six-tap
difference at voxel spacing.

    material = the first coat
    for each rule in file order:
        v = field_eval(rule.node, p)
        if above <= v <= below and the facing test passes:
            material = rule.material     <-- the LAST match wins

**Last match wins**, because the stack paints each rule over the last. Reading it as first-match
gives a picture that looks entirely plausible and is wrong everywhere two rules overlap, which on
this building is most places — the weathering coats in `surface.clip` are laid over everything.

Three things about the numbers, each of which is a wrong picture rather than an error:

- **`above` is the LOW end of the band and `below` is the HIGH end**, which reads backwards until
  you remember they are written about the field's *value*. For a rule keyed on a shape that value
  is a signed distance, so `below=0.02` means "inside that shape, or within 2 cm outside it" —
  `clips/facility/BRIEF.md` rule 5 is three pages on why it is 0.02 and not 0, and why anything a
  transform placed needs 0.035.
- **`facingAxis` is −1 when the rule does not ask for a normal**, and the engine's own `PaintRule`
  writes 3 for that. A shader testing `axis < 3` reads −1 as "yes, ask about axis −1"; and
  `int(-1.0 + 0.5)` truncates to 0, which is "ask about x" for every rule in the clip.
- **The facing test is not symmetric.** The sign of the threshold is the direction: `at=0.6` keeps
  `n·axis >= 0.6`, `at=-0.6` keeps `n·axis <= -0.6`. An `abs()` there paints the ceiling with the
  floor's moss.

**Nothing matching is not "no colour".** `sample.cpp` ends with "a cell with matter in it and no
rule that matched is still matter" and hands back rule 0's material whether or not rule 0's own test
would have passed. So the walk stops at rule 1 and returns rule 0's material — which is also one
field evaluation saved at every pixel of every clip.

### What it costs, and the three things that stop it costing that

A facility fragment carries **348 rules**, and `facility/terrace` exports 4,829 field nodes at depth
54. A hundred-odd field walks per pixel on a phone is not a slow feature, it is not a feature.

- **A box per rule.** `PANT` ships `lo`/`hi` with every rule and a `BOXED` flag saying they mean
  something. Six floats and a compare, before any field walk. The flag is honoured rather than
  second-guessed: deriving a tighter box here than the exporter's would reject a rule that should
  have fired, and the only sign of it is a building that quietly lost its paint.
- **Walk it backwards and stop at the first match.** Since the last match wins, the first rule that
  matches walking from the end *is* the answer and everything before it is dead.
- **A budget of 32 field walks a pixel**, and then the first coat. `?paint=cap` paints magenta
  wherever it bit, because a silent cap reads as "it worked".

Measured on `sampler`, 24,534 shaded pixels, counted rather than timed — a millisecond under the
software rasteriser this was measured on varied by a factor of three between two runs of the same
arm, and a field walk per pixel does not vary at all:

| the stack | field walks per shaded pixel |
|---|---|
| the sampler's own four rules, two of them boxed | 2.0 |
| 133 rules, each boxed to a slice of the clip | 0.55 |
| 133 rules, none boxed | 32 — the budget, at every pixel |

The last row is what the box test is for: without it the walk hits its cap everywhere, which means
it is also handing back the first coat everywhere. `?paint=off` compiles the stack out entirely and
is the other arm of any measurement of it; `?paint=evals` is where that table comes from.

<!-- <<< paintstack -->
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

// >>> fieldeval
## 4c. Running the clip's own fields in the browser

A shape has no material. Colour comes from a stack of paint rules, and a rule is a **field**, a range
that field must fall in, and optionally a direction the surface must face. So the ◉ view can only
stop being flat grey if something in the shader can answer *what is this field, here* at the point
the ray hit. `web/js/features/field.js` is that: `float field_eval(uint node, vec3 p)`, the same
function `src/forge/field.cpp`'s `eval` is, over the `FLDG` chunk.

**Recursion is an explicit stack, and it is a transliteration of `Field::mirror_eval`** — the
non-recursive twin already in `field.cpp`, written there precisely so the shader would be a
transliteration of something already proved. One frame per node on the way down; a `step` counter
over SAMPLE POINTS rather than over children, which is what lets `curvature` ask its child seven
times, `occlusion` fourteen, `facing` six and `repeat` up to eight.

The obvious alternative — evaluate the array forward into a scratch value per node, which the export
order allows because a child index is always below its parent's — is not available here: that is one
float per node of private memory, and `facility/terrace` is 4,829 nodes. The stack is bounded by the
graph's **depth** instead, which for the same clip is 54.

**The depth is compiled in**, taken from the graph the page loaded rather than guessed: `sampler` is
depth 4 and gets a stack of 4. The ceiling is 64. Past it the walk is **refused** —
`field_eval_ok` returns false, which a rule reads as no match — rather than truncated, because
"I could not" and "the answer is nought" must never be the same reply.

### It is checked numerically, because a screenshot cannot check noise

A rule keyed on `above=0.55` paints somewhere else entirely if the hash, the octave seeds or the
octave weights differ by anything, and the picture stays perfectly plausible. So every op is held
against `Field::eval` over 1024 points in a clip-sized box and 1024 thirty metres out, with the
node arguments narrowed to `f32` on both sides so the comparison measures the shader's arithmetic
rather than the exporter's rounding. Worst absolute difference:

| | near | at 30 m | cells crossed |
|---|---|---|---|
| `noise` | 2.6e-6 | 4.3e-5 | 0 |
| `fbm` | 3.8e-6 | 4.9e-5 | 0 |
| `ridged` | 1.6e-6 | 2.8e-5 | 0 |
| `rasp` | 9.6e-6 | 1.4e-4 | 0 |
| `cells` | 2.3e-7 | 3.4e-6 | 0 |
| `cell_edge` | 2.5e-7 | 3.0e-6 | 0 |
| every solid | 6.5e-7 | 8.3e-6 | — |

Those are values in [-1, 1], and the column that matters is the last one: **not one sample of twelve
thousand landed in a different noise cell.** A cell boundary crossed is not a nearer answer, it is a
different number, and it is the only way this could have been wrong in a way that still looked right.

**Everything with a sine in it is the driver's error, not the port's.** `rotate`, `around`, `twist`,
`bend`, `sine` and `waves` differ by up to 1.9e-4 — and a control shader that computes `sin(x)` with
no field in it at all is **1.894e-4** from the double-precision answer on the same renderer, which is
the same number. GLSL ES 3.00 promises sin and cos only to 2^-11. Under a `rotate` at thirty metres
that becomes a centimetre of position, which no paint rule can see and a march would.

### What it costs, and the one shape of rule that is unaffordable

One evaluation per fragment per rule, at the marched **hit point** — not per march step. Measured on
the software rasteriser the check runs on, cost is linear in node visits at about **3 µs a visit**
(one leaf 3.1 µs, an eight-node union 25 µs, a twenty-nine-deep chain 80 µs); real hardware is
two orders faster, but the shape holds. So a rule keyed on a pattern or on a wall is nothing, and
**a rule keyed on a whole building's solid is not affordable at any resolution** — 4,829 visits per
fragment. `curvature`, `occlusion` and `facing` are ported and correct (worst 9.9e-6, 5.1e-8 and
1.2e-5) but multiply their subtree by 7, 14 and 6, so a fragment with 22 rules reaching them is the
case to watch.

// <<< fieldeval
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

**Sixteen lamps a draw**, and sixteen is a **choice, not yet a measurement**: it is the largest
uniform array that fits comfortably inside the ES 3.0 minimum of 224 fragment uniform vectors
alongside what this shader already holds. Nobody has
run it against eight or against thirty-two on a phone. Ranked by what each delivers at the camera —
intensity over distance squared, the same rank `src/world/light_list.cpp` uses for the game's own
list — and the baker keeps at most 256 in the file. The count, how many a draw is shading, and whether the cap bit are printed
per clip by the baker and logged once per clip by the viewer, and they are on `renderer.lights`.
It bites on both clips looked at: many_lamps has 36 and `facility/fittings` has 25.

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

Both baked with `--budget 8000000`, which puts many_lamps at 16 voxels to the metre and the
fragment at 8; CI bakes at 700 M and `--part-metre 16`, so the facility figures there will be larger.

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

**The frame cost has not been measured on a GPU.** Everything here was rendered by SwiftShader, a
software rasteriser, on a box shared with fourteen other jobs. Two paired runs of many_lamps at
900×700 with the camera a metre from a lit wall, `?nolamps` against the shipped build:

| | lamps off | lamps on (16) |
|---|---|---|
| run 1 | 171 ms | 677 ms |
| run 2 | 221 ms | 1535 ms |

Four to seven times, and both halves of that are untrustworthy for a phone: a software rasteriser
has no texture-sampling hardware, so the sixteen bilinear atlas fetches cost far more of the frame
there than they would on any GPU, and the run-to-run spread is larger than the effect being
measured. What the numbers do establish is that the term is **not free and scales with the cap** —
whoever puts this on a real device should measure eight against sixteen against thirty-two before
believing the cap is right.

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

<!-- >>> matvol -->
## 6. What the stone inside a wall is made of

A `.wsc` carried the exposed surface and a one-bit occupancy grid, and **nothing in it could say
what the matter at a point INSIDE a wall was**. Two things were wrong for exactly that reason and
they are the same missing thing seen from two ends:

- **the slice cap was one colour for the whole clip** — the commonest opaque material by area, a
  stand-in — so cutting through the rotunda's porphyry-and-lapis floor gave the building's
  limestone;
- **refraction and translucency had no distance.** `alabaster` is translucent 210, `porcelain` 90,
  and the three coloured glasses carry a Beer-Lambert `absorb` **per metre**, which is a number
  with nothing to multiply.

So the baker writes two more things, on the occupancy grid's own 12.5 cm cells:

| | |
|---|---|
| **`MVOL`** | one byte a cell, the material. 0 is air. |
| **`THCK`** | one byte a cell, how thick the matter through it is, in the clip's own voxels — the thinnest run over the three axes, so a pane comes out a pane and a wall comes out a wall. |

### The size is the whole design, and it is measured rather than feared

The facility's occupancy grid at 16 voxels to the metre is **272 x 168 x 200 = 9.14 million
cells**, so the two channels DENSE are **17.4 MB** against the 8.6 MB its quads cost. That is not
going onto a phone and it is not coming down a phone's network.

It is stored **sparse, by 4x4x4 block**. A building is mostly air and mostly uniform: a block
inside a wall is one stone at one thickness and a block of sky is air at zero, so only the blocks a
surface actually passes through cost anything. A uniform block is one word in a directory, holding
both its bytes; the rest are pages of 64 cells. **Four and not eight is measured** — on
`facility/rotunda` the same volume is 0.65 MB at four and 0.95 MB at eight, and worse again at
sixteen.

| | cells | dense | packed | file, gzipped |
|---|---|---|---|---|
| `facility` at 16/m | 9,139,200 | 17.43 MB | **5.73 MB** (32.9%) | 2.64 -> 3.14 MB |
| `facility-rotunda` at 16/m | 948,693 | 1.81 MB | **0.65 MB** (36.1%) | 278 -> 335 KB |
| `sampler` at 16/m | 147,456 | 0.28 MB | **0.09 MB** (33.2%) | 120 -> 133 KB |
| `glass_test` at 16/m | 331,776 | 0.63 MB | **0.08 MB** (11.9%) | 4.3 -> 6.4 KB |

**It is affordable, and the number that says so is the gzipped one**: the site serves `.wsc.gz`,
and the building's download goes from 2.64 MB to 3.14 MB — a fifth more for the largest clip in the
repository, and one part in twenty of a photograph for the small ones. Roughly half of it is
`THCK`: on the facility, `MVOL` is 3.14 MB of the 5.73 and `THCK` the other 2.59.

**And what it costs the card is what it costs the wire**, which is the actual reason it is a block
index rather than a run length. The packed form IS the form the GPU reads — a block directory as
one `R32UI` volume and the pages as one `RG8UI` atlas — so nothing is decompressed on load and
nothing is held twice. A run-length encoding would have been smaller on the wire and 17 MB in VRAM.

### More than 255 materials

A byte indexes 255 of them and air. Nothing is near it — **the whole facility at 16 to the metre
interns 58 distinct visuals**, because the mesher interns by what a material LOOKS like and not by
its type, so the 203 names in `_contract.clip` collapse hard. But a clip will pass it one day, and
a silent truncation is the failure this repository keeps writing down.

So the volume carries **its own palette**: the 255 materials with the most cells, and everything
past that mapped to the nearest kept one by colour, opacity and translucency. The baker
`WS_LOG_WARN`s with how many were dropped and how many cells were repainted, and prints it in the
per-clip line. A clip over the line loses its rarest interior stone to one that looks like it and
says so; it does not lose the volume.

### A voxel the air touches is worth two

A cell is 12.5 cm and nearly every colour in this building is a coat **two centimetres deep** —
porphyry in the rotunda's floor bands, verde in its niche linings, lapis in the halls. A straight
majority over the eight voxels in a cell erases every one of them and paints the cell the
structural stone behind, so the cap would come out limestone right beside a quad drawn porphyry.
The volume and the mesh are drawn touching each other and they have to agree, so a voxel with air
on any side counts double.

### What a shader asks it

Paste `MATVOL_GLSL` from `web/js/features/matvol.js` and call `Matvol.bind(uniforms, unit)`, which
takes three consecutive texture units:

```glsl
int   ws_material_at(vec3 world);    // the CLIP's material index -- what the material texture is
                                     // addressed by -- or -1 for air and for a clip with no volume
float ws_thickness_at(vec3 world);   // metres of matter through that point, 0 where there is none
uvec2 ws_matvol_cell(vec3 world);    // both raw: x the volume's own byte, y thickness in voxels
```

A uniform block costs one `texelFetch` and only a block with a surface in it costs two. A clip
baked before this existed has no `MVOL`, the dims come out zero, and every call answers "no
matter" — one code path, and the cap falls back to its one colour.

### The chunk directory, which is why this did not move anything

Version 1's header was 192 bytes with nothing spare, and version 2 exists because adding one block
to it moved every offset in the file. **Version 3 spends the last spare word on an indirection
instead of on a block**: `u32` at 200 says where a directory of `(tag, offset, size, spare)` is and
`u32` at 204 says how many entries. Everything added from here on is appended and listed there,
nothing already in the file moves, and a viewer that has never heard of a tag steps over it rather
than mis-reading the bytes behind it.
<!-- <<< matvol -->
