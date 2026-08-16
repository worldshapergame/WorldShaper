# The facility — a brief for everyone building it

A neoclassical public building, Ionic order, built as one clip out of many files so that many
people can work on it at once. This is what you are building, how your piece fits, and the rules
that keep twenty pieces looking like one building.

## It is two things at once

It is a building. It is also the only scene this engine is ever judged against, and the second
is the reason it exists. Every fragment carries a share of putting the engine under load, named
in its own brief: thin matter that breaks a sampler, a hollow only light can reach through a
small opening, a material nothing else uses. A part that looks beautiful and exercises nothing
has done half the work; so has a part that stresses the sampler and looks like scaffolding.

And it has to be able to GROW. New rooms and features will be added to test things the engine
cannot do yet - that is why it is written as separate fragments rather than one file. See
_TEMPLATE.clip: adding a room is one new file and three lines in the manifest. Do not write
anything another fragment has to know about, and leave your part self-contained enough that
somebody can add a wing beside it next month without reading your file.

## What it is

A hexastyle Ionic portico facing south, raised on a podium and reached by a broad flight of
steps; a main block behind it with a rusticated ground storey and a piano nobile of aediculed
windows; wings east and west; a domed rotunda on the centre line; a balustraded parapet hiding
the roof. Inside: a vestibule, the rotunda under the dome with niches and a coffered ceiling,
side halls, and a stair to the upper floor.

It is also the engine's test piece. It has to be **walkable** — real stairs, real doors, real
head height — and it has to exercise the renderer: polished marble and rough stone, bronze and
gilt, glass, water, emissive fittings, deep shadow under the portico and light falling through
an oculus.

## The rules

**1. Read `_contract.clip` and take every dimension from it.** The order is a module: D = 0.90 m
is the column diameter, M = 0.45 m is half of it, and everything in the building is a multiple or
a simple fraction of those. If you find yourself typing a number that is not a multiple of 0.45,
stop and work out what it should be. This is the single most important rule; it is the difference
between a classical building and a pile of classical-looking parts.

**2. Your file defines exactly one part, named `part_<yours>`.** Everything else you bind must be
prefixed with your own name, so nothing collides:

```
let portico_shaft   = ...
let portico_capital = ...
let part_portico    = union { portico_shaft portico_capital ... }
```

**3. Symmetry is not optional — OUTSIDE.** This building is symmetric about x = 0. Build one side
and `mirror` it, or place things at ±x, but never let the two halves drift apart. Use `mirror { x }
axis=x` — it folds the coordinate, so a shape built on the +x side appears on both.

*Outside*, and the qualifier was added on 2026-08-16 when the interiors stopped being able to obey
it. This rule was written when the only rooms were a vestibule, a rotunda and two halls, and all
four are symmetric because they are the same room twice or a room on the axis. The state rooms
added since are not: a mirror salon in the south-east corner and an oval chapel in the south-west,
a ballroom over one and a library over the other. Mirroring them would mean two salons and no
chapel, which is not a building anybody has ever built.

So the rule now reads: **the shell is symmetric and the plan of the rooms is symmetric; what is
inside a room is not required to be.** A wing, a window, a pilaster, a cornice, a stair and the
walls that divide the floor are all the shell. The room's own contents are not. Real buildings of
this century are exactly this — a symmetrical envelope with a chapel on one side and a library on
the other — and the eye that catches a part sized by taste rather than by the module does not
catch a chapel where it expected a salon, because it cannot see both at once.

Nothing else about the rule changes. If your part is any of the shell, mirror it.

**4. Everything must touch something.** No part may float. If your piece sits on the podium, it
must overlap the podium by at least a voxel (0.03 m); do not butt it exactly, because a surface
displaced by weathering can open a hairline gap. Overlap by 0.05 m and the join is certain.

**5. Paint only your own geometry, and write `below=0.02`, never `below=0` — and `below=0.035`
for anything a transform put where it is.** Every paint line must be
`paint <material> where=<one of your own shapes> below=0.02`, or `below=0.035` when the shape is
placed by `translate`, `rotate`, `around` or `repeat`.

Two rules in one, and both were learned the hard way.

*Only your own shapes*, because painting by a bare coordinate paints a third of the building the
wrong colour. That happened.

*`below=0.02` and not `below=0`*, because the manifest displaces the whole building by 12 mm of
grain at the very end. A voxel that only exists because the grain pushed the surface outward sits
**outside** the shape its rule names, so `below=0` misses it, no rule matches, and it falls back
to the first coat — leaving a rind of pale limestone-coloured voxels over every outward-facing
surface of your part. It looks like weathering and it is not; it is your paint failing to reach
the surface of your own geometry. Displacement moves a surface; it does not move which part a
voxel belongs to, and the test has to say so. 0.02 is comfortably past the 12 mm.

