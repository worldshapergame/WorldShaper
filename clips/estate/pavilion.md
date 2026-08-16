# pavilion — what the language could not do, and what had to be worked round

Four things, in the order they cost time, and then two the tool caught that reading the file did
not. None of them is a bug; all four are places where the
grammar has no word for something the building wanted, and the workaround is in the clip with a
comment. Written down here so the next person building an octagon or a bell does not rediscover
them.

## 1. There is no way to run a moulding round a polygon

`revolve { profile } axis=y` turns a section about an axis and gives a **circle**. The six-number
mouldings (`ovolo x0 y0 z0 x1 y1 z1 run=z`) run a section **straight** along one axis. Between them
they cover a dome and a cornice on a rectangular building, and they cover nothing at all on an
octagon, which is what most garden buildings and every drum in this repository actually are.

The obvious fix does not work. `intersection { revolve { profile } prism }` gives a circular
moulding inscribed in the octagon: correct at the middle of each face, and receding from the stone
all the way to the corners, where it stands short by `apothem × (sec 22.5° − 1)` — 0.44 m here.
Cutting it the other way, `intersection { union of 8 straight mouldings, prism }`, does mitre
correctly, but each member is a `rotate` with no bounding box and eight of them per course, five
courses, is forty unbounded nodes in the middle of the building.

So the cornice here is **five stepped prisms with `round by=0.01`**, and the rounds are doing the
work the curves would have done. At 32 voxels to the metre a 0.01 round is a third of a voxel and
the steps read as steps rather than as a cyma. It is the one place in this file where the profile
is approximate.

**What would fix it:** a `sweep { profile } sides=8 turn=0` — the same fold `revolve` already does,
but onto a polygon's apothem instead of onto a radius. `sd_prism` already computes exactly the
number that fold needs (the maximum over the face planes), so the section would be asked at
(distance-to-nearest-face-plane, height) rather than at (radius, height). It is a few lines beside
`Op::Revolve`, and it would let a drum, a lantern, a font, a chimney and this cornice all be drawn
the way the round ones already are.

## 2. `stairs` always rises toward +z, and a flight that descends has to be turned three quarters

`sd_stairs` hard-codes `run = 2u, rise = 1u`. There is no `run=` key on `stairs` the way there is on
`wedge`, which takes `rise=` and `run=` and honours them.

For a flight that climbs **out of the water toward the middle of the island** the foot has to be
drawn at the more negative z, and the whole member then has to be turned by −0.375 of a turn rather
than −0.125 to reach the +x +z quadrant that `mirror { mirror { } axis=x } axis=z` requires. That is
easy to get wrong and silent when you do: the flight lands in −x −z and both mirrors throw it away,
leaving four flights that build as none, with no error and a plausible volume.

**What would fix it:** `stairs ... run=z rise=y`, keyed exactly like `wedge`.

## 3. A `mirror` has to be told its child is symmetric, and it cannot be

This is the trap `drum.clip` documents and it costs an afternoon per building, so it is worth saying
what would remove it. `mirror { a } axis=x` answers a query at `|x|`, so anything in the child that
lies at negative x is **not copied — it is deleted**. Every eight-fold member in this file therefore
has to be drawn in the +x +z quadrant AND, for the members that get turned by a quarter to reach the
±z faces, drawn symmetric about z before the turn. The two door leaves here are written out twice
for that reason, at +z and at −z, rather than as one leaf and a `mirror`.

There is no diagnostic for getting it wrong. The clip builds, reports one component, and is missing
half its ornament.

**What would fix it:** `mirror` could report, at `build_bounds` time, that its child's bounding box
lies wholly on the negative side of the fold — which is exactly the case that always means a
mistake, is cheap to detect, and would have caught both of this file's occurrences.

## 4. `displace` drops an amount under half a voxel, and the threshold is read off `metre` and not
off the sampling resolution

`usable_displacement` compares the amount against `script.settings.voxels_per_metre` — the file's
own `metre 32` — and drops anything under half a voxel with a warning. That is the right rule and
the warning is a good one. But it means the grain amount is tied to the resolution the file
*declares*, and a file authored at `metre 32` and opened at `--metre 8` keeps a displacement that is
an eighth of a voxel there. It is only a nuisance, and it is written here because the first version
of this clip used the facility's own `amount=0.012` and had it silently dropped: 0.012 × 32 = 0.384,
under the half. The facility gets away with it because `grain_fine` is not the only thing displacing
it. 0.016 is the smallest amount that survives at metre 32.

