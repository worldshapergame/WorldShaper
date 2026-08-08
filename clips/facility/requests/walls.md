# Requests from the walls (`clips/facility/walls.clip`)

Six things, in the order of how much they cost the building. The first two are the ones that
matter; the rest are worth knowing.

---

## 1. `mirror` silently deletes anything built on the negative side

`Op::Mirror` evaluates its child at `|coordinate|`. So the child's material on the **positive**
side is what appears on both, and a child that lives entirely on the negative side is asked about
at a place it does not exist and **vanishes from the building**. No error, no warning, no half a
shape — nothing.

BRIEF.md says "it folds the coordinate, so a shape built on the +x side appears on both", which is
true and is not the same sentence as "a shape built on the -x side is deleted". Every fragment
that faces south — and this building faces south, the contract says so twice — will reach for
`mirror { my_south_thing } axis=z` and get an empty set.

What it cost here: the first working version of this wall had the rustication's vertical joints,
the whole string course and **every quoin on the building** built on the south and east faces and
folded. It parsed clean, measured **1 component, all of it joined**, reported a plausible volume
and a plausible worldbox, and rendered a contact sheet from eight sides — with no quoins anywhere,
no vertical rustication on the north or south wall, and a string course that existed only on the
two end walls. Nothing in the toolchain said a word. It was found by eye, on a close render, and
confirmed with `--clip-part walls_quoins`, which reported

```
connected     0 components, largest 0 voxels, all of it joined
```

**"0 components, all of it joined" is the tell, and it should not need to be.** Two fixes, either
would do:

- **Say so.** `mirror` knows its child's bounding box. If the box is entirely on the negative side
  of the folded axis, the result is empty and that is almost certainly not what was meant: emit a
  script error naming the binding, the same way `weather on=` names an unknown scope.
- **Or fold the other way when that is what the child is.** Less principled, but a `mirror` whose
  child is wholly negative could fold to `-|c|` and mean what the author meant.

Failing both, BRIEF.md's grammar line needs the second sentence: *the child must be on the
positive side of the axis; a child on the negative side disappears.*

---

## 2. One `weather … on=` line makes the facility unbuildable

Measured, `--clip-part part_walls` at metre 16, changing nothing but adding a single line:

| | sample |
|---|---|
| no `weather` | **4.4 s** |
| `weather desert 0.25 scale=1.0 seed=13 on=walls_rustic` | **> 600 s** (abandoned at ten minutes) |

At least 140x, for one coat scoped to one storey of one fragment. This is why the facility still
has no weathering, and it is **not** the paint-keying problem that BRIEF.md describes — `on=`
fixed that, and the scoping is correct. It is the cost.

The reason is in `apply_weather` in `src/forge/clip_script.cpp`. Every kind builds its coats out of

```cpp
const u32 cavity = f.occlusion(shape, 0.22 * s);   // 14 evaluations of `shape`
const u32 up     = f.facing(shape, 1);             //  6 evaluations of `shape`
```

where `shape` is `script.solid` — the **entire building**. A coat keyed on those is keyed on a
pattern, so `Field::metric_slack` is infinite, so the sampler cannot settle it for a block and
asks it **once per solid voxel of the whole clip**. Twenty evaluations of the whole facility's
field per voxel, times tens of millions of voxels.

`on=` does not help, and cannot as written: `only_here(test)` is `f.add({test, banish})`, and an
`add` evaluates both its children. The banish term makes the answer wrong-on-purpose everywhere
outside the scope **after** the expensive half has already been computed. The scope is checked
last, when it is the only cheap thing in the expression.

The fix is the same shape as the fix for the coats' correctness:

- **Gate, do not add.** A node that evaluates its scope mask first and returns a constant
  out-of-range value without touching its other child — `select`, `gate`, call it what you like —
  turns the cost off where the paint is already off.
- **And let it settle.** The scope is a *shape*, so its `metric_slack` is finite: a block wholly
  outside `walls_rustic` can be settled to "this coat does not apply" from one reading at the
  block centre, exactly as a shape-keyed paint rule already is. `rule_slack` would then be the
  scope's slack rather than infinity, and a scoped weathering would cost roughly what it covers.

Until then: this is a **stone building outdoors** and the contract names weathering as one of the
seven things that has to end up exercised. It cannot be, by anybody, at any price.

---

## 3. "No two voxels alike" is arithmetically impossible past about 150,000 voxels per material

This part's whole brief is that the plain ashlar is the surface the variation has to carry alone,
so I measured it. `--clip-part part_walls` at the contract's metre 32:

```
volume        27340220 voxels   834.3573 m3
surface       2298308 faces   2244.441 m2
variation     1000008 distinct records over 27340220 voxels (3.6576%), largest identical group 588
```

and at three resolutions, which is where it gets interesting:

| metre | voxels | distinct records | unique |
|---|---|---|---|
| 8 | 407,808 | 276,484 | 67.8% |
| 16 | 3,264,936 | 787,779 | 24.1% |
| 24 | 11,584,512 | **1,000,007** | 8.6% |
| 32 | 27,340,220 | **1,000,008** | 3.7% |

