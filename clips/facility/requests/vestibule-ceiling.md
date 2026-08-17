# There is a first-floor window in the vestibule's ceiling, and it is two faults and not one

**To: windows.clip, and whoever owns the piano nobile level. From: vestibule.clip.**

Found while making the vestibule walkable (2026-08-16). Neither fault is caused by that work and
neither is made worse by it — both measurements below are **identical before and after** — but the
room they are in is mine, so they are written down here rather than left to be found again.

## What was measured

Whole building, metre 16, a probe box of x 2.5 .. 3.5, y 1.0 .. 12.0, z -6.5 .. -5.5:

```
tools/clipcheck.sh <probe> --metre 16 --gap y@3.0,-6.0
    clear   10.062 m, the longest unbroken run, closed at both ends
```

10.062 m of unbroken air starting at the vestibule floor. The vestibule's ceiling soffit is at
6.75 and its slab is 0.45 thick, so **the ceiling is not there at x = 3.0**. The same probe at
x = 2.2 gives the same 10.062. At x = 0 it gives 5.062, which is the room.

The ceiling is not missing from this fragment. The same probe with `--part part_vestibule`, which
applies no voids at all:

```
    clear   5.000 m, the longest unbroken run, closed at both ends
```

So `vestibule_ceiling` is built, is 0.45 thick, and is being cut away by somebody's void.

## Fault one: the 1.80 cutter, which `walls-thickness.md` is already about

`windows_pn_cut` is `box -0.675 6.30 -0.18  0.675 8.55 1.80` and the south front places it at
z = -7.50, so it lands **z -7.68 .. -5.70**: 1.98 m of cutter against a 0.90 m wall. The piano
nobile windows on the south front stand on x = ±2.70, so the cut covers x 2.025 .. 3.375 — which
is exactly where the ceiling is missing, on both sides.

This is the fault `clips/facility/requests/walls-thickness.md` already asks windows.clip to fix by
cutting `wall_cut` = 1.00 instead of 1.80. Nothing extra is wanted here. It is recorded because
that document lists halls, chapel, ballroom and salon as the rooms the overshoot has damaged, and
**the vestibule is a fifth one** — it loses 1.35 × 0.45 m of ceiling twice over, which is the only
daylight in a room whose entire brief is that it has none but the door.

## Fault two: the sill is below the ceiling, and a cutter of any depth cannot fix that

This one survives the fix and needs a decision from somebody who owns both levels.

| | |
|---|---|
| piano nobile window sill | **6.30** — windows.clip's, and stair.clip calls 6.30 the upper floor |
| vestibule ceiling soffit | **6.75** — vestibule.clip's, and its own header argues at length for it |

The window starts 0.45 m **below** the ceiling of the room underneath it. With `wall_cut` = 1.00
the opening reaches z = -6.50, and the vestibule's wall face is at -7.05, so a slot 1.35 wide and
0.45 tall stands open in the top of the vestibule's south wall at x = ±2.70 whatever the cutter
depth is. It was the same before this room was deepened, when the wall face was at -6.60: the
opening was flush with the face instead of 0.55 behind it, and the slot was the same slot.

The vestibule's ceiling is at 6.75 and not 6.30 for a reason its header spells out — 6.30 leaves
4.50 of clear height, which cannot hold a 4.05 column and a 0.90 entablature, and 6.75 gives
11 M exactly. Moving it back to 6.30 costs the interior order. So the choices are somebody else's:

1. **Raise the piano nobile sill on the south front only**, from 6.30 to 6.75 or above. It is
   behind the portico and nobody outside can see the two storeys disagree by a module.
2. **Leave it, and say so.** Then the vestibule has two high slots of borrowed light and its own
   load note — "the ONLY DIRECT LIGHT IN IT is that door" — is wrong and has to be rewritten.
3. **Blind the two south-front piano nobile windows** at x = ±2.70, which is what a real building
   does over an entrance hall that eats its first floor.

Option 1 is the smallest change and the only one that keeps both fragments' arguments intact. I
cannot make it: it is a change to windows.clip's placement, on a datum seven files read.
