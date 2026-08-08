# Requests from the site fragment

Four things, in the order I would fix them. The first two are the expensive ones; the third can
silently cost another agent an afternoon; the fourth is a measurement one fragment can break for
everybody.

---

## 1. `weather ... on=<shape>` is priced by the complexity of the shape, per voxel of the whole clip

**Measured**, on `part_site` alone at metre 16, everything else held constant:

| what | sample time |
|---|---|
| no `weather` line | 2,722 ms |
| `weather cracks 0.10 scale=0.5 on=<four boxes>` | 1,453 ms (within noise of no weather) |
| `weather cracks 0.09 scale=0.45 on=<union of my stonework — 24 revolved balusters>` | **574,333 ms** |

Same kind, same amount, same stone. Two hundred and eleven times.

**Why.** `apply_weather` in `clip_script.cpp` builds

```
elsewhere = smoothstep(scope, 0, band)
banish    = multiply { elsewhere, -1e9 }
only_here(test) = add { test, banish }
```

and every coat's `PaintRule.test` becomes that. `metric_slack(Op::Smoothstep)` falls into the
`default:` case and returns `kInfiniteSlack`, so the sampler can never settle that rule for a
whole block — it asks it once per voxel, everywhere in the clip, and every ask evaluates the
entire named shape. Name `union { plinths rails balusters copings }` and you have bought
twenty-four `revolve`s of an eight-moulding profile *per voxel of the whole assembled building*,
and every other fragment in that build pays it too.

The walls fragment already knows the shape of this problem — `walls.clip` says "a paint rule
keyed on a shape is settled for a whole block at once by the sampler; one keyed on a pattern has
to be asked per voxel, everywhere in the clip" — and hangs its four pattern rules off a
two-box `walls_ring` by hand. `weather on=` gives you no way to say that: the scope you name is
both *where the coat goes* and *what gets evaluated per voxel*, and those want to be different
things.

**What I would like.** Either

- give the banish term a finite slack when the scope is a settleable shape — the scope's own
  distance is 1-Lipschitz, so `smoothstep(d, 0, band)` moves at most `1/band` per metre and could
  report `band`-scaled slack rather than infinity; or
- intersect the coat's test with the scope's **bounding box** as a separate, settleable factor, so
  a block wholly outside the scope is rejected on the box before the shape is ever asked.

Either way it should be possible to write `on=my_part` and not pay for it.

**Workaround in the meantime**, and it is worth putting in BRIEF.md: **scope weathering on a box,
not on your part.** `clips/facility/site.clip` does this — `site_weather_zone` is four boxes that
contain the plinth course, and the coat is free.

---

## 2. `overgrown` and `desert` occlude the whole assembled solid, per voxel

Separate from and worse than (1). `apply_weather` builds

```
const u32 shape  = script.solid;          // the WHOLE building at this point
const u32 cavity = f.occlusion(shape, 0.22 * s);
```

and `Weather::Overgrown` and `Weather::Desert` both read `cavity`. On `part_site` at metre 8:
27 ms with no weather, **10,167 ms** with `overgrown 0.18 on=site_stonework` — for a coat confined
to about forty square metres of balustrade. At metre 32 that is minutes, on a building with
nineteen columns and a coffered dome still to come.

Occlusion of the whole solid is the right question for an unscoped coat. For a scoped one it is
the wrong solid *and* the wrong extent: what the moss wants to know is how buried it is in
`site_stonework`, not in the entire facility. Passing the scope to `f.occlusion` when one is given
would make it both cheaper and more correct.

This is the reason the site's balustrade has hairline seams rather than moss, which is not what a
damp garden wall wants. If it is fixed, the line this fragment wants is in the comment at the
bottom of `site.clip`.

---

## 3. `tools/views.ps1` aborts in the middle of a run whenever the engine prints to stderr

Line 56 sets `$ErrorActionPreference = "Stop"`; line 249 is

```powershell
$log = & $exe @shot 2>&1 | Out-String
```

In Windows PowerShell 5.1, `2>&1` on a native command wraps every stderr line in an ErrorRecord,
and with `Stop` in force that record is terminating. The engine writes `[WARN ] frame  frame 1
took 391 ms` to stderr on any heavy frame — which is most frames at metre 32 with `-PathTrace`.
The result: the loop dies partway, the remaining views are never taken, **and no contact sheet is
written**, so you get nothing rather than a partial sheet. It looks like a render failure and it
is not.

I hit this three times before I found it and worked around it with a patched copy of the script
outside the repo. I have not touched `tools/views.ps1` because other agents are using it right
now, but the fix is one line — wrap the call in `try { } catch { }`, or set the preference to
`Continue` for that statement, or drop the `2>&1` and let stderr through to the console.

(Related, smaller: a concurrent `build.bat` from another agent makes `build/bin/WorldShaper.exe`
briefly not exist, and views.ps1 then reports every view as `FAILED` with
`CommandNotFoundException`. Nothing to fix, but worth knowing before you go looking for a bug in
your clip.)

---

## 4. `walkability` floods from the single lowest standable column, so one pond breaks the number

`measure.cpp`, `walkability()`:

```cpp
i32 lowest = clip.size[1];
for (...) if (here >= 0 && here < lowest) { lowest = here; start = ...; }
```

The flood starts at exactly one column: the lowest surface anywhere in the clip. A reflecting
basin sunk 45 mm below the lawn is that column, nobody can climb its 135 mm coping, and the whole
facility reports **1.1% walkable** — not the site, the whole building, because the metric is
global. It looks exactly like a building nobody can walk through.

I worked around it by putting the waterline exactly level with the lawn, so the tie goes to the
lawn (the scan finds z = 0 first) and the number means what it is supposed to again. That is a
good design decision for other reasons and I have kept it, but it should not be load-bearing: the
next fragment that adds a sunken area — a light well, a basement stair, an areaway — will break
the number again and will have no idea why.

Suggested: flood from every surface and report the **largest** reachable set as the percentage,
or seed the flood at the lowest column of the largest connected surface region rather than the
lowest column outright.

---

## Not a request, just a note for whoever reads the walk report

`--clip-part part_site --clip-metre 8` reports **3 components, 4 voxels not joined**. It is a
resolution artefact, not a fault: the order's baluster has a 0.063 m neck, which is half a voxel
at metre 8, so two of the twenty-eight balusters come apart in the middle. At metre 16 and at the
contract's metre 32 it is **1 component, all of it joined**. Measure this fragment at 16 or 32.
