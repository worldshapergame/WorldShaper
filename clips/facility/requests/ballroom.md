# What the ballroom needed and could not have

Five things, in the order they cost me time. The first one is the important one: it is a way of
getting the exact fault BRIEF.md rule 5 was written to prevent, by a road BRIEF.md does not mention,
and it is silent.

## 1. `below=0.02` IS NOT ENOUGH FOR ANYTHING PLACED BY A TRANSFORM

BRIEF.md rule 5 sizes `below=` against the manifest's 12 mm of grain and says 0.02 is "comfortably
past" it. It is, for that. It is **not** enough for a shape that is placed by a `translate`, a
`rotate`, an `around` or a `repeat`, and the failure looks exactly like the failure the rule exists
to prevent: a rind of the base coat over your own geometry.

A voxel is decided solid by how much of it a shape covers. A paint rule is decided at the voxel's
**centre**. So a shape whose surfaces do not land on the grid owns voxels whose centres are outside
it — by up to half a voxel, which at the contract's metre 32 is 0.0156 m — and 0.02 does not clear
that plus the shape's own phase. Measured, each of these on its own solid, at metre 32:

| shape | `below=0.02` | clears at |
|---|---|---|
| `box` drawn in absolute coordinates | 0% unpainted | — |
| the same box under `translate` by a fraction of a voxel | **3%** | 0.025 |
| a `repeat` of ribs under `rotate y=0.125` | **16%** | 0.025 |
| rays under `around count=16` | **4%** | 0.030 |

The reproduction is four lines and needs nothing from this building:

```
let t = translate { box 0.10 -0.05 -0.015   0.80 0.05 0.015 } 0.05 0 0.02
paint b where=t below=0.02
solid t
```

In this room, before it was found, the ceiling trellis came back with **1021 voxels of the
manifest's limestone** strung along every rib — pale specks on gold, on the one surface in the room
a viewer looks up at. Nothing in the report says so: `clipcheck` prints `limestone` in the material
list along with everything else, no rule reports `never fired` because every rule DID fire, and the
component count is unaffected. I found it by painting the base coat `lawn` in a throwaway copy and
looking for it.

**What BRIEF.md should say:** `below=0.02` where the shape is drawn in absolute coordinates, and
**`below=0.035`** wherever the rule paints a solid that a `translate`, `rotate`, `around` or
`repeat` put where it is. That is 1.1 voxels of margin at metre 32 and it costs nothing but a
halo of the same width, which lands on the thing's own neighbours.

A stencil that has no solid of its own — the painted marquetry in a floor — does not need it and
should not have it: it cannot leave a rind, and a bigger `below` only widens the line.

**And a way to check it that anyone can run:** copy your fragment, `sed` every `paint <mat> where=`
to one material, put `paint lawn` at the top, and measure. If `lawn` is not zero, you have a rind.
It took two minutes and it is the only test I know of that finds this.

## 2. `round { }` ON A UNION CAN CUT IT INTO PIECES

`round { a } by=0.05` is documented as "grows the shape and rounds its arrises". Growing a connected
set cannot disconnect it, so I reached for it as vestibule.clip does, to knit a chandelier strand
whose drops were shedding voxels. It made things worse and I nearly believed the drops were at
fault:

```
ball_ch_str1                          ->  44 voxels, components 1
round { ball_ch_str1 } by=0.006       ->  72 voxels, components 5
```

More volume **and** four new components. So whatever `round` does, it is not a pure dilation of the
set — presumably it works on a field that is a bound rather than a distance for a union of mixed
primitives, and the error is not one-signed. It is fine on a single primitive and it is fine on the
vestibule's `scale { ionic_column }`; it is not safe on a union of a capsule and five polyhedra.

**What would be better:** either make it sound on a union, or say in BRIEF.md that it is only
meaningful on a shape whose field is exact. Failing that, the honest fix is the one this file uses —
run the rod through the CENTRE of the last drop instead of up to its shoulder, so the overlap is
half a bead rather than eight millimetres.

