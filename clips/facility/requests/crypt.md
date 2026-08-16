# Requests from crypt.clip — the undercroft

Four things. The first is a reproducible wrong answer from the sampler and is the only one that
matters; the rest are things the language or the tool could have said and could not.

---

## 1. A SMALLER BOX MAKES A BIGGER SOLID, and the solid reaches outside every shape in it

`crypt_shell` is one block with the room's air taken out of it:

```
let crypt_block = box -11.85 -0.50 -5.85   11.85 1.80 5.85 round=0.02
let crypt_shell = difference { crypt_block  crypt_room crypt_wells crypt_coffers
                               crypt_slot_hollows crypt_niches }
```

Shrink that block by 0.02 on all six faces and change nothing else — one line, six numbers, each
0.02 nearer the centre — and the part grows by 26 cubic metres and spills past the block:

```
./build/clipcheck clips/facility/requests/crypt-probe.clip --metre 8

  block -11.85 .. 11.85     volume  93238 voxels  182.11 m3   worldbox x +-11.875  z max 5.875
  block -11.83 .. 11.83     volume 106549 voxels  208.10 m3   worldbox x +-12.000  z max 6.000
```

**x = 12.000 is 0.17 m outside the block the whole part is a difference of.** Nothing in this
fragment is drawn there. `components` is 1 in both arms, there are no floating voxels in either,
and no warning is printed. The extra matter is in the right shape to be *cuts that did not happen*:
z reaches 6.000, which is exactly where the niche voids end, and the 26 m3 is about what the niches
and the coffers take out.

