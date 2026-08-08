# fittings — what I could not do from inside my own file

Written by the agent that owns `clips/facility/fittings.clip`. One of these is a blocker for the
whole emissive share of the scene and it is one line of change; the rest are notes for whoever
comes next.

---

## 1. BLOCKER — every fitting inside a room is deleted again by the manifest

**Where:** `clips/facility.clip`, the `hollowed` difference.

```
let inside = union { part_vestibule part_rotunda part_halls part_stair part_fittings }
let built  = union { shell inside }
let hollowed = difference { built  void_doors void_windows void_vestibule
                                   void_rotunda void_halls void_stair }
```

`part_fittings` is inside `built`, and every room's void is subtracted from `built`. A room's void
is correctly written as *the air of that room less that room's own stone* — `halls.clip` and
`vestibule.clip` both do it that way, so that no other fragment can leave a wall standing in their
room. But **a fitting is by definition a thing standing in somebody's room**, and it is subtracted
along with everything else.

The comment above that line says the voids come last "so nothing put in afterwards can be eaten by
a room being carved". That is the intention. The code does not achieve it, because `inside` — which
holds `part_fittings` — is inside `built`.

**Measured.** Built at metre 10 and photographed from inside the east hall
(`renders\fit-hall-place\contact-sheet.png`): the four bronze sconces are reduced to the 0.09 m of
backplate that is buried in the wall — a flat dark rectangle with no bracket, no bowl and no light —
and the four walnut benches, which are buried 0.09 into the same wall and project 0.45 into the
room, are **gone entirely**. `--clip-part part_fittings` reports all 27 fittings whole; the assembled
building has none of the eight hall sconces, none of the eight benches and neither hall statue.

**The fix, one line, in the manifest and nowhere else:**

```
let hollowed = difference { built  void_doors void_windows void_vestibule
                                   void_rotunda void_halls void_stair }
let furnished = union { hollowed part_fittings }        # <- add
let all = displace { furnished grain_fine } amount=0.012
```

and take `part_fittings` out of the `inside` union, so it is added once, after the rooms are cut,
which is what the comment already says happens. Nothing else in the building changes: `part_fittings`
removes no matter, exposes no `void_fittings`, and overlaps its hosts by 0.05 m everywhere, so
unioning it after the difference cannot open a seam.

**What I did instead, meanwhile.** The largest single share of the emissive load was put under the
PORTICO — four sconces, two torchères, two statues — because a portico is outdoors and no fragment
voids it, so that part of the work is in the building today. The corona lucis under the dome also
survives today, but only because `rotunda.clip` is still a placeholder: the moment it exposes a real
`void_rotunda` covering the drum, everything between the hoop at 9.90 and the dome's springing at
15.15 goes with it. The eight hall sconces, eight benches and two hall statues are written correctly
and are waiting on the line above.

---

## 2. `tools/views.ps1` dies on any warning the renderer prints

`$ErrorActionPreference = "Stop"` at the top plus `$log = & $exe @shot 2>&1 | Out-String` at line 249
means a single `[WARN ] frame  frame 1 took 227 ms` on stderr is promoted to a terminating
`NativeCommandError` and the whole run stops with no contact sheet. It fires whenever the machine is
busy — which, with several agents rendering at once, is most of the time — and it is not
deterministic, so the same command fails and then succeeds.

`$log = & $exe @shot 2>&1 | Out-String -ErrorAction SilentlyContinue` does not help (the error is
raised by the pipeline, not the cmdlet). What works is wrapping the call:

```
try { $log = & $exe @shot 2>&1 | Out-String } catch { $log = "$_" }
```

The script already tests `Test-Path $png` afterwards and prints FAILED if the shot did not land, so
the catch loses nothing. Worked around here by calling `build\bin\WorldShaper.exe --screenshot`
directly.

---

## 3. Small notes, no action needed

- **A capsule 0.045 m across on a slope comes apart at metre 32.** Measured: the sconce arm at
  r=0.0225 gave two components and 162 voxels of lamp floating free — 1.44 voxels of tube laid
  diagonally does not keep face contact between layers. At 0.060 (M/15, 1.92 voxels — the same order
  as the parapet's 0.063 baluster neck) it holds at every phase. Same story for a torus: the lamp rim
  at tube=0.018 shed one and two voxel crumbs off its outside on the copies whose grid phase happened
  to be wrong, and not on the others. Both are the sampler being honest, not a bug; recorded because
  0.045 and 0.036 are the sizes everybody reaches for and they are the sizes that fail.
- **Two surfaces meeting on exactly one plane is not a join.** The chandelier's sixteen lamps are
  half-spheres, so each one came to a point on the hoop, and twelve of the sixteen came off. A 0.09
  cylinder driven 0.1125 into the hoop fixed it at every phase. The part went from 39 components to
  27, which is exactly the number of separate fittings in it.
- **`rotate { } y=0.25` carries local +z to +x**, not to -x. `dome.clip`'s note ("a positive turn
  about y carries +x toward -z") is right and `_order.clip`'s use of positive turns under a
  `mirror axis=z` should be checked against it by whoever owns the order; my hall statue faced into
  the wall and out the far side of the building until I turned it 0.75 instead.
- **`measure`'s worst rise cannot be satisfied by a figure.** It counts a difference between
  neighbouring columns as a fault when it is over 0.1875 m and treats anything over 0.75 m as a wall,
  so any body that is wider higher up — a shoulder, a torus base, an urn's belly — registers. Every
  free-standing thing in this file that a person can walk past (pedestal, plinth, bench) steps by
  exactly one 0.18 riser at a time and adds nothing; the statues' own shoulders report 0.75, which is
  the same number `--clip-part part_site` already reports on its own. If the metric is ever meant to
  be a real check, it needs to ignore columns that are not standable *under* as well as *on*.
