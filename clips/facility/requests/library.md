# library.clip — what I needed the language to do and it could not

## 1. `mirror` folds to the positive side, and nothing says so

`mirror { a } axis=x` evaluates the child at `|x|`. A shape drawn entirely at negative x therefore
appears **nowhere at all** — not on one side, not on the other. My reading desks arrived with no
legs, twice, and the only sign of it was 2,844 floating voxels reported at the height of the desk
rail. BRIEF.md's grammar line says "folds the coordinate: appears both sides", which is true and is
also exactly the sentence that makes you think a shape at -0.36 will be folded to +0.36.

**What would help:** either a warning when a `mirror` child's bounding box lies wholly on the
negative side of the fold (it can only ever produce nothing), or the grammar note reading "folds
the coordinate — DRAW THE CHILD ON THE POSITIVE SIDE".

## 2. There is no way to clip a repeated field without bisecting its members

The books are a `repeat` intersected with a zone box, which is the only way to give one rank a
different length on each wall. Where the zone edge falls inside a spine it leaves a sliver 0.01 m
wide: one or two voxels, floating, invisible, and counted. I fixed it by hand — every zone edge in
this file is on a period boundary, and the two book periods have a gap at their end so that a
period boundary is always in air — but that arithmetic is done four times in this file and will be
done again by everybody who repeats anything.

**What would help:** a key on `intersection` (or on `repeat`) meaning *keep only whole copies* —
drop any member of a repeated field that the clip would cut. `repeat { a } x=0.27 nx=17 whole=1`.

## 3. A moulding cannot go round a room, and a frame cannot be moulded

`fillet`, `ovolo`, `cyma_reversa` and the rest are straight runs; four of them and a backing box
make one cornice, and this room has two cornices, so that is ten bindings for two mouldings. The
`difference { box box }` frame trick lines all four walls at once and mitres for free, but a frame
can only be square in section.

**What would help:** a moulding that takes a `difference`-of-boxes frame as its path, or a
`revolve`-like operator that sweeps a section round a rectangular plan. Every interior fragment in
this building has written the same ten lines.

## 4. A face that lands on a voxel boundary produces speckle, not a surface

The gallery brackets had `round=0.010` and a bottom face at 8.550. The bottom row of voxels came
back as a scatter of 8-voxel islands rather than a face — the rounded arris pulls the surface in by
a hair, and whether a voxel centre 0.001 inside survives is decided per column. Removing the round
fixed it. The same class of thing bit the books: a spine lapping its shelf by 0.03 laps it by 0.96
of a voxel at metre 32, and whether the shared voxel exists depends on where the shelf falls
against the grid — six whole periods of six books came back adrift, at two shelf levels and not at
the other three.

**What would help:** `clipcheck` already knows both numbers. A note like
`overlap 0.030 m is 0.96 of a voxel at metre 32 -- may not join` would have saved two hours, and it
is the same check the tool already makes for `displace amount`.

## 5. What I could not check here at all

No Windows and no Vulkan, so there is no contact sheet and no render. Everything in this file about
how the room LOOKS — whether a rank of spines reads as books or as a comb, whether the fresco
panels read at all from the floor, whether the light well is too narrow — is unverified. The
geometry is measured; the appearance is not.

## 6. The brief's own numbers, corrected in the file

Three figures in my brief do not survive contact with the module or with `clipcheck`, and the file
says so where it uses the corrected ones:

- the gallery at **8.85**: 6.30 + 2.55 and 2.55 is 5.667 M. Built at 9.00 (6 M), which is also the
  only level that leaves 2.10 under the deck.
- the vault springing at **9.30**: 6.30 + 3.00, and 3.00 is 6.667 M. Built at 10.35 (9 M).
- the stair gallery at **z -6.57 .. -4.74**: `--part part_stair` reports worldbox z 3.000 .. 6.875.
  The stair is on the NORTH front; the z in the brief is sign-flipped. The doorway is therefore in
  my north wall at the west end, on the only route from that gallery to this room.
