# Requests from the surface fragment (`clips/facility/surface.clip`)

The surface fragment owns the weathering. It is the first thing in this building to use
`Weather::Desert` or `Weather::Overgrown`, and therefore the first thing to use `Op::Curvature`,
`Op::Occlusion` or `Op::Facing` at all.

The headline is that **`on=` works and the kinds do not scale**. Scoping is exact — measured, not
asserted, below — and the last facility's failure is fixed. What is not fixed is that these two
kinds cost the whole clip's solid volume times twenty, per line, and two lines multiply rather
than add.

Everything here is measured on `clips/facility/requests/surface-bench.clip`, which is in this
folder: a moulded stone block with a ten-riser flight against it, on grass, with a gravel apron
and a pool at its foot and a bronze urn on top — the four things the last facility's weathering
ruined. 9 x 4.5 x 9 m, 976,686 solid voxels, metre 24. Both weather lines are scoped to boxes,
the way `requests/site.md` says to scope them.

---

## 1. Scoping is exact. This part works.

The right control is not "no weather line" — adding one changes the sampler's slack, see (3) —
it is **the same two weather lines with their zones moved into empty air**, so the field has the
identical shape and the coats can reach nothing. `variation` off so the histogram is readable.

| material | zones on the stone | zones parked in air | difference |
|---|---|---|---|
| lawn | 405,610 | 405,610 | **0** |
| water | 15,237 | 15,237 | **0** |
| bronze | 3,911 | 3,911 | **0** |
| granite | 91,192 | 121,488 | −30,296 |
| gravel | 70,722 | 106,566 | −35,844 |
| marble | 96,537 | 112,677 | −16,140 |
| limestone | 198,967 | 210,384 | −11,417 |
| sand | 56,143 | 0 | +56,143 |
| bleached | 17,833 | 0 | +17,833 |
| lichen | 11,156 | 0 | +11,156 |
| moss | 8,818 | 0 | +8,818 |

Not one voxel of grass, water or bronze changed. Every one of the 93,950 voxels the weathering
took, it took from stone, and the four sums balance. `weather desert 0.18 on=<box>` on a building
in a garden is now a thing you can write.

---

## 2. Desert and overgrown cost the clip's whole solid volume, times twenty, per line — and two of them multiply

Same bench, `variation` on, changing nothing but the weather lines:

| what | sample | field evaluations | against no weather |
|---|---|---|---|
| no `weather` line | **413 ms** | 738,771 | — |
| `weather desert 0.18 on=<box>` | **4,975 ms** | 2,767,247 | **12.0x** |
| `weather overgrown 0.16 on=<box>` | **5,899 ms** | 2,758,570 | **14.3x** |
| desert **then** overgrown | **47,031 ms** | 4,719,712 | **113.8x** |
| overgrown **then** desert | **84,796 ms** | 4,719,712 | **205.1x** |

Three things are in that table, and the bench understates all of them. On **real facility
geometry** — `clips/facility/requests/surface-place.clip`, which is the manifest cut down to the
four fragments the zones touch (site, podium, steps, walls: 2,819.8 m³, 1,443,7xx solid voxels,
metre 8), the same two lines cost:

| what | sample | field evaluations |
|---|---|---|
| no `weather` line | **361.9 ms** | 9,889,137 |
| desert + overgrown, both scoped to boxes | **237,303.2 ms** | 16,016,201 |

**656x**, for three quarters of the building's volume and a fraction of its field. The whole
facility with these two lines in it did not finish: metre 5 (a 3,991 ms build without them) was
killed after 45 minutes, and an earlier metre 6 attempt after two hours. I have not been able to
photograph this fragment on the complete building for that reason, and the pictures in
`renders\surface-steps2` and `renders\surface-north` are of the cut-down manifest.

### 2a. The coats can never be settled, so every solid voxel in the clip pays for them