`round=` is not the cause and is not what I first thought it was. Measured on a 2 m cube at
metre 32, `box -1 -1 -1 1 1 1 round=0.02` and the same box with no key give the same 8.0000 m3 and
the same worldbox to three decimals — **`round=` does not grow a box.** BRIEF.md's grammar does not
say it does either, but `clips/facility/stair.clip` is written throughout on the belief that it
does ("EVERY NUMBER BELOW IS 0.015 INSIDE THE FACE IT MAKES, and that is not a typo... a box
written to a tread top of 1.98 has its tread at 1.995"), and `halls.clip` says the same of its
transverse arch. One of those two readings is wrong and it is worth somebody settling which,
because a whole flight of stairs is placed by it.

What I think is happening, and I could not prove it: with the block's faces on -11.85 and -5.85 the
cut shapes' planes coincide with them, and with the block 0.02 in they do not, so some bound the
sampler settles whole boxes against stops being tight and a box is called solid without the field
being asked. That is a guess. What is not a guess is the two numbers above.

I worked round it by leaving the block on its module faces. That is where it wanted to be anyway,
so this fragment is not damaged by it — but the workaround is "do not perturb a number", which is
not a thing anybody can be told.

---

## 2. `--gap` needed a box that begins and ends in stone — and clipcheck now has the line that fixes it

*Superseded and left here because the reasoning was right.* `--gap` reports first-empty to
last-empty and says BROKEN when the air is in more than one piece, so against a probe of my part
alone it reported the whole 21 m box for every point in the room: the ceiling of this room is the
podium and the floor is the bottom of the podium, so there is sky above and open ground below.
No clip can promise a box that starts and ends in stone.

clipcheck now prints a second line — `clear 2.125 m, the longest unbroken run, closed at both ends`
— which is exactly the measurement a room needs, and it flags anything under 2.10 on a y query.
This room is now gated on it. (The `--gap` disagreement reported in the first version of this file
was a real bug in clipcheck's axis mapping for y queries, since fixed; the instinct to trust the
slices over the tool was right, and the workaround — calibrating against a clip of two bare planes
— is still how the numbers below were established.)

**What `clear` still needs:** it flags any column, and a room has things standing in it. Sweeping
this room's 63 points, six came back under 2.10 and every one is furniture — 1.00 m inside the
basin ring, 1.94 in the sump, 0.97 on a sarcophagus lid, 0.47 on a stair tread, 1.75 on a respond's
plinth. All correct, none a place a person stands. What a room has to promise is 2.10 m over every
part of the FLOOR. A `--head` that walked the topmost floor-like surface, or a `min=` that only
flagged columns whose run starts at the room's floor plane, would let a fragment gate itself. As it
is I swept a grid by hand and classified the flagged points by what is standing on them, which is
fine once and not fine every time somebody edits this file.

## 2a. Two voxel rules that between them cost this room 125 mm of head height

Neither is written down anywhere and both were found the hard way, each against a control.

**A voxel is solid if any of it is stone, so a face part-way through a voxel gives the whole voxel
away.** 2.10 is not a whole number of voxels at any metre this building uses — 2.10 / 0.03125 is
67.2 — so a room drawn to exactly the brief's minimum cannot measure it. This room, drawn floor
-0.45 and soffit 1.65 for a geometric 2.10, measured **2.000 at metre 16 and 2.094 at metre 32.**

**Voxels are half-open, so the two ends are not symmetric.** A solid's BOTTOM face on a voxel
boundary costs nothing; a solid's TOP face on a boundary costs the voxel above it. So a soffit
wants to be ON a boundary and a floor wants to be JUST UNDER one. Measured on a calibration clip of
two bare planes:

```
  floor top   soffit    metre 16   metre 32
  -0.45       1.65      2.000      2.094     the room as first drawn
  -0.45       1.6875    2.062      2.125     soffit on a boundary: fixes 32, not 16
  -0.4625     1.6875    2.125      2.156     what is built
  -0.46875    1.6875    2.125      2.125     also works; floor slab one voxel thick
```

The rule worth putting in BRIEF.md: **a floor or a soffit that bounds head height should land on a
voxel boundary of the contract's metre — a multiple of 0.03125 — with the soffit ON one and the
floor just UNDER one, and the room drawn 40-50 mm over the minimum so the grid has something to
take.** Anything upstairs whose head height was checked by arithmetic rather than by `clear` is 30
to 100 mm shorter than its file says. stair.clip's 2.115 m is the one with 15 mm of margin in it
and it is worth re-measuring first.

## 3. `repeat` cannot place a thing on the module twice over, so forty columns are two folds

The column grid wants x on +-1.35, +-4.05, +-6.75, +-9.45 and z on +-1.35, +-4.05: one square
lattice of the bay, offset half a bay from both axes. `repeat` counts copies either side of the
ORIGIN, so it cannot start at 1.35; the idiom that works is

```
mirror { translate { repeat { unit } x=2.70 nx=3 } 1.35 0 0 } axis=x
```

— repeat about the origin, carry the whole line out half a bay, then fold. It is exact and it keeps
its bound, and it is three operations to say "on the module, offset by half of it". A `phase=` key
on `repeat` (`repeat { a } x=2.70 nx=3 phase=1.35`) would say it in one, and every fragment in this
building that lays anything out on the bay is writing the same three.

## 4. `weather` has one dial and it turns two coats at once

`weather overgrown 0.16 ... on=crypt_damp` puts moss on the lowest courses, which is what this room
wanted, and it puts about five times as much LICHEN on with it — 7.9% of the part against 1.6% at
metre 16. There is no way to ask for one of the coats a weathering family carries and not the
other, and no way to say how far up it should reach except by the shape named in `on=` (which is
what I did: my own part intersected with the bottom 0.65 m).

Nothing here is broken. But a crypt is a room whose whole surface story is *water is coming through
the wall at the bottom*, and the only control over that is the amount and the mask.

---

## 5. A slab thinner than a voxel is dilated a whole voxel in every direction

`--part crypt_ch_water` at metre 16, on a water box 0.225 x 0.030 x 13.575:

```
worldbox   -0.125 -0.562 -8.562    0.125 -0.375 5.062
```

0.188 m tall for a shape 0.030 m tall — three voxels for half of one. The width is honest rounding
to voxel boundaries; the height is a voxel of dilation at each end. It is why the rill in this room
is water lying flush in the flags rather than water standing in a trough: there is no drawn top low
enough to survive it, because the drawn top would have to be at -0.50 and that is the underside of
the podium.

This has teeth well beyond head height. Every thin horizontal feature in this building — a glazing
bar seen edge-on, a fascia, a dentil bed, this room's own 0.01875 coffer steps — is being sampled
three voxels thick where it is drawn one, which inflates volume and surface and would flatter any
thin-matter measurement taken from `--part`. podium.clip reasons carefully about whether its 0.045
and 0.0375 grooves survive; that reasoning should be re-checked against this.

## 6. 375 voxels of air I could not get out of the void, and they were there before I started

`void_crypt` is one body at metre 8. At metre 16 it is eleven components — fourteen single air
voxels on the corners of the outer light-well shafts — and at metre 32 it is forty-eight, with 375
voxels stranded round the sarcophagus plinths and the shaft corners. They sit where an air face and
a stone face fall inside the SAME voxel and the difference resolves that voxel to air while all its
neighbours resolve to stone.

**They are not mine and they are not new.** The committed version of this fragment, measured on the
same binary, reports the same 48 components, the same 375 voxels, at the same coordinates. Widening
the grating rim from 0.05 to 0.12 — the obvious fix — moved the void volume by exactly zero,
because the extra stone lies outside `crypt_air` and the difference never sees it, so the rim is
back at the brief's 0.05.

What it costs the building is that many one-voxel bubbles buried inside podium stone, which nothing
can see. What it costs anybody measuring is that `components` on a VOID is not a clean gate the way
it is on a part, and no fragment can currently promise one.