***0.035 and not 0.02 for anything you placed with a transform***, and this is new — found on
2026-08-16 while the ballroom was being built, and it is the reason that rule now names two
numbers instead of one.

The 12 mm of grain is not the only thing that pushes a voxel outside the shape its rule names. A
voxel is decided **solid by coverage** — any part of the cell in the shape — and **painted by a
rule tested at the cell's centre**. For a shape sitting square on the grid those two nearly agree.
For a shape put where it is by `translate`, `rotate`, `around` or `repeat` they do not, because the
surface no longer lies anywhere near a voxel boundary, and **3 to 16 per cent of that shape's own
solid voxels fall outside its own test.**

They do not vanish and they do not warn. They fall through to the base coat, so a gilt trellis on a
white ceiling comes back with a pale limestone voxel every few centimetres along every rib. The
ballroom measured **1021 of them** on one ceiling. `clipcheck` cannot see it either: the rule fired,
so `never fired` is empty; the geometry is joined, so `components` is 1; and 1021 voxels do not move
a material share to four decimal places. It is found by painting the base coat something absurd in
a throwaway copy and looking for what is still wearing it.

So: **0.02 for a shape you drew where it stands, 0.035 for a shape you moved there.** 0.035 costs
nothing — it only widens the band a rule answers to, and every rule here is keyed on its own shape,
so there is nothing next to it for the extra 15 mm to bleed onto.

**6. Stairs are for walking up.** A riser is 0.18 m and a run is 0.32 m, everywhere in this
building, without exception. Ten risers take you from the ground to the podium. If your part has
steps, they use those numbers.

**7. Head height is 2.10 m minimum** anywhere a person can stand, and 2.40 m through a door.

**Measure it. Do not assume it, and do not measure it with `--gap`.** Two rooms drawn to exactly
2.10 have come in under it, for two different reasons, and neither showed up in any other number:

*A room drawn to exactly 2.10 measures 2.0625.* A voxel is solid if **any** of it is stone, so two
planes 2.10 apart that do not land on voxel boundaries lose a voxel at each end. Put your floor and
your soffit on numbers that ARE boundaries at metre 16 and metre 32 — the crypt's soffit is at
1.6875, which is 1.80 − M/4 — or draw the room 2.20 and let it lose two.

*`--gap` cannot answer this question.* It reports from the first empty cell to the last, so on any
probe whose box is taller than one storey it reports the box and flags it BROKEN because your
ceiling is in between. Asked across the crypt floor it said "3.125 m of air, BROKEN" everywhere,
about a room 2.14 m high. Use the **`clear`** line instead — the longest unbroken run of air, with
whether stone closed it at each end — and sweep it across the floor rather than taking one point:

```
tools/clipcheck.sh clips/facility/requests/yours-probe.clip --metre 16 --gap y@0,0
    gap     3.125 m of air along y (BROKEN — there is matter in it)     <- the box, not the room
    clear   2.125 m, the longest unbroken run, closed at both ends      <- the room
```

For anything with a floor under an overhang — a stair, a gallery, a vaulted passage — the engine's
own `walkability` check cannot help you at all: it walks a heightfield and finds the topmost matter
in each column, so a floor under something else is never a surface and is never tested. `stair.clip`
solves it by building the volume a head sweeps through as a shape of its own and intersecting it
with the part, which must then measure EMPTY. Copy that.

## How to build and look at your part

Everything runs from the repository root, `C:\Users\pc\Desktop\WorldShaper`.

**If you are not on Windows** — no Vulkan SDK, no SDL, no MSVC — the game's own `--clip-file` will
not run, and `tools/clipcheck.sh` is the same four calls (parse, sample, despeckle, measure)
against the same libraries, built with g++ in about forty seconds. Everything below that reads
`WorldShaper.exe --clip-file X --clip-metre N --clip-part P` is `tools/clipcheck.sh X --metre N
--part P`. It does not take screenshots and there is no substitute for those.

**Check that it parses and measure it:**

```
.\build\bin\WorldShaper.exe --clip-file clips\facility.clip --clip-metre 8 --clip-part part_yours
```

That prints the extent, the volume, the surface area, how many separate components it is in
(anything but 1 usually means something is floating), and `worldbox` — where it actually is.
Errors are reported as `yourfile.clip:12: what was wrong`.

**Photograph it, on its own, from every side:**

```
powershell -ExecutionPolicy Bypass -File tools\views.ps1 -Clip clips\facility.clip -Part part_yours -Out renders\yours -Metre 12 -Views ring
```