Two separate ceilings, and they happen to sit on top of each other at a million:

**The budget.** `Variation::budget` defaults to 1,000,000 and `clips/facility.clip` does not set
`budget=`. That million is for the **whole clip**, and this one fragment spends all of it before
any other part of the building gets a look in. Which fragment gets the variation and which gets
the leftovers is currently decided by which slab of z the sampler reaches first.

**The arithmetic, which is the real one.** `apply_variation` perturbs an 8-bit channel by
`signed_unit * amount * 255`. With the manifest's `colour=0.030` that is ±7.65 — **16 reachable
values per channel** — and `rough=0.070` is ±17.85, **36 values**. So one base material can
produce at most

    16 x 16 x 16 x 36 = 147,456 distinct records

no matter how many voxels it covers or how large the budget is. This part carries five base
materials of its own, so its own ceiling is about 737,000; the coats other fragments lay on it
(window surrounds, bands) bring it to about a million. **The budget is not really what bound it —
it just happens to be set at the same place the arithmetic stops.** Raising `budget` alone would
buy almost nothing.

If a genuinely unique record per voxel over a large surface is the thing being tested, the dial is
`colour` and `rough`, not `budget`: `colour=0.060` quadruples the reachable colours per channel
and takes one material's ceiling to about 9 million.

Two small asks, neither of which I can do from a fragment:

- **The report should say which ceiling bit.** `VariationReport` already counts `reused`, and
  `main.cpp` does not print it. `reused == 0` means the arithmetic ran out; `reused > 0` means the
  budget did. As printed the two are indistinguishable and look identical to "it works".
- **`clips/facility.clip` should set `budget=` deliberately**, whatever the number, so that it is
  a decision rather than a default that one fragment can eat.

For the record, the load itself does land: on a path trace the ashlar reads as granular stone and
not as flat grey (`renders/w-det/pt-necorner.png`). It is the *measurement* of it that is
currently ambiguous.

---

## 4. `bricks` cannot tell you *which* brick, only how far into the joint

`Op::Bricks` returns `-min(du, dv)` — the distance into the mortar. That is exactly right for
carving or colouring a joint, and this part paints its ashlar joints with it: a 0.03 m line, one
voxel at metre 32, on a wall with no geometry under it.

But the thing an ashlar face most wants is **a different stone in every block**, and that needs a
value that is *constant across one brick and pseudo-random between bricks*. `Op::Bricks` already
computes the course index and the along index; a hash of the two is three lines and one more
output. Nothing else in the language can produce it: `checker` and `stripes` do not know about the
running bond's half-course offset, and an `fbm` does not respect a block boundary.

I worked round it with a broad `fbm`, and the workaround reads as **staining** rather than as
**bedding** — patches that drift across joints instead of stopping at them. It is a defensible
look for weathered limestone and it is not the look I wanted. Suggested spelling, so that the
existing one is untouched:

```
let p = bond length=1.35 height=0.675 facing=z     # 0..1, one value per stone
```

---

## 5. `tools/views.ps1` stops mid-sheet whenever a frame is slow

The script sets `$ErrorActionPreference = "Stop"` and then calls

```powershell
$log = & $exe @shot 2>&1 | Out-String
```

In Windows PowerShell, `2>&1` on a native executable wraps each stderr line in an ErrorRecord, so
the engine's own perf note —

```
[WARN ] frame    frame 1 took 260 ms
```

— becomes a terminating NativeCommandError and the whole run stops, on a render that **succeeded**
and wrote its PNG. It works at metre 12, where the first frame is quick, and fails every time at
metre 24, where it is not. The failure looks like a render error and is not one.

Any of: drop the `2>&1` (stderr is shown anyway), wrap the call in `try { } catch { }`, or set
`$ErrorActionPreference = 'Continue'` around just that line. Workaround, which is what every
render past metre 16 in this session used: call `WorldShaper.exe --cam x,y,z,yaw,pitch` directly.

Worth writing down alongside it, because it is not obvious and it is not in BRIEF.md: **`--cam`
coordinates are in the built world's units, and `--clip-metre M` scales the whole clip by M/32.**
At metre 16 a camera aimed at a real 16.0 m goes at 8.0.

## 6. A render above metre 16 exits silently when several agents are rendering at once

Twice, `--clip-metre 24` and `--clip-metre 32` with `--screenshot` exited with status 0 about
0.7 s in, immediately after `jobs started 8 worker threads`, with no PNG and nothing whatever in
`worldshaper.log`. The same commands worked ten minutes later. Measuring the same part at metre 32
without a screenshot works every time and takes 37 s, so the sampler is fine — it is the GPU world
buffers (460 MB of payload at metre 16 for one fragment, and there are several of us on one card).

Not necessarily a bug. The **silent exit** is: an allocation that cannot be satisfied should say
so, because as it stands the only signal is a missing file.
