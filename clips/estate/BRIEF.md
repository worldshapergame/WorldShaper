# The estate — a brief for everyone building on it

The facility is one building. This folder is the rest of the place it stands in: an orangery, a
grotto, a belvedere, a bell tower, a fountain court, a colonnade and a garden theatre, in the
century and a half between the Renaissance and the Rococo. Each is **its own clip in its own
file**, and that is a decision with a reason, written down below so nobody undoes it.

## Why these are separate clips and not more fragments of the facility

`clips/facility.clip` is one clip made of twenty-odd included fragments, and it can be, because
they all stand inside one 34 × 21 × 25 m box. **A clip is a dense array over its bounds** — five
bytes a cell in the game, and rather more in the measuring tool — so that box is already 582
million cells and about 2.8 GB before a single voxel exists. `_contract.clip` says so at length
and cuts the bounds to the building plus a metre for exactly that reason.

An estate is a hundred metres across. Put in one clip at the contract's metre 32 that is eleven
billion cells, and there is no machine on which that is a scene. Measured rather than assumed:
a 120 × 27 × 120 m box at **metre 8** — a quarter of the authored detail — samples to 199 M cells
and peaks at **2.2 GB**, which is the whole budget spent on voxels four times too big.

So the estate is a FAMILY of clips. Each building is authored at metre 32, full detail, in a box
cut to itself, and each is opened on its own — from the game's library, or with

```
build\bin\WorldShaper.exe --clip-file clips\estate\orangery.clip
```

That is not a compromise on what the user asked for. It is the only shape in which "a full
complex" is a thing anybody can load. The buildings share a module, a material list, a site plan
and a north point, so they belong to one place whether or not they are ever in one array.

## The shared contract

**Every file here starts with `include "../facility/_contract.clip"`**, and then declares its own
`metre` and `bounds`, which override the facility's. That gets you, for free and identically:

- the module — **D = 0.90 m**, **M = 0.45 m**, and every level the facility is built to
- **every material**, including the state-room list added for this work: `mirror`, `crystal`,
  `alabaster`, `porcelain`, `velvet`, `silk`, `damask`, `ormolu`, `gold_leaf`, `silver`, `iron`,
  `parquet`, `boiserie`, `lacquer_red`, `ebony`, `gesso`, `shellwork`, `wax`, `candle`, `taper`,
  `travertine`, `giallo`, `rosso`, `malachite`, `breccia`, `jasper`, `onyx`, `tuff`, `boxwood`,
  `citrus`, `orange`, `bark`, and `glass_ruby` / `glass_blue` / `glass_gold`, which carry
  `absorb=` and are therefore coloured VOLUMES rather than coloured surfaces
- the shared patterns — `grain_fine`, `grain_broad`, `grain_stone`, `courses`, `ashlar`

Do not re-declare a material the contract already has unless you mean to change it everywhere.

## The site plan

North is **+z**, east is **+x**, and the facility faces **south**. It occupies x −16 … 16,
z −7.5 … 7.5, with its great steps running out to z −15.7. Its ground is y = 0 and its main floor
is y = 1.80. Everything here is placed against that, in its own coordinates, with its own origin
at a sensible corner of itself — a clip does not have to be built where it stands, and a tight box
round a building at the origin is cheaper than the same building 60 m away with 60 m of empty
array in front of it.

Where each thing belongs on the ground, for the day these are stamped into one world:

| | where, relative to the facility | roughly |
|---|---|---|
| forecourt colonnade | embracing the great steps, south | z −16 … −40 |
| triumphal gate | on the axis, at the far end of the forecourt | z ≈ −44 |
| orangery | east terrace, facing south | x 24 … 60, z −12 … 0 |
| bell tower | north-east, on the cross axis | x ≈ 30, z ≈ 14 |
| fountain court | on the axis, north of the building | z 16 … 40 |
| grotto | north-west, cut into the terrace wall | x −34, z 20 |
| belvedere pavilion | on an island in the north basin | x ≈ 0, z ≈ 46 |
| garden theatre | west, in the bosquet | x −40 … −20, z 4 … 28 |

