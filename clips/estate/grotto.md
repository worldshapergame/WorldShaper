# grotto — what the language could not do, and what it did that surprised me

Six things, in the order they cost time.

## 1. A union of two shapes has a surface that is not a surface, and a displacement grows lumps off it

This is not a missing feature — it is the field arithmetic working exactly as documented — but it is
the single most expensive thing in this file and it will catch the next person who grows anything
off a room.

`grot_hall_air` is a walls box (top 2.70) unioned with a vault clipped to y 2.65 .. 6.20. A union
answers with the SMALLEST of its children's distances. At a point 80 mm below 2.70 and a metre from
any wall, the vault says "+0.03, outside me" (it is clipped at 2.65) and the box says "-0.08, just
inside my top face" — so the union says **-0.08 for a point that is a metre from anything**. Every
lump field in this file is `f(distance to the room)`, so at that point the encrustation reads "the
wall is 80 mm away" and grows a lump of shellwork in mid air.

Measured, at metre 8 with the weathering off, which is the control that settled it:

| | components | floating voxels |
|---|---|---|
| vault clipped at the springing, y 2.65 | 442 | 524 |
| vault clipped at the floor, y 0.00 | 3 | 2 |

The 439 were single lumps hanging 0.08 m under the springing of the hall and 0.13 m under the
springing of the chambers — which is where each wall box has its top. The fix is one number: clip
the vault's box from the FLOOR, so that below the springing the vault reports the same large
negative distance the walls do and the `min` is right again. It changes not one voxel of the shape.

**What the language could do to help**: nothing about `min` is wrong, but there is no way to ask a
node "give me the distance to your BOUNDARY as a set" rather than "the min of my children". A
`union { } exact` that re-evaluated as a real distance would cost far more and is probably not
wanted; a note in the grammar next to `union` would have saved the afternoon.

## 2. `offset by=` positive SHRINKS, and the grammar says "shrinks or grows"

`documentation` and `clips/facility/BRIEF.md` both give `offset { a } by=-0.05  # shrinks or grows
without rounding`, which reads as "negative shrinks". It is the other way round: `Op::Offset` ADDS
its number to the distance, so positive shrinks and negative grows — the same convention as
`displace`, and the opposite of what "offset outward" suggests.

Written the obvious way round, `difference { offset { room } by=0.045   room }` is the empty set —
and it does not LOOK empty, because the displacement that follows makes it solid anyway. It sampled,
it measured plausibly, and what it built was a crust grown off a surface 45 mm INSIDE the room with
nothing joining it to the hill. Three places in this file needed the sign (the skin, the water film
over the cascade, and the trim that beds the grille bars into the arch jambs) and all three were
wrong the first time.

One line in the grammar — `by=0.05 shrinks, by=-0.05 grows` — closes this.

## 3. `curvature`, `occlusion` and `facing` exist in the field and are not reachable from a clip

`weather` is built out of all three (`field.cpp` has `Op::Curvature`, `Op::Occlusion`,
`Op::Facing`), and §6 of the forge document is entirely about them, but `clip_script.cpp` exposes
none of them as a pattern an author can name. `paint ... facing=y at=0.6` is the one thin slice of
it that reaches a clip, and it only decides a colour — it cannot drive a displacement.

That is what the stalactites in this file want. A stalactite is "grow the surface along its own
normal, but only where the normal points down", which is one multiply if `facing` is a pattern. It
is not, so they are built by stretching a `cells` field fourteen times along y — `scale { cells }
y=14` — so that its cells are columns, and a columnar field displacing a down-facing vault happens
to grow something that hangs. It works, and it has one real virtue the honest version would not
have (a column field barely changes along y, so a stalactite stays joined to the vault instead of
shedding its tip). But it also grows sideways off anything vertical in the same zone, which is why
the zone has to be fenced by height rather than by orientation.

`let p = facing of=y` and `let p = occlusion radius=0.2` would be four lines in the pattern parser
and would make "weathering follows the shape" available to an author instead of only to `weather`.

## 4. `on=` is a bounding box on a paint rule and a real mask on a `weather` — the same keyword, two meanings

`paint <material> where=<pattern> on=<shape>` bounds the rule by the shape's BOUNDING BOX and does
not test the shape. On a room whose box is the whole building that is no confinement at all, so
every pattern-keyed rule in this file is written as `where=intersection { <my shape> remap { field
} }` instead — a remap with `low` negative and `high` positive turns a pattern into a thing with an
inside, and an intersection of that with a real shape is a real shape with a real box. It is a good
idiom and it should be in the BRIEF, because the obvious reading of `on=` is that it masks.

`weather ... on=` DOES mask (it multiplies by an inside-ness smoothstep), which makes the two
spellings of the same keyword mean two different things. That cost a second afternoon: `weather
cracks on=grot_facade` names the facade BEFORE its arches are bored out of it, so the iron grilles
standing in the middle of those arches are inside the named shape and were being cracked as
masonry — 82 components on the facade alone at metre 32, every one of them a piece of a 45 mm bar
with a 29 mm fissure bitten out of it. `on=difference { facade arches }` is 1.

## 5. `--gap y@a,b` has its two arguments crossed in `tools/clipcheck.cpp`

```
const i32 a = (gap.a * per) - built.origin_voxel[(gap.axis + 1) % 3];
const i32 b = (gap.b * per) - built.origin_voxel[(gap.axis + 2) % 3];
```

`clipcheck` uses `(axis+1)%3, (axis+2)%3` for the two cross axes; `measure.cpp`'s `other_axes` uses
`(1, 2)`, `(0, 2)`, `(0, 1)`. Those agree for x and for z and disagree for **y** — the axis you use
for head height. For `--gap y@a,b`, `a` is scaled by the **z** origin and then used as an **x**
index, and `b` the other way round.

It is invisible on a clip whose bounds start at the same number on every axis, which is why nobody
has met it. This clip starts at x −11 and z −1.5, so `--gap y@0,3` actually probes x = −9.5,
z = 12.5. Not fixed here, because the fix is in `tools/` and this agent owns two files in
`clips/estate/`.

## 6. `--gap y@` cannot measure head height in a building that has sky over it

Separately from the bug above. `gap_along` reports the run from the FIRST air cell in the column to
the LAST, and reports `BROKEN` if there was matter in between. A grotto has open sky over the mound
and open ground under the apron, so every vertical probe anywhere inside it returns "8.000 m of air
along y (BROKEN)" — the height of the box — whatever the room under it is doing. The facility can
use `--gap y@0,0` because the facility has a roof.

`--slice z@4.3` at metre 8 gives the clear height above the floor at every x across the section, at
0.125 m, in one sample, which is strictly more information than a dozen gap probes would have been.
That is what the head heights in this file's ledger are measured from. A `--gap` that reported the
LONGEST contiguous run rather than first-to-last would answer the question directly and would still
be right for the facility.
