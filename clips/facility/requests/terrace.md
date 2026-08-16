# terrace — what the language and the roof would not let me do

Five things. Two are facts about the building that the brief could not have known, two are
limitations of the clip language, and one is a sampler artefact that only shows at metre 8.

## 1. There is no ground north or south of the drum, so there is no walk round it

The brief asks for "a broad walk all round the drum". Measured rather than assumed:

| what | where |
|---|---|
| drum.clip's plinth | annulus to **5.85**, topping out at 12.15 |
| drum.clip's copper apron lip | to **5.895**, 12.15 .. 12.195 |
| roof.clip's gutter, inner edge | **5.85** |

So on the north–south axis the drum's apron *oversails* the gutter by 0.045 m. There is not one
millimetre of deck between them. Carrying a walk round would mean bridging a 1.20 m channel, and
the north and south gullies with their bronze gratings sit at |z| 6.55 .. 7.10 on that exact
centre line — the only place a bridge could land.

I did not build the bridge. The terrace is two courts, east and west, each with its own
quarter-circle ambulatory ending at the terrace edge, and they do not meet. The consequence the
brief cares about is that the cistern and the armillary cannot both stand on the building's own
north–south axis, so each court has one of each — cistern north, dial south — which keeps the
pairing and keeps the whole terrace symmetric about x = 0.

**If a walk round is wanted later**, the only honest way is a *grated* catwalk at 12.20 .. 12.30
spanning |z| 5.70 .. 6.30, which clears the socle at 5.625, clears the gully at 6.55, and lets the
gutter run under it. That is 0.60 m wide and it would want an opinion from roof.clip before
anybody built it. It is not something a fragment should decide for another fragment's drainage.

## 2. The floor had to go to 12.30, not 12.25

The brief gives the deck as 11.75 .. 12.25 and the terrace zone as starting at 12.25. The deck is
really 11.85 .. 12.15 at the drum, stepping down to 12.0375 at the outside, with rolls standing
0.045 proud of each bay — so the tallest thing the paving has to pass over is 12.195.

At a floor of 12.25 the lid over a roll channel is 0.055 m. That is one and three quarter voxels
at metre 32 and **under one voxel at metre 8**, and it was measured: no voxel centre lands in it,
the paving comes apart into twelve strips lying between the roll lines, and the part reports
**52 components** at metre 8. At 12.30 the lid is 0.09 (M/5) and the channels are cut per bay
rather than to one depth, which puts 0.1275, 0.165 and 0.2025 over the outer three bays; the part
reports **2** — one to a court — with everything else standing on them.

The cost is that the balustrade is 0.70 above the terrace floor instead of 0.75. It is still low
and still deliberate; the parapet is not raised.

## 3. `rotate` has no box, and that shapes what a treillage can be

A real treillage is a **diagonal** lattice. Written that way it is one bar `rotate`d and then
`repeat`ed, and `repeat` declares infinite slack the moment its child has no finite box —
roof.clip's note records that costing that file 8000x. The workaround (intersect the rotated
shape with a box first) works for one object and I used it for the armillary, where there is
exactly one rotate; it does not work inside a repeat without giving every copy its own box.

So this screen's lattice is **orthogonal**, which is a garden fence rather than a treillage panel.
What would fix it is either a bounding box for `rotate` (it is a rigid motion of a box — the
answer is exact and cheap) or a `lattice`/`grid` node that takes two directions.

## 4. A revolve profile cannot draw a straight taper

A terracotta pot flares. The profile primitives are boxes and the eight mouldings, and none of
them is a straight sloping line, so a taper is either a stack of boxes (what I did — three, and
it reads as a slightly stepped pot) or a `cavetto`/`cyma`, which is a curve and not a taper. A
`taper p0 q0 p1 q1` section — the moulding family's missing straight member — would be one line
here and in every column shaft, plinth and pedestal in the building.

## 5. The iron rail reads as floating at metre 8, and does not at metre 16 or above

Two components of 580 voxels apiece, at the rail line, at every metre-8 run. The rail is bedded
0.10 into the paving and shares space with it, so this is not a gap in the geometry — it is what
0.06 m of matter does to a 0.125 m sampler. Measured across three detail levels:

| metre | components | matter loose | what floats |
|---|---|---|---|
| 8 | 44 | 1178 of 37396 voxels | the four rail runs, the caisse straps, specks of ring and lattice |
| 16 | 14 | 12 of 265 956 | twelve single voxels, all on armillary rings |
| 32 | 38 | **94 of 2 037 173** | 36 specks, every one of them on an armillary ring |

Deepening the rail's bed from 0.05 to 0.10 changed nothing at metre 8, which is the evidence that
it is the sampler and not the model: the same change is what fixed the lantern cages, whose
0.03 m plates really were vanishing, and by metre 16 the rail is part of its court again.

At metre 32 the only thing still coming loose is the armillary — 36 pieces averaging two and a
half voxels, on rings whose tubes are 1.0 to 1.6 voxels through. That is the load case behaving
exactly as the load case is meant to: a hoop at one voxel is dashed, and thickening it to make the
report clean would be deleting the measurement. The two courts are 1 018 541 and 1 018 538 voxels
— three voxels apart out of a million, which is as close as a mirrored pair gets when the grain
and the weathering are asked at different coordinates on either side.

## 6. `weather` costs about thirteen seconds a coat, and it is NOT the scope — a control says so

roof.clip records `weather overgrown` on its gutter taking that part from 110 ms to 17,646 ms at
metre 8 — 160x — and says the cost is not the scope but `occlusion`, `curvature` and `facing`
being asked of the whole clip once per solid voxel. This part carries two coats. Three alternated
pairs of runs at metre 8, the two `weather` lines present and commented out:

| | 1 | 2 | 3 |
|---|---|---|---|
| with, scope = `terr_paving` (48 nodes) | 22.9 s | 24.3 s | 17.1 s |
| without | 3.4 s | 1.9 s | 2.8 s |

About **13 s a coat**, and eight times what the rest of the part costs to sample. My theory was
that the scope's own field is walked once per voxel, so I rewrote both scopes as one box less one
cylinder — two nodes instead of forty-eight — and measured again, alternated, same session:

| | 1 | 2 | 3 |
|---|---|---|---|
| with, scope = 2 nodes | 29.4 s | 28.6 s | 30.2 s |
| without | 2.3 s | 2.7 s | 1.9 s |

**A scope of two nodes costs the same as a scope of forty-eight, or slightly more.** The cheap
scope was my theory and the control killed it; roof.clip's diagnosis stands, and this is now a
second independent measurement of it on a different part with a different shape. The complex
scope is the one that shipped, because it is the one that puts the bleaching on the paving and
not on the citrus.

(The absolute numbers in both tables are inflated — the machine was running several other agents'
clipchecks throughout — which is exactly why every figure above is a with/without pair taken back
to back rather than a number on its own.)

Both coats fire: `bleached` at 6.4% of the part's voxels, `lichen` at 2.0%.

**What would be worth fixing:** `occlusion`, `curvature` and `facing` are surface quantities. They
are being asked for every solid voxel of the clip and then thrown away everywhere the scope is
zero. Asking them only where the scope's mask is non-zero would make a scoped coat cost what its
scope covers, which is what everyone writing `on=` already believes it does.