That writes eight elevations, a plan, and `renders\yours\contact-sheet.png` — one image with all
of them, labelled with how much of the frame the part filled. **Read the contact sheet.** It is
the only thing that will tell you your capital looks like a mushroom.

`-Views close` gets you a tight orbit for detail. `-Views inside` stands in the middle of the
part and looks each way, which is how you check an interior. `-Metre 8` is fast and rough,
`-Metre 16` is a fair likeness, `-Metre 32` is the real thing and slow.

**Photograph it in place, with the rest of the building around it:**

```
powershell -ExecutionPolicy Bypass -File tools\views.ps1 -Clip clips\facility.clip -Focus "-8,1,-13, 8,12,-7" -Out renders\yours-in-place -Metre 12
```

`-Focus` is a box in metres, `x0,y0,z0, x1,y1,z1`. Use it to frame your part while the whole
building is built — which is the only way to see whether it actually meets its neighbours.

## The language

The full grammar, with everything you can write:

```
let name = box    x0 y0 z0  x1 y1 z1  round=0.02      # two opposite corners
let name = sphere cx cy cz  r=1.0
let name = cylinder cx cy cz  r=0.45 h=8.1 axis=y     # h is the FULL height, centred on cy
let name = cone   cx cy cz  r=1.0 h=2.0 axis=y        # base at cy, apex h above
let name = torus  cx cy cz  ring=1.0 tube=0.2 axis=y
let name = capsule x0 y0 z0  x1 y1 z1  r=0.25
let name = ellipsoid cx cy cz rx=1 ry=2 rz=1
let name = prism  cx cy cz  r=1 h=1 sides=6 axis=y turn=0.0
let name = wedge  x0 y0 z0  x1 y1 z1  rise=y run=z    # a ramp
let name = stairs x0 y0 z0  x1 y1 z1  run=0.32 rise=0.18
let name = plane  nx ny nz  at=0.0                    # the half space behind it
let name = cube   cx cy cz r=1        # also tetra, octa, dodeca, icosa

let name = spiral cx cy cz  r=0.36 tighten=0.55 tube=0.045 turns=2.5 axis=z
                                      # a logarithmic spiral swept as a tube — the Ionic volute.
                                      # `tighten` is what the radius is multiplied by over one
                                      # whole turn, so 0.55 means each turn is a little over half
                                      # the one outside it. It starts at radius r along the first
                                      # cross-axis and winds from there.

let name = union        { a b c }     # smooth=0.1 rounds the joins
let name = difference   { a b c }     # a minus everything after it
let name = intersection { a b }

let name = translate { a } dx dy dz
let name = rotate    { a } x=0 y=0.25 z=0             # in TURNS, not degrees
let name = scale     { a } x=1 y=1 z=1
let name = mirror    { a } axis=x                     # folds the coordinate: appears both sides
let name = repeat    { a } x=2.7 nx=3                 # every 2.7 m, 3 either side of the origin
let name = around    { a } count=8 axis=y             # radial repeat
let name = shell     { a } thickness=0.1
let name = round     { a } by=0.05                    # grows the shape and rounds its arrises
let name = offset    { a } by=0.05                    # POSITIVE SHRINKS. See the note below
let name = displace  { a pattern } amount=0.02
let name = twist     { a } turns=0.25 axis=y
let name = bend      { a } turns=0.1 axis=y
let name = revolve   { profile } axis=y               # also: revolve cx cy cz { profile } axis=y
                                      # turns a section about an axis. The section is asked at
                                      # (radius, height), so its first two numbers are a RADIUS
                                      # and a height, measured from the axis. Exact — a circle
                                      # revolved about its own centre is a sphere to the last
                                      # decimal — so a base, a baluster, an urn or a dome is one
                                      # profile drawn once rather than a stack of cylinders.

# `offset`, and the sign, which has already cost a whole set of joints.
#
# A POSITIVE `by` SHRINKS THE SHAPE. Negative grows it. That reads backwards and it is not a
# choice: negative is matter, `offset` is `field + by`, so adding a positive number pushes the
# surface inward. Every other key here that takes a size grows the thing it is given -- `round
# by=`, `shell thickness=` -- which is exactly why this one gets written the same way and is wrong.
#
# The line above said `by=-0.05  # shrinks or grows` and taught the mistake. In steps.clip,
# `offset { steps_mass } by=-0.045` was written to shrink the flight by 45 mm to make a core to cut
# joints out of, and grew it by 45 mm instead. The core came out BIGGER than the mass, so
# `difference { mass core }` was empty and every joint in the great stair simply did not exist.
# It sampled to the voxel: steps_mass 2,423,674 and steps_flight 2,423,674, the same number. No
# error, no floating component, no missing material, no effect -- a cut that removed nothing, which
# is the failure that has no symptom at all.
#
# If you want a core to cut out of something, `by` is POSITIVE. Then check it: sample the two and
# compare the counts before you believe the joints are there.

