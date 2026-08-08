# steps — what I could not do, and what I found

From the fragment that owns `clips/facility/steps.clip` (the great south steps). Three things for
whoever owns the engine, one for whoever arbitrates the contract.

---

## 1. THE CONTRACT CONFLICT: where does the flight land? (needs a decision, not a fix)

`_contract.clip` and my brief both say the great steps are `z -15.70 .. -12.50`, ten risers of
0.18 and ten runs of 0.32, top landing flush at y = 1.80.

`podium.clip` has moved the podium's south face to **z = -13.05** (plinth and corona) and
**z = -12.60** (rusticated die) — 0.55 m south of the contract's -12.50 — and its header asks for
the flight to run `z -16.25 .. -13.05` instead. `site.clip`, meanwhile, states in its own header
that it assumes the contract: "the podium's south face is at z = -12.50 (contract)" and "the great
steps occupy x -9.45..9.45 and z -15.70..-12.50 (contract). Nothing of mine is inside that." Its
gravel forecourt and its gate piers are laid out to that line.

Two of three fragments are on the contract, so **steps.clip is built to the contract**. The cost of
that, until somebody moves, is visible and worth stating exactly:

* the podium's corona occupies y 1.50..1.80 for all z >= -13.05, so it swallows tread ten
  entirely and all but 0.09 m of tread nine;
* the flight therefore reads as eight treads of 0.32, one tread of 0.09, and then the terrace;
* every riser is still exactly 0.18, so the walkability number this fragment exists to set is
  unaffected. It is a looks problem, not a measurement problem.

If the podium is right and the contract is wrong, the fix on my side is three characters per line:
subtract 0.55 from the ten tread z values, from the two trim frames, and from the pedestal's
translate. If the contract is right, the podium's south face wants to come back to -12.50. Either
is one small edit. **What must not happen is both of us moving.**

Related, and much smaller: `site.clip` puts its gate piers at x 9.45..9.90, z -16.20..-15.75, which
leaves a 0.05 m slot in z between them and my step pedestals at x 8.55..9.45, z -15.70..-14.74.
Two stone blocks 50 mm apart reads as a construction fault. I have not chased it, because the
0.05 is theirs to close (their pier is the one that stops short of a contract line) and because
moving mine would break the pedestal's alignment with the bottom riser.

---

## 2. `sd_stairs` OVER-STATES DISTANCE NEAR A RISER (a real bug, and the reason I did not use it)

`src/forge/field.cpp`, `sd_stairs`:

```cpp
const f64 tread = std::floor(u / run);
const f64 top   = (tread + 1.0) * rise;
const f64 box   = sd_box(p, half);
return std::max(box, v - top);
```

`v - top` is the point's height above the tread it stands over. For a point just *south* of a riser
and well above the tread below it, that is not the distance to the shape — the riser face is
closer, horizontally. Concretely, with run 0.32 and rise 0.18: a point 0.01 m south of a riser and
0.17 m above the tread below it is 0.01 m from solid stone, and `sd_stairs` returns **0.17**.

That is an over-statement, which is the dangerous direction — the same direction `metric_slack`'s
own comment on `Op::Repeat` calls out, in this file, twenty lines away: "it says 'nothing near
here' when there is something near here, and a sampler that believes it skips over the slat."
`metric_slack` currently treats `Op::Stairs` as an exact primitive (it falls through to the
primitive default), so a sampler is entitled to believe it. A flight built with `stairs` and
sampled with skipping enabled can lose slivers of riser face, and the wedge of points affected is
exactly the thin wedge in front of every riser in the flight.

The honest value is `max(box, min(v - top, horizontal distance to the riser plane ahead))`, which
costs one `fmod` that is already computed. Failing that, `metric_slack(Op::Stairs)` should report
the rise as slack so nothing settles a box against it.

I used ten explicit boxes instead. That is exact, it settles boxes, and it also guarantees what
this part is judged on: ten treads that are identical to the last decimal.

---

## 3. THE WALKABILITY METRIC IS SET BY ORNAMENT LONG BEFORE IT IS SET BY STAIRS

Not a bug, but it decides what can be built, and everyone building an ornament should know it.

`walkability()` counts a rise between neighbouring columns only when it is `<= max_step * 4`, so at
metre 32 the window is **0.1875 m to 0.75 m**. Anything in the building whose top surface differs
from its neighbour's by an amount in that window becomes the reported worst rise, and the stair —
whose whole purpose is to be that number — is buried.

The trap is that almost every free-standing ornament falls in the window automatically. Any object
whose widest point overhangs its own foot puts a rise beside itself equal to the height of that
widest point above the surface it stands on. The order's `urn` is widest 0.585 m above its base, so
one urn standing on a flat cap reports a 0.59 m worst rise for the entire facility. `site.clip`
stands two of them on its gate piers, so as of now **the facility's worst rise is already being set
by an urn, not by these steps**, and it will read about 0.59 m rather than 0.19 m.

Three ways out, and somebody should pick one:

* **Report both numbers.** The worst rise *on a reachable surface* is the walkability question;
  the worst rise anywhere includes every vase and finial in the building. The flood already knows
  which columns are reachable; scoring the rise only over reachable pairs would have reported
  0.19 m for this building at the first attempt.
* **Exclude columns whose top is not level enough to stand on** — a surface whose neighbours in
  all four directions disagree is a lip, not a floor.
* **Leave it, and accept that the number means "worst rise including ornament".** In that case say
  so in the report line, because "worst rise 0.59 m" reads as a broken staircase.

Measured on this fragment alone (`--clip-part part_steps`, metre 32): **1 component, worst rise 6
voxels = 0.19 m**, which is exactly what 0.18 quantises to at that resolution — 0.18 x 32 = 5.76,
and no placement of the treads can make ten consecutive 0.18 rises land on 5 voxels every time.
0.19 is the honest reading of a genuine 0.18 riser and the metric cannot do better; it is worth
knowing that before somebody files "the steps report 0.19, not 0.18" as a fault.

---

## 4. SMALL THINGS

* **`views.ps1` cannot render at metre 32.** `-Metre 32` on this clip writes no PNG at all and the
  script reports FAILED for every view with no error line, because the dense array for the
  contract's bounds is about 2.8 GB before the renderer's own allocations. Metre 16 is the ceiling
  for anything that goes through the renderer; `--clip-metre 32` for *measurement* is fine, since
  measurement does not build the world. Worth a line in the script that says so rather than eight
  silent FAILEDs.
* **PowerShell 5.1 turns any stderr line from the exe into a terminating error** inside
  `views.ps1`, because the script sets `$ErrorActionPreference = "Stop"` and line 249 does
  `& $exe @shot 2>&1`. A single `[WARN ]` about a slow first frame therefore kills the whole run
  after it has already written its PNGs. Redirecting to a file, or `$ErrorActionPreference =
  "Continue"` around that one line, would fix it.
* **`_contract.clip` is spliced into the clip once per fragment** — twenty-odd times — because
  `expand_includes` guards only against cycles, not against a file already pulled in. It costs
  nothing visible today (every `param`, `material` and `let` is simply redefined to the same
  thing), but a `#pragma once`-style guard would make the parse cheaper and would turn an
  accidental *conflicting* redefinition into an error instead of into a silent last-one-wins.