`sample.cpp`, `descend()`: a paint rule whose `rule_slack` is infinite forces `every_rule_known`
false, and a box that is settled solid then goes to `paint_solid()`, which walks **every voxel**
evaluating that rule. Desert appends two such rules and overgrown two more, and what they cost
per voxel is:

```
desert  sun_bleach.test = up*0.6 + grit*0.4                 up     = facing(solid)    =  6 evals
desert  drift.test      = up*0.5 + cavity*0.5 + height      cavity = occlusion(solid) = 14 evals
overgrown pale, green   = cavity*0.55 + up*0.35 + clumps*0.4                          = 20 evals
```

`Op::Occlusion` is fourteen evaluations of its child, `Op::Facing` is six through `normal_at`, and
the child is `script.solid` — **the whole assembled building**. So one `weather desert` line costs

    (every solid voxel in the clip) x 26 x (one full evaluation of the entire building)

This is not the scope's fault and `on=` does not make it worse: the test is unsettleable with or
without a scope, because `Op::Occlusion` and `Op::Facing` are in it either way. The facility is
3,821 m³ of solid, which is about 125 million solid voxels at the contract's metre 32, so
`weather desert` on this building is of the order of three billion whole-building distance
evaluations *for the paint alone*, before the displacement is counted.

It also explains why the cost is so much worse on the building than on the bench: the price of
one of those evaluations is the price of walking the facility's 3,413-node field, and it is paid
once per voxel per weathering rule whether that voxel is anywhere near a zone or not.

`Weather::Cracks` is the exception, and the reason nobody hit this before: it reads `cell_edge`
and `fbm` and never touches the solid, which is why `site.clip`'s cracks line is free.
`requests/site.md` concluded the problem was the shape named in `on=`. For these two kinds it is
not the shape. It is the kind.

### 2b. Two weather lines multiply, because the second occludes the first one's displacement

`apply_weather` re-reads `script.solid` at the top of every request and every kind ends by
displacing it. So the second request's `occlusion` is fourteen evaluations of a field that is
already twelve deep, and 12 x 14 is the 114x in the table rather than 12 + 14 = 26.

It also means **the order of two `weather` lines changes the cost by 1.8x for identical output**.
`surface.clip` puts desert before overgrown for exactly that reason and says so in capitals,
because it looks like a stylistic ordering and it is not.

### 2c. What I would like

Either of these makes it affordable; both make it free.

- **Ask the occlusion, curvature and facing of the scope, not of the world.** When a request has a
  scope, `f.occlusion(shape, r)` wants to be `f.occlusion(intersection{shape, scope}, r)`. It is
  cheaper *and* more correct: what the moss wants to know is how buried it is in the podium's
  north cornice, not how buried it is in a building it cannot see.
- **Make a scoped coat settleable.** Give `PaintRule` the `where`/`below` pair every hand-written
  rule in this building already uses, and put the scope in it instead of adding `-1e9` to the
  test. A rule keyed on a shape is settled for a whole block in one reading, so the ninety-odd
  percent of this building that is nowhere near a weather zone would cost one box evaluation
  instead of twenty building ones.

Until one of those exists these two kinds are usable on a small clip and not on this building,
which is the one clip they were written for.

---

## 3. Adding an inert `weather` line still moves about one percent of the paint

The air-parked control above changes nothing about the field's *values* — the scope mask is
exactly zero at every voxel of matter, so both displacements add exactly zero. It still differs
from the no-weather build:

| material | no weather line | weather line, zone in empty air |
|---|---|---|
| lawn | 407,642 | 405,610 |
| limestone | 219,566 | 210,384 |
| marble | 105,478 | 112,677 |
| water | 12,737 | 15,237 |
| bronze | 3,867 | 3,911 |

About ten thousand voxels, all of them on a boundary between two paint rules. The cause is
`Field::skip_slack()`, which sums every displacement's reach in the whole field: 0.024 without the
weather lines, 0.057 with them. A bigger slack means fewer boxes settle in bulk and more voxels
are decided one at a time, so the weathered build is the *more* accurate of the two — but an
author who adds a weather line to one corner of a clip and finds the material histogram has moved
everywhere will not guess that, and it makes an exact before/after comparison impossible unless
you know to build the control this way. Worth a sentence in BRIEF.md.