## Not a language problem, but worth the note: the water must not be displaced

`clips/facility.clip` displaces the whole building by grain at the very end, and that is right for
stone. It is wrong for the one surface this clip exists to produce. 16 mm of fbm at 0.07 m across,
on a plane 26 m wide seen at grazing incidence from the near kerb — 5.7 degrees, eye 2.60 above the
water, far kerb 26.10 away — scatters the far half of the reflected image, which is where all of it
is. So `pav_water` is unioned **after** the `displace`, and its top face is exactly
the plane y = 0 everywhere.

Anyone stamping this into one world with the other estate clips should keep that split. A single
grain pass over a scene containing water gets a lake with a broken sky in it and no way to tell that
from a renderer fault.

## What the tool caught that reading the file did not

Both of these were invisible in the text and obvious in one line of `clipcheck` output. They are
here because the same two mistakes are available to anybody drawing an eight-fold building with
small metal in it.

**A hundred and ninety-one components, and every floater in one cubic metre of air.** At
`--metre 16` this clip reported `components 191 (largest 3983329 voxels, 2612 floating)`, and the
six largest floating pieces were all between y 4.31 and 4.81 with |x| and |z| under a metre — the
chandelier, and nothing else in the building. The drops were fine. What had gone was every piece
holding them up: a 0.09 stem, 0.072 hoops and 0.072 arms, which are the proportions a real
chandelier has and are ONE VOXEL ACROSS at metre 16 and two at metre 32. A tube whose diameter is
near one voxel is not thin matter, it is a dotted line. The metalwork is 0.09 to 0.11 now. The bars
(0.030) and the ribs (0.09 wide, standing 0.06 proud) are left alone, because they are where the
thin-matter argument is actually being made and they are held by stone on both ends.

**A hundred and ninety-two separate crystal beads.** Before that, the drops themselves were 0.108
across on 0.18 centres, threaded on a 0.018 ormolu wire. 0.018 is 0.58 of a voxel at metre 32: it
does not exist at any resolution this clip will be opened at, so the beads hung in the air with a
perfectly sensible volume and no error. They are 0.144 on 0.12 centres now and overlap each other
by 0.024, which is what a cut-glass chain is anyway.

The general rule, and it is not in any brief: **before you draw a connector, divide its diameter by
the voxel and check the answer is at least two.** At metre 32 that is 0.0625 m. Anything thinner has
to be held by something else at both ends, the way the glazing bars are held by the frame and the
frame is bedded 0.045 into the stone.

## What it measures, at the resolution the file claims

```
./build/clipcheck clips/estate/pavilion.clip --metre 32
sampled box   903 x 427 x 759 voxels   28.219 x 13.344 x 23.719 m
volume        32419291 voxels   989.36 m3   11.08% of the box
surface       3093754 faces    3021.24 m2
materials     30 distinct records
components    264 (largest 32418993 voxels, 298 floating)
42 min 37 s wall, peak RSS 4.01 GB
```

`--metre 16`, for comparison: `components 125 (largest 3985494 voxels, 534 floating)`, peak RSS
0.50 GB, 7 min 20 s. `--metre 8`: `components 94`, 41 floating.

The component count going UP between 16 and 32 while the floating volume goes DOWN is the shape of
the answer, not a regression: at 16 a handful of features are wholly missing and what is left of
them is in a few large clumps; at 32 they are all present and each is fraying at the edges into
one-voxel specks. 264 components against 298 floating voxels means two hundred and sixty of them
are a single voxel apiece. Nothing structural floats — the largest component is 99.9991 per cent of
the matter.

Where they are: the 0.030 m glazing bars (0.96 of a voxel at metre 32), the eight cupola ribs where
they converge at the crown, and the finial's coronet, whose eight cone points taper below a voxel
in their last 0.08 m and come apart into about thirty specks at y 11.44. **A cone tip is sub-voxel
by construction at any radius**, which is worth knowing before drawing one; the coronet is left as
it is because thickening it is exactly the move this part's brief exists to refuse.
