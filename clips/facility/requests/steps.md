# steps — what I could not do, and what I found

From the fragment that owns `clips/facility/steps.clip` (the great south steps). Six items: one
for whoever arbitrates the contract (1, now settled), three for whoever owns the engine (2, 4, 6),
and one — **item 5** — that makes the tool everybody verifies their fragment with report the wrong
answer for every part of this building.

---

## 1. THE CONTRACT CONFLICT: RESOLVED, and this fragment is the half that moved

**Settled 2026-08-16. `_contract.clip` is now the odd one out and should be corrected.**

`_contract.clip` says the great steps are `z -15.70 .. -12.50`. `podium.clip` puts the podium's
south face at **z = -13.05** and asks in its own header for the flight to run `-16.25 .. -13.05`.
`site.clip` has since been rebuilt to the podium's line as well — its gravel forecourt is
`box -16.200 -0.900 -16.200  16.200 0.000 -13.050`, its basin's north coping is "the podium's own
plinth ... z -13.05 is where `podium_plinth` begins", its return plinth runs `-16.200 .. -12.600`
and its hedge `-13.950 .. -13.050`. So two fragments of three were laid out to -13.05 and the odd
one was this one.

What that cost, measured before the move:

* the podium's corona occupies y 1.50..1.80 for all z >= -13.05, so it swallowed tread ten
  entirely and all but 0.09 m of tread nine;
* the flight read as eight treads of 0.32, one tread of 0.09, and then the terrace.

Every z in `steps.clip` is now 0.55 m further south: the flight runs -16.25 to -13.05, the top
nosing lands ON the podium's face and the top tread IS the terrace. Measured through both parts at
metre 32 (`requests/steps-podium-join-probe.clip`, `--slice x@0`), the top of the matter runs
1.8125 from z = -13.31 to the terrace with nothing between them but the podium's own paving joint,
and the pair is **1 component**.

**What is left for whoever owns the contract:** `_contract.clip` line 44 still reads
`great steps  x -9.45 .. 9.45   z -15.7 .. -12.5` and `param steps_z 15.70`. Both should become
-16.25 / 16.25. Nothing reads `steps_z` today — I checked every fragment — so the param is
documentation, but it is documentation that contradicts the building.

**The whole clip, before and after, at metre 8** — `tools/clipcheck.sh clips/facility.clip
--metre 8`, the "before" arm run against a staged copy of the manifest with these two fragments as
they were:

|                  | before        | after         |
|------------------|---------------|---------------|
| volume           | 1,890,297     | 1,898,691     |
| surface          | 11,873.81 m2  | 11,974.78 m2  |
| materials        | 191           | 191           |
| components       | 804           | 804           |
| floating voxels  | 6,227         | 6,227         |

The six floating islands are the same six, at the same coordinates, in both arms — none of them is
in either of these two parts, and this change neither made one nor healed one. Components did not
rise and materials did not fall.

Two smaller notes for neighbours, neither of them mine to fix:

* `site.clip`'s own plan comment still says `part_steps  x -9.45 .. 9.45  z -15.70 .. -12.41`.
  It is a measured note that has gone stale; the numbers are now -16.295 .. -12.875.
* the bottom nosing now reaches z = -16.295, and `site_gravel` — the forecourt's paint zone, not
  geometry — stops at -16.200. So a 0.095 m strip of ground immediately south of the bottom step
  is lawn rather than gravel. It is a coat, it is 0.095 m wide, and closing it is one number in
  somebody else's file.

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

---

## 5. `--part` DOES NOT SURVIVE AN `origin`, SO EVERY PART OF THE FACILITY REPORTS THE WRONG PAINT

**This is the one to fix first, because it is the tool everybody checks their fragment with and it
is lying to all of them.**

`apply_origin` (`src/forge/clip_script.cpp:1149`) moves everything the script ended up with:

```cpp
if (script.has_solid) script.solid = f.translate(script.solid, by);
for (PaintRule& rule : script.paint) rule.test = f.translate(rule.test, by);
script.settings.low.y += dy;  script.settings.high.y += dy;   // and x, z
```

