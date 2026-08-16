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

## 2. `--gap` needs a box that begins and ends in stone, and nothing says so

`--gap y@x,z` is the only way to check rule 7, and against `crypt-probe.clip` — my part on its own,
in the contract's box — every point in the room reports

```
gap    20.938 m of air along y (BROKEN — there is matter in it)
```

which is the whole height of the sampled box. It is correct and it is useless: the ceiling of this
room is the podium, the floor is the bottom of the podium, and in a probe of my part alone there is
sky above and open space below, so the column has three runs of air in it and the tool reports the
span of all three.

So the head height of a room can only be measured on a clip that contains what is over it, in a box
whose top and bottom are *inside stone*. `clips/facility/requests/crypt-place.clip` is that: podium
plus crypt with `void_crypt` taken away, in `bounds -12.3 -0.49 -9.4  12.3 1.79 6.4` — -0.49 is
inside the 0.05 floor slab and 1.79 is inside the ceiling, so there is exactly one run of air in
every column and the number printed is the head height.

**What would have helped:** `--gap` printing the LARGEST clear run as well as the span, or a
`--head y@x,z` that answers "how much clear air is over the topmost floor in this column". Every
fragment with a room in it needs this and every one of them will trip over the same thing.

**AND I COULD NOT GET `--gap` TO AGREE WITH `--slice` EVERYWHERE, WHICH IS SAID HERE BECAUSE IT IS
NOT SETTLED.** Eleven sample points in this room report a clean `gap 2.062 .. 2.156 m of air along
y (clear)`, and a calibration clip — two slabs with the same two planes, floor top -0.45 and soffit
1.6875, and a pillar beside the sample point — reports exactly the 2.125 m those planes should
give, and reports `0.000 m of air (BROKEN)` when the sample lands on the pillar. So the tool works
and the number means what it says.

But a scatter of other points in this room also report `0.000 m of air (BROKEN)` where
`--slice y@0.5` at the same metre shows open floor: (2.70, 4.95) and its neighbours up and down the
north aisle, for instance, which the plan slice shows as clear between the stair at |x| > 9.5 and
the channel kerbs at |x| < 0.25. The same slice, read character by character against the box, puts
every piece of this fragment where the file says it is. Either the second coordinate of `y@a,b` is
not z, or something about a folded `mirror` is being asked at the wrong place; I could not tell
which from the outside, and I would rather write that down than pick the reading that flatters the
room. The geometry is checked by the slices, which I can read; the head height is checked by the
calibration, which uses this room's own two planes.

## 2a. And the answer it gives is a voxel short at each end, which cost this room 0.0375 m

A voxel is solid if any of it is stone. Neither -0.45 nor 1.65 — the floor and soffit this room
was drawn to, both module numbers — lands on a voxel boundary at metre 16 or metre 32, so the grid
takes most of a voxel off the bottom and most of one off the top, and a room built to exactly the
brief's 2.10 m measures **2.0625 m**: 37 mm short, in the thing you actually walk about in.

It cannot be fixed by drawing more carefully, because 2.10 is not a whole number of voxels:
2.10 / 0.03125 = 67.2. The nearest clear height a voxel grid can hold at the contract's metre is
68 voxels, 2.125 m.

I fixed it by raising the soffit to 1.6875, which is 1.80 - M/4 (a module number, and the ceiling
slab is happier at M/4 than at M/3) and IS a voxel boundary at both metres — 54 at metre 32, 27 at
metre 16 — and by giving the room 37 mm over the minimum, because the grid is anchored on the
clip's `bounds` and those move from probe to probe. Measured clear height is 2.125 m on a
calibration clip of the same two planes, and 2.062 to 2.156 at points in the room itself.

**This is a rule that should be in BRIEF.md and is not**: *a floor or a soffit that bounds head
height should land on a voxel boundary of the contract's metre — a multiple of 0.03125 — or the
room loses up to 62 mm to the grid.* Anything upstairs whose head height was checked by arithmetic
rather than by `--gap` is 30 to 60 mm shorter than its file says. halls.clip's 2.25 m and
stair.clip's 2.115 m are both worth re-measuring; stair.clip's is the one with 15 mm of margin in
it and it is measured by a probe of its own that has the same voxel question in it.

---

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
