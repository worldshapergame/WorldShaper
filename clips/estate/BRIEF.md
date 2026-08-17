# The estate — a brief for everyone building on it

The facility is one building. This folder is the rest of the place it stands in: an orangery, a
grotto, a belvedere, a bell tower, a fountain court, a colonnade and a garden theatre, in the
century and a half between the Renaissance and the Rococo. Each is **its own clip in its own
file**, and that is a decision with a reason, written down below so nobody undoes it.

## These were separate clips, and on 2026-08-17 they stopped being

**REVERSED BY THE OWNER, and the reasoning that is struck through below is kept because being
wrong in an interesting way is the most useful thing a document here can record.** Every building
in this folder is now ALSO a fragment of `clips/facility.clip`, standing at its place on the site
plan, and the facility is the whole complex rather than the one neoclassical building. See D672.

The originals in this folder stay exactly as they are and still open on their own:

```
build\bin\WorldShaper.exe --clip-file clips\estate\orangery.clip
```

That is deliberate. A standalone clip is a tight box round one building, which is the only way to
measure that building at the metre 32 it is authored for — see "the cost", below. The fragment in
`clips/facility/` is a converted COPY, not a move.

### The argument this folder used to make, and the one thing it missed

It said: `clips/facility.clip` is one clip of twenty-odd fragments, and it can be, because they
all stand inside one 34 × 21 × 25 m box. **A clip is a dense array over its bounds** — five bytes
a cell — so that box is already 582 million cells and about 2.8 GB before a voxel exists. An
estate is a hundred metres across; at metre 32 that is eleven billion cells, and there is no
machine on which that is a scene. Measured rather than assumed: 120 × 27 × 120 m at metre 8
samples to 199 M cells and peaks at 2.2 GB, the whole budget spent on voxels four times too big.

Every number there is correct. **What it missed is that the game never samples the whole clip at
metre 32.** `--clip-coarse` defaults to 4, so the up-front build allocates the dense array at
**metre 8** and scales 4× on paste; every node after that is sharpened individually, each one a
`forge::sample` over its own small box. High detail is paid for per node, a few metres at a time,
and only where somebody is looking. The dense array is only ever allocated at the coarse metre.

So the complex — 125.5 × 37.5 × 110.5 m, 520,092 m³ — costs **1.33 GB at metre 8**, which is a
scene, and it was measured: the enlarged box samples in 32 seconds with 96% of its cells settled
in bulk, because empty air is exactly what a signed-distance descent is good at.

### The cost, which is real and is not memory

**Nothing can measure this clip whole at metre 32 any more.** Over the complex's bounds that is
17 billion cells and 85 GB, and it will not run. So:

- each building carries its own probe in `clips/facility/requests/<name>-probe.clip`, with its own
  tight bounds around its own position, and **those probes are now the only way to check a part at
  the resolution it is authored for**;
- whole-clip checks run at **metre 8**;
- and the rule "check at metre 32 before you finish" now has to be obeyed one building at a time
  rather than all at once. It is not optional — half the faults in this repository were features
  that existed at one resolution and not another.

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