## 3. A PROBE CANNOT NARROW `bounds` WITHOUT KNOWING WHICH INCLUDE COMES LAST

The contract's box is 34 x 21 x 25 m: 582 million cells at metre 32, about 2.8 GB, for a room that
is 1.6% of it. Narrowing it takes a metre 32 run from minutes to two seconds, which is the
difference between checking the thin matter after every edit and checking it once.

`metre` and `bounds` are last-writer-wins, and **every fragment includes `_contract.clip` again**,
which re-states them. So in a probe they have to be written after the LAST include — not at the top
where they read naturally, and not after the contract, which is where I put them first. Three runs
came back at the full 34 x 21 x 25 and looked exactly like the narrowing having no effect.

**What would be better:** an include guard, so a file included twice is read once; or a `bounds`
that a later statement cannot silently widen.

## 4. NO WAY TO PARAMETERISE A UNIT, SO EVERY WALL-MOUNTED THING IS DRAWN TWICE OR TURNED

There is no function and no argument, so a mirror pier that wants to be 0.675 wide on one wall and
1.125 on the other is two bindings that differ in six numbers, and a girandole that has to appear on
a wall facing +z and a wall facing -z is `translate { rotate { unit } y=0.5 }` with a clipping box
round it to put the bounds back. That works — everything in here that stands on the north wall is
the south unit turned half a turn — but it costs a box per group and it is the one place a wrong
sign is invisible: `rotate y=0.25` carries local +z into world **+x**, so the east wall's hangings
built with `y=+0.25` are drawn 0.08 m inside walls.clip's stone and the clipping box then leaves a
rind of them. It looked exactly like the silk not being there.

**What would be better:** `let unit(w) = ...`, or even just a `place { unit } at=x,y,z facing=-z`
that composes the turn and the bounding box together.

## 5. WHAT I COULD NOT MEET: THE STAIR

The brief asks this room to meet the interior grand stair "on the east side at 6.30" and to leave a
doorway where it arrives. `stair.clip`'s gallery is at x 11.86..15.07, **z 4.74..6.57** — the NORTH
strip of the east wing. This room is on the SOUTH front at z -6.60..-3.00. They are 7.7 m apart
across the east hall's groin vaults, whose block is solid to y 6.975 in z +-2.925, and **there is no
floor at 6.30 in the strip between them**: halls.clip's slab stops at x 13.95, stair.clip's gallery
stops at z 4.74, and nothing owns x 13.95..15.10, z -3.00..4.74.

So the doorway is where it would arrive — 0.90 wide on x = 14.40 (32 M), at the east end of the
north wall, 2.40 m high — and it opens onto air. **Whoever builds the upper east hall should carry a
floor at 6.30 along the east wall from the stair gallery's south edge to z = -3.00 and it will
land on this threshold.** It is 1.15 m wide, which is the strip left between the hall block at
13.95 and the wall's inner face at 15.10.

## Smaller things, without complaint

- **`repeat` takes `y=`/`ny=` and `z=`/`nz=` as well as `x=`/`nx=`.** BRIEF.md only shows `x=`.
  Both work; the stove's tile courses and joints use all three.
- **The corners of a `box` are normalised**, so `box 0 0 0.5  1 1 -0.5` is the same as the sorted
  one. The mouldings are NOT — for those the order of the corners is the orientation, which
  BRIEF.md does say.
- **`solid` with no `paint` before it reports EMPTY**, not "unpainted". A probe manifest needs the
  base coat or it looks like the geometry failed to build.
- **A `spiral`'s inner tip is not always found by the sampler.** Nine C-scrolls came back as one and
  two loose voxels sitting in the middle of the curl. A boss over the eye covers the inner coil and
  knits it, and it is what a carved rocaille scroll has there anyway.