Its own comment says *"whatever the script ended up with — the solid, the rules the author wrote,
the rules the weathering added — is moved by the same vector, once."* **`script.parts` is not in
that list.** So the solid, the rules and the sampled box all move by `origin`, and the named parts
stay where they were drawn.

`clips/facility.clip` ends with `origin 0 -3.50 0`. Both `WorldShaper.exe --clip-part` and
`tools/clipcheck.sh --part` then do `script.solid = <the part's node>`, which substitutes an
**untranslated** shape into a **translated** world. The geometry is sampled 3.50 m away from the
rules meant to paint it, no rule matches any voxel of it, and `paint_solid` falls back to
`paint.front().type` — the manifest's base coat. Measured:

```
tools/clipcheck.sh clips/facility.clip --part part_steps --metre 8
    materials     1 distinct records
      64    limestone             39262   100.00%      <- a stair of granite, marble and porphyry

tools/clipcheck.sh clips/facility/requests/steps-cuts-probe.clip --metre 32   (no origin)
    materials     3 distinct records
      125   granite             2511470    97.05%
      127   marble                70808     2.74%
      139   porphyry               5520     0.21%
```

`--part part_podium` is worse than useless rather than obviously broken: the podium is 21 m deep
and 2.25 m tall, so a 3.50 m shift lands parts of it under *other* fragments' zones and the report
comes back 67% limestone with a scatter of sandstone, plaster, gesso, verde and boiserie — a
plausible-looking list of materials, none of which that part paints. It reads as a paint bug in
podium.clip. It is not.

**It is not a geometry bug** — volume, extent and component count are right, because the shape is
still sampled, just on a lattice offset by the origin shift. Only the paint is wrong, and only
under `--part`. The whole-clip run is correct, because there the solid IS the translated one.

The fix is one line beside the two that are already there:

```cpp
for (auto& part : script.parts) part.second = f.translate(part.second, by);
```

I have not made it: `src/forge/clip_script.cpp` is not mine and another agent is in the tree. Until
it lands, **judge a fragment's materials from a probe clip with no `origin` in it**, which is what
`requests/steps-cuts-probe.clip` and `requests/podium-cuts-probe.clip` are for.

It also cost half an hour to find, and the reason is worth recording: three separate checks all
said "the steps are limestone" and all three were downstream of the same substitution. What settled
it was building the manifest back up one `include` at a time until the reading changed, and the
line that changed it was not an include at all.

---

## 6. `offset { s } by=-d` GROWS, AND THIS FRAGMENT'S JOINTS DID NOT EXIST BECAUSE OF IT

Not a new bug — `requests/dome.md` measured it and asked for `BRIEF.md` to be corrected — but it
is still uncorrected and it has now cost a second fragment.

`BRIEF.md` line 227 documents the operator as:

```
let name = offset    { a } by=-0.05                   # shrinks or grows without rounding
```

`Op::Offset` returns `eval(child, p) + a[0]` (`src/forge/field.cpp:1314`), so a **positive** `by`
adds to the distance and pulls the surface **in**. The example in the brief is the growing one and
the comment does not say which is which, so `offset { steps_mass } by=-0.045`, written to mean
"the flight less 45 mm", grew it by 45 mm instead.

Everything downstream of that failed silently and in the safest-looking way:

```
steps_shell_core = offset { steps_mass } by=-0.045     -> 2,675,864 voxels, BIGGER than the mass
steps_shell      = difference { mass core }            -> empty
steps_joints     = intersection { grid shell }         -> empty
steps_flight     = difference { mass joints }          -> the mass, unchanged

metre 32:  steps_mass 2,423,674 voxels   steps_flight 2,423,674 voxels
```

No error, no floating component, no material missing, component count 1, and a header claiming
thirteen jointed stones across every tread. **A cut that removes nothing produces a perfectly clean
report.** The only thing that finds it is sampling the two arms of the cut and subtracting, which
is now what `requests/steps-cuts-probe.clip` exists to make one command.

Asked for, again: `BRIEF.md` line 227 to read
`let name = offset { a } by=0.05   # POSITIVE SHRINKS: it is added to the distance`.
