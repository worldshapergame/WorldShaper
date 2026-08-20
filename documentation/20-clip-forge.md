# 20. The clip forge

A clip is not a box of voxels somebody saved. It is a **description**, and the voxels are what
you get when you evaluate it. That one decision is what lets the same clip be stamped at a
different size, re-cut at a different resolution, and have its numbers moved while you watch
without any of it being rebuilt.

This is the pipeline that turns such a description into matter, and the instruments that say
whether what came out is what was meant.

## 1. One mechanism: the field

Every shape and every pattern is the same thing — a function from a point in space to a number.
What the number *means* is decided by whoever reads it:

| read as | meaning |
|---|---|
| a distance | negative inside, positive outside, magnitude ≈ how far the surface is |
| an amount | a wave, a grain, a checker — read for its value, not its sign |

One mechanism for both is the point, not a saving. It means a wave can be carved into a wall by
*adding* it to the wall's distance; that the same wave can then decide which voxels are moss; and
that neither needs to know anything about the other. A pattern is a shape whose sign nobody
looked at. A shape is a pattern somebody took the sign of.

Signed distance also makes the operations exact and total:

- union is a minimum, intersection a maximum, carving a maximum against a negation
- a shell is `|d| - t`; a rounded edge is `d - r`
- none of them need special cases, and none can produce a shape the next operation cannot handle

That last property is what makes "any shape, any pattern" a claim rather than a wish.

## 2. The file *is* the clip

`clips/sampler.clip` is the worked example — one of everything, laid out to be looked at.

```
metre 32
bounds -6 0 -3   6 4 3
param  rise 0.18

material stone rgb=124,120,112 rough=210

let plinth = box -6 0 -3   6 0.3 3 round=0.04
let grain  = fbm size=0.10 octaves=4 seed=3
let slab   = displace { box -2 2 -0.4  2 3 0.4  grain } amount=0.04
let all    = union { plinth slab }

paint stone
paint moss where=grain above=0.55 facing=y at=0.6

solid all
```

Nothing refers to anything outside the file, which is what makes a clip a thing you can send
someone. A name can be re-bound — `let all = displace { all grain }` reads as a pipeline, and is
the form most authoring actually takes.

### Vocabulary

**Solids** `sphere box cylinder capsule torus cone plane ellipsoid prism tetra cube octa dodeca
icosa wedge stairs`

`prism` covers every regular polygon from a triangle up, by its number of sides. The five
Platonic solids are each an intersection of their face planes, sized by circumradius — the
distance to a *vertex*, which is the size a person means.

**Combining** `union difference intersection` — each takes `smooth=<metres>` for a fillet, which
is a real distance rather than a unitless knob, because "join these with a five centimetre
fillet" is a thing an author knows they want.

**Moving the point** `translate rotate scale mirror repeat around`

**Changing the answer** `shell round offset displace twist bend blend`

**Patterns** `sine waves noise fbm ridged rasp cells checker stripes bricks axis distance
constant`

**Arithmetic** `add multiply min max remap abs negate step smoothstep clamp power`

### Painting is a stack of rules

The obvious design gives every shape a material, and it falls apart on the first real surface: a
wall is stone, except where it is damp, except where the damp has moss, except in the mortar.
Those are not four shapes. They are one shape and four rules.

So a rule is a field, a range it must fall in, and optionally a direction the surface must face.
Rules apply in order, each painting over the last. `paint stone` is the undercoat.

## 3. Live parameters, and why they are slots

A number typed into a node is baked into it, and changing one would mean re-parsing the file and
rebuilding the graph. So numbers meant to move are not typed into nodes — they are **slots**, and
a node refers to a slot. Turning a dial writes one double and re-evaluates. Nothing is rebuilt,
nothing is allocated, and a visual editor's displayed graph does not change under it.

Today `param` declares the slot and the parser reads its value when the shape is built; wiring
the slot through to the shape's argument so that moving it re-shapes without re-parsing is the
next piece of work, and the representation is already the right shape for it.

## 4. Speed, and where it is going

Two optimisations, both of which had to be proved exact rather than nearly exact.

**Skipping empty space.** The field answers with a distance, so a point half a metre from
anything means the next sixteen voxels are empty too. Displacement breaks the bound — a displaced
surface can be nearer than the field admits — so `skip_slack()` adds up what displacement can
hide and takes it off every jump. When it cannot bound it, no jump is ever taken.

**Bounding boxes on unions.** A union of thirty parts costs thirty evaluations at every voxel,
and at nearly every voxel twenty-nine of them are answering about something metres away. With a
box round each, the union asks how far the point is from the *box* and only evaluates the child
if that could beat what it already has.

Both were wrong first and measurably so:

- the box test skipped children *inside* a solid, where the box distance is zero and the child's
  distance is negative. Nothing appeared or vanished — the sign was always right — but the
  magnitude moved, and magnitude is what surface normals are made of, so four hundred voxels of
  moss landed in the wrong place.
- the jump skipped the *mask* as well as the evaluation, leaving holes in "which cells belong to
  this clip". Invisible in the voxels. Instantly visible in a printed slice.

The sampler is parallel across z slabs. The real answer for live re-voxelisation is the GPU:
nodes are plain data of a fixed size with no pointers and evaluation is a switch with a shallow
stack, which is a shape that transliterates to a compute shader without changing.

## 5. Instruments

A screenshot says a room looks plausible. It does not say the doorway is 2.05 m when you meant
2.00, that the two halves differ by three voxels, or that the material meant for the trim is on
none of the surface because its rule never fired. Those errors survive looking.

```
WorldShaper.exe --clip-file clips/sampler.clip --clip-symmetry --clip-slice 2,-1,3
```