Write that placement into your file's header as a comment. Nothing enforces it yet.

## The rules, and they are the facility's rules

Read `clips/facility/BRIEF.md` in full. All of it applies here, and these are the ones that catch
people:

1. **Take every dimension from the module.** If you are typing a number that is not a multiple or
   a simple fraction of 0.45, work out what it should be. This is the difference between a
   classical building and a pile of classical-looking parts, and it is the same rule whether the
   building is Palladio's or Cuvilliés's.
2. **One part per file, named `part_<yours>`**, and everything else you bind is prefixed with your
   own name so nothing collides.
3. **Everything must touch something.** Overlap a join by 0.05 m. Never butt it exactly: a surface
   displaced by weathering opens a hairline gap and your building comes back in two pieces.
4. **Paint only your own shapes, and write `below=0.02`, never `below=0`.** Painting by a bare
   coordinate paints somebody else's building. `below=0` misses every voxel the final grain pushed
   outward and leaves a pale rind over your work.
5. **Stairs are 0.18 rise and 0.32 run**, everywhere, without exception. Head height is 2.10 m
   where a person stands and 2.40 m through a door.
6. **`weather` needs `on=<one of your own shapes>`.** Without it, it bleaches the lawn.
7. **Nothing is a sharp arris.** `round by=0.01` on stone edges is what makes it read as carved.

## What a part is FOR, which is half of what it is

Every fragment of the facility carries a named share of putting the engine under load, and says
so in its own header before it says anything else. Do the same. A building here that looks
beautiful and exercises nothing has done half the job.

The engine's own list, from `_contract.clip`: **thin matter** one and two voxels across; **deep
hollows** reached only through small openings; **materials** compared against each other in one
frame rather than in isolation; **light** that arrives only after a bounce; **surface**, meaning
variation and weathering; and **walkable**, meaning a person can get in, climb, cross and get out.

The path tracer specifically has never been shown: a mirror facing a mirror, a caustic under
water, a coloured volume of glass throwing its colour on a floor, a translucent stone with a lamp
behind it, a retro-reflective fabric, or four hundred small refracting solids on one chandelier.
The materials for all six are in the contract now. Use them where they belong and nowhere else.

## How to check your work

There is no Vulkan SDK and no Windows on the machine most of this is being built on, so the
game's own `--clip-file` cannot be run there. **`tools/clipcheck.sh` is the same four calls
against the same libraries** — parse, sample, despeckle, measure — and it is what to use:

```
tools/clipcheck.sh clips/estate/orangery.clip --metre 8
tools/clipcheck.sh clips/estate/orangery.clip --metre 8 --part part_orangery
tools/clipcheck.sh clips/estate/orangery.clip --metre 16 --slice y@2.0
tools/clipcheck.sh clips/estate/orangery.clip --metre 32 --gap y@0,0
```

`--metre 8` is fast and rough and is what to iterate on. **Check at metre 32 before you finish**,
because that is the resolution the file claims and half the mistakes in this repository are
features that vanish at one resolution and not another.

What it tells you, and what each line catches:

| line | what it catches |
|---|---|
| `ERROR file:line` | a typo, in the file you actually wrote, through the include splice |
| `matter extent` | a shape that is not the size you think it is |
| `worldbox` | where it really is — a building 3 m from where it was meant to stand |
| `components 1` | **the first test of done.** Anything else is something floating |
| `never fired` | a paint rule that painted nothing, which is silent and looks like success |
| `materials` | a rule that fired on far more than it should have |
| `--slice` | a wall one voxel thick, a tread that is not level |
| `--span` / `--gap` | head height, doorway width, wall thickness |

`components 1` and an empty `never fired` list are not style points. Both have shipped broken in
this repository, and neither was visible in a screenshot.