# The mouldings. Sections, not solids of their own: put them inside a `revolve` for anything that
# goes round a column, or give them six numbers to run one straight along a cornice.
#
# Two opposite corners like a box, and THE ORDER OF THE CORNERS IS THE ORIENTATION: the first is
# in the stone, the second is in the air. Swap them and the curve turns over — there is no flip
# key because there does not need to be. Four numbers is (across, up) twice, with the run set to
# a metre either side of zero, which is what a revolve wants; six is a full box, and `run=` says
# which axis the moulding travels along (z by default, so the section is drawn in x and y).

let name = fillet  p0 q0  p1 q1                       # a plain square band
let name = ovolo   p0 q0  p1 q1                       # a convex quarter round
let name = cavetto p0 q0  p1 q1                       # a concave quarter hollow
let name = bead    p0 q0  p1 q1                       # a half round: the order's "torus"
let name = astragal p0 q0 p1 q1                       # the same thing, small, by its own name
let name = scotia  p0 q0  p1 q1                       # the deep hollow, deepest above the middle
let name = cyma    p0 q0  p1 q1                       # the S: convex at the first corner's end
let name = cyma_reversa p0 q0 p1 q1                   # the same S turned over
let name = ovolo  x0 y0 z0  x1 y1 z1  run=z           # six numbers: a straight length of it

let p = fbm size=0.1 octaves=3 seed=7                 # also ridged, noise, cells, rasp
let p = stripes axis=y period=0.6 duty=0.5
let p = bricks length=1.2 height=0.6 mortar=0.02 facing=z
let p = checker size=0.5
let p = sine axis=x period=0.1 phase=0.0
let p = axis of=y                                     # the raw coordinate
let p = distance cx cy cz                             # radial

paint marble where=some_shape below=0                 # where that shape is inside
paint moss   where=some_pattern above=0.55            # where a pattern is high
paint lead   where=roof below=0 facing=y at=0.6       # only on up-facing surfaces

weather desert 0.14 scale=0.8 seed=9 on=my_walls      # desert, overgrown, cracks, burnt, sea
```

`repeat` needs the shape to fit inside one period, and `nx` counts copies **either side** of the
original, so `nx=2` gives five. Rotations are in turns: 0.25 is a quarter turn.

**`weather` needs `on=`.** Without it, weathering works on the whole clip: it will bleach the
lawn, scour the volutes and sand the steps along with whatever you meant it for, because the coats
it adds are keyed on a value rather than on a place and they go on after everything you painted.
That is why the last facility shipped with none. With `on=<one of your own bindings>` both halves
are confined — the deformation is multiplied by an inside-ness mask of that shape, and every coat
it adds is pushed out of range everywhere the shape is not — so you can weather your own part and
nobody else's. Name a shape of yours, never somebody else's.

## Making it Ionic

The order is the point. A few things that separate an Ionic column from a cylinder:

- **The base is Attic**: a square plinth, a big lower torus, a hollow scotia, a smaller upper
  torus, and a fillet. Draw it as a section and `revolve` it — four mouldings, in radii, in four
  lines. Stacked cylinders were the old advice and are no longer it: a stacked scotia is a
  staircase and a stacked torus is a drum, and both cost more lines than the real thing.
- **The shaft tapers** — the top is five-sixths of the bottom — and it is **fluted**: 24 hollows
  with flat fillets between them. `around { }` a small cylinder subtracted from the shaft is the
  way to do it.
- **The capital has volutes**: two scrolls on the front and back, joined by a cushion on the
  sides, over an egg-and-dart echinus. A volute **is** a logarithmic spiral, so it is a `spiral`
  and not a stack of rings; the echinus is an `ovolo` revolved about the shaft.
- **The entablature is three bands, not one**: an architrave of three fasciae each stepping
  forward of the one below, then a plain frieze, then a cornice with **dentils** — a row of small
  blocks — and a projecting corona above them.
- **Nothing is a sharp arris.** Every edge in stone has a small `round by=0.01`. It is what makes
  it read as carved rather than extruded.

## What "done" means

Your part is done when:

- `--clip-part part_yours` reports **1 component** and no floating voxels.
- The contact sheet shows it correct from all eight sides, not just the front.
- Every dimension in your file traces back to the module.
- It meets its neighbours — checked with `-Focus` on the whole building.
- It is painted, with its own materials, keyed on its own shapes.