| measure | what it catches |
|---|---|
| extent, in voxels **and** metres | the conversion, where most mistakes live |
| volume, m³ and litres | a shape that is not the size it claims |
| exposed faces, m² | **texture** — the only number that notices roughness |
| roughness (faces per voxel) | a displacement that has broken a surface into gravel |
| centroid as a fraction of the box | a lean, or a symmetry that is not one |
| material shares | a paint rule that never fired |
| `span_along` / `gap_along` | doorway widths, wall thickness, head height |
| `mirror_mismatch` | how many cells break a symmetry, not merely whether one is broken |
| `slice_text` | a picture, when a number will not do and a screenshot is too much |

The slice is worth more than it looks. It found the mask bug in §4 in one glance, and no
measurement of the matter could have.

With `--screenshot` the same `--clip-file` stamps the clip into the world instead, so the
ordinary camera and screenshot machinery can look at it.

## 6. Weathering follows the shape

Weathering is not a texture laid over a surface. It is a consequence of the surface's own
geometry: sand piles where a wall meets the ground and blows off an exposed arris, moss grows
where a corner stays damp, soot collects under an overhang and washes off a sill, cracks open
across a face and branch where they meet. Paste a weathering texture on and it looks right in one
screenshot and wrong the moment the shape changes.

So three questions are asked of the geometry, and everything else follows from them:

| quantity | what it is | what it drives |
|---|---|---|
| `curvature` | the field's Laplacian at a radius: **+** on an arris, **−** in a hollow | wear on edges, collection in corners |
| `occlusion` | how much of a small sphere is solid: 0 open, 1 buried | damp, soot, drift — everything that needs shelter |
| `facing` | the surface normal along an axis | up collects, down stays dry and takes soot |

Five kinds, each an amount from 0 to 1, composing in any order:

```
weather desert    0.4 scale=1.0 level=0
weather overgrown 0.6
weather cracks    0.5 scale=0.8 seed=3
weather burnt     0.3
weather sea       0.5 level=1.2      # the tide line
```

Each expands into **both** a deformation of the solid and coats of paint that follow the same
geometry — cracks carve fissures and darken inside them, growth swells the surface and greens it,
barnacles stand proud below the tide while salt bleaches above it. Each brings its own materials
unless the file already declares one of that name, so `weather overgrown 0.6` is a one-liner.

A crack is worth singling out. It is not drawn as a line: it is the seam between two scattered
points that are equally near — `cell_edge` — which branches, meets itself and closes loops for
free, because seams do.

Signs are easy to get backwards here. Displacement moves a surface by *adding* to how far away it
says it is, so a positive value eats into the solid and a negative one grows it. Cracks were
negated at first and stood proud of the face like veins, and the block came out bigger than it
started.

## 7. No two voxels alike

A real surface has no two square centimetres the same. Photograph a concrete wall or scan a
weathered stone and every patch differs from every other in colour and in how it catches the
light — not by much, and never by nothing. A clip built from a handful of materials has the
opposite property: millions of voxels sharing a dozen records. **That repetition is why a voxel
wall reads as a voxel wall however good the lighting is.**

```
variation colour=0.05 rough=0.10 seed=4 [budget=1000000] [by=<field>]
```

Every voxel gets its own perturbation of its material, hashed from where it is — so the same clip
always builds identically, which is what makes a clip shareable and cacheable. `by` scales it by
any field, so a weathered face can be more varied than a sheltered one.

Measured rather than asserted: a weathered block of 1.24 M voxels yields **585,559 distinct
records, largest identical group 45**. The report always says how close to unique a clip got.

Two honest limits:

- Literal uniqueness costs one visual record per voxel. At the facility's scale that is millions
  of records for a difference no eye can resolve.
- The renderer's type table is a fixed GPU buffer. It held 262,144 records, and the first
  properly varied block asked for 595,846 and took the renderer down with an assertion. It now
  holds 2,097,152 — 48 MB beside a 460 MB payload buffer — and `budget` stops the variation pass
  minting more than it can take. Past the budget records are reused, so the ceiling costs quality
  and never correctness.

Players can turn it off. Omit `variation` and every voxel of a material is identical, which is
faster to build and smaller to store.

## 8. What this is for

A clip is the unit of authored content in this game, and it will nearly always be procedural — a
tree that is a different tree each time, a wall that fits the gap it is put in. The test facility
is the exception that proves the format: a fixed room, but still one file, still procedural,
still measurable.

Players will write these, or wire them together visually. The node array is the real
representation; text and wires are two views of it, and either can be saved as either.

**One correction to that last sentence, made when the visual view was built** (D745). It is true of
what a clip *means* and it is not what an editor can edit. By the time a file has become a `Field`,
one `let` is a dozen nodes, every name has gone, the numbers are folded together and the comments
never existed — so a visual edit made against the node array could not be written back without
rewriting the whole file, and `23-shell-and-libraries.md` §5c forbids exactly that: *a round trip
does not reformat what you wrote.* What the two views actually share is the **document** — the
statements the author wrote, the names they bound and the numbers as they spelled them, each with
the line and column it sits at. `src/game/clip_graph.*` is that reader, and it is what the editor's
wires are drawn from. The statements lower to field nodes; the field nodes do not lift back.

It **writes** as well (D757): joining two boxes in the visual view puts a name in a `{ }`, cutting a
wire takes one out, and the palette writes a whole statement. Every one of them is surgery on the
author's own bytes with everything else in the file left alone, which is the round-trip rule
`23-shell-and-libraries.md` §5c states and the reason the reader records a line and a column for
every number, every name and every brace.