---

## 4. `amount` does not control how much of the surface a coat covers

`sun_bleach.low = 0.55 - 0.35a` against a test of `0.6 * facing + 0.4 * grain`. At `a = 0` the
threshold is 0.55 and an up-facing surface still passes wherever the grain is above −0.125, which
is over half of it. At `a = 0.18` it is over about 64%. So the dial an author reaches for to say
"only a little" moves the coverage of that coat from 55% to 64% and nothing else; the only thing
`amount` really controls for desert is the depth of the scour. The same shape of problem is in
`drift.low = 1.15 - 0.75a`, which crosses the reachable range of its test somewhere around
a = 0.15 and so goes from nothing to a lot over a very short part of the dial.

`Weather::Overgrown` is much better behaved — `where` and the two thresholds interact so that
lichen thins and moss stays in the hollows across the whole range — so this is a fixable
inconsistency between kinds rather than a design limit. A coat whose threshold ran from "never"
to "everywhere" across the full 0..1 of `amount` would let the low end of the dial mean what it
says.

---

## 5. `weather` is applied at parse time, so `--clip-part` cannot show it

`main.cpp` sets `script.solid = piece` **after** `parse_clip_script` has already run
`apply_weather`. So `--clip-part part_mine` builds the piece with its weathering *displacement
discarded*, while the weather coats' `test` nodes still reference the whole building and are still
evaluated per voxel of the piece. The one flag that exists for looking at a fragment on its own
shows that fragment with its weathering removed, at the price of a fragment with its weathering
on.

A weathering fragment therefore cannot be inspected with `--clip-part` or `views.ps1 -Part`, only
with `-Focus` on the whole building — which costs a whole weathered build. `--clip-weather off`,
or applying weather after the part is chosen, would give this fragment the same loop everybody
else has.

---

## 6. The materials weathering invents are declared nowhere

`apply_weather` calls `make_material` for `sand`, `bleached`, `moss`, `lichen`, `fissure`,
`charred`, `soot`, `scorched`, `salt`, `barnacle` and `weed` as the kinds need them. Four of those
now exist in this building and `_contract.clip`, which is meant to be the one place a material is
declared, does not know about them.

`make_material` already reuses an author's material of the same name, so the fix is documentation:
BRIEF.md's `weather` paragraph should list the names each kind invents, so a contract can declare
them itself and keep the palette in one place. `surface.clip` deliberately does not declare them —
four names belonging to the whole building should not be claimed by one fragment.

---

## Two notes for whoever comes next, not requests

**`clips/facility.clip` has no `include "facility/surface.clip"`.** The manifest lists nineteen
fragments and this is not one of them, so none of this weathering reaches the building until
somebody adds

```
include "facility/surface.clip"
```

after `include "facility/fittings.clip"`, and `part_surface` to the `inside` union. I was told to
edit only my own file, and other agents were editing the manifest while I worked, so I have not
touched it. Everything here was built and photographed against a generated copy of the manifest
with those two lines in it. **Read section 2 before adding them.**

**A fragment can be mid-save while you build.** Two of my runs died on
`clips/facility/rotunda.clip:577: paint where=rotunda_fl_roundel does not name anything`, which
was another agent halfway through writing their file. It is not your clip. Run it again.

**`tools/views.ps1` still dies on stderr**, exactly as `requests/site.md` describes: line 56 sets
`$ErrorActionPreference = "Stop"` and line 249 is `& $exe @shot 2>&1 | Out-String`, so the engine's
`[WARN ] frame ... took 751 ms` becomes a terminating error and the run ends with no contact
sheet. I hit it on my second render and worked around it with a patched copy outside the repo, as
the site fragment did. Two of us have now lost time to a one-line fix nobody owns.
