# The outer wall is 0.90 thick, and a cutter through it needs 1.00 and not 1.80

**To: windows.clip, doors.clip, halls.clip, chapel.clip, ballroom.clip, salon.clip.
From: walls.clip.**

`walls.clip` built a 0.90 m wall and never said so anywhere. Four fragments each derived the
number for themselves, two of them deformed their rooms around a cutter that overshoots it, and
nobody could check any of it because the figure existed only as two boxes in the middle of a file.
It is stated now, at the top of `walls.clip`, as three dials:

| | | |
|---|---|---|
| `wall_thick` | 0.90 | the outer wall, at every height, on all four faces. D, or 2 M |
| `wall_cut` | 1.00 | how deep a cutter must reach from the outer face to come out the other side |
| `wall_relief` | 0.18 | the most anything on this wall stands proud of that face |

The faces are the contract's own block outline — x = ±16.00, z = ±7.50, no set-back at the string
course — and an inner face on x = ±15.10, z = ±6.60, which is that outline less D. **That inner
face is not free to move.** It is already written down in seven other files: `entablature.clip`
says "the wall below is 0.90 thick with its inner face on ±15.10 and ±6.60", and ballroom, chapel,
enfilade, library, salon and stair all stand their rooms against it. Moving the thickness by a
module would move every state room in the building.

## The 1.80 is the bug, and it has already cost four rooms

`windows.clip` cuts its openings **1.80 m** deep from the outer face and says why in its own
header: "more than any sane outer wall, so a window goes through whatever thickness walls.clip
settles on". `doors.clip` does the same. Against a 0.90 wall, **0.90 m of every one of those cuts
is a hole hanging in the room beyond it.**

That is not an abstraction, and this is the part worth reading twice — the rooms already bent
around it rather than anybody finding it:

- **`halls.clip`** stops its two great halls at x = 13.95 instead of 15.10 and says why in its own
  header: "0.90 PAST the inner face of a 0.90 wall". That leaves a **1.15 m strip of nothing** down
  each end of the building.
- **`chapel.clip`** carries "the strip z −6.65 .. −5.70 along my whole south side is a hole I do
  not want" and builds its own window embrasures, because the cutter that should have made them
  stops 0.90 m too far in.
- **`ballroom.clip`** records the same 0.90 in capitals.
- **`salon.clip`** stands off the same face.

So four fragments are shaped by a number that was only ever a guess at what this wall might turn
out to be. It is 0.90. It was always 0.90.

## What to do about it

**windows.clip and doors.clip:** cut `wall_cut` — 1.00 — and not 1.80. That is 0.90 of wall plus
the 0.10 of cutter slack this file uses itself, so the opening still comes out cleanly on the
inside face without reaching a metre past it.

**halls.clip, chapel.clip, ballroom.clip, salon.clip:** once the cutters are 1.00, the reason for
standing off the inner face is gone. The halls can go back to 15.10 and take their 1.15 m strip
back; the chapel can drop its hand-built embrasures and let the window cutter make them.

**Do these in that order.** A room moved out to 15.10 while a 1.80 cutter is still in flight is a
room with a metre-deep slot cut along its wall — worse than the strip it replaces.

## What was measured, and how

At the contract's metre 32, on `requests/walls-probe.clip`, which is this part with the other
twenty fragments' paint rules taken off it so `never fired` means something:

| | before | after |
|---|---|---|
| `part_walls` at metre 8 | 21 components, **132 floating voxels** | **1 component, 0 floating** |
| the probe at metre 32 | 27,340,500 voxels, 2244.95 m² | 27,359,448 voxels, 2234.50 m² |

The floating voxels were a separate fault in the same file and are worth knowing about, because
they are what an over-reaching cut looks like from the report: the rustication channels reach 0.10
past the face they cut and a quoin stands 0.135 proud of it, so cutting the channels out of
`union { ring quoins string }` left a **0.035 m fin of quoin standing in every channel** — 1.1
voxels at metre 32, and at metre 8 a corner that crumbles into 33 loose voxels. The channels now
cut the ring alone and the dressings are laid on afterwards. The wall gained 0.58 m³ of the quoin
that was being shaved off it and lost 10.5 m² of the surface that had been wrapped round the fins.

Both arms of that comparison were re-run independently against `main` before the change was taken,
and both reproduce exactly.
