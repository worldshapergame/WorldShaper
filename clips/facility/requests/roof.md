# roof — what I could not do, and three things somebody else should know

From building `clips/facility/roof.clip`. Four findings, one note for `drum.clip`, one for whoever
owns `tools/views.ps1`. The first two are measured with numbers; the third has a twelve-line repro
clip sitting next to this file.

---

## 1. `brush` renders as fixed glitter, and it does not converge

**This is the one worth reading.** `lead` is `rgb=118,120,124 rough=170 metal=140 brush=2`. Path
traced, 340 m² of it comes back looking like gravel or old snow: hard white speckle two or three
voxels across, over the whole deck.

I ruled the alternatives out one at a time, each with a render:

| suspected | test | result |
|---|---|---|
| the manifest's `variation colour=0.030 rough=0.070` | same clip with the line and without it | **identical** |
| my paint missing voxels, leaving the base coat | changed my base coat from `limestone` to `porphyry` and re-rendered | **identical** — so every voxel really is `lead` |
| Monte Carlo noise not yet converged | 72 accumulated frames vs 600 | **identical** |
| the material being a metal at all | see below | no — plain metal is fine |

`clips/facility/requests/roof-brush-repro.clip` is the isolation: one flat slab, three bands, all
three the same colour (118,120,124) and the same roughness (170), differing only in the metal keys.

```
material lead   rgb=118,120,124 rough=170 metal=140 brush=2
material lead_p rgb=118,120,124 rough=170 metal=140
material stone  rgb=118,120,124 rough=170
```

```
.\build\bin\WorldShaper.exe --clip-file clips\facility\requests\roof-brush-repro.clip ^
    --clip-metre 32 --pathtrace --width 760 --height 400 ^
    --screenshot-frame 200 --screenshot out.png --cam "0,7.0,-7.0,450,-42"
```

The `stone` band is smooth. The `metal=140` band is smooth. The `metal=140 brush=2` band is
speckled, and only that one, from every angle and at any sample count. Images:
`renders\roof-pt\leadtest-steep.png` (all three bands in one frame, steeply from above — the
cleanest picture of it), `renders\roof-pt\leadtest.png` (grazing, 72 frames) and
`renders\roof-pt\leadtest-600.png` (the same at 600).

It looks like the anisotropic tangent frame is being picked per voxel rather than from the surface,
so each voxel gets its own brush direction and its own answer for the same sky. That would explain
all four observations: fixed, not noise; independent of `variation`; invisible without `brush`; and
worst at grazing angles, where an anisotropic lobe is widest.

**Two things follow.** `bronze` is also `brush=2`, and it is on the great door, the entablature's
swags and every railing in the building — small enough that nobody has noticed. And until it is
fixed, this roof is a speckled roof. I have deliberately NOT worked around it: `lead` is the
contract's material, this is the largest surface in the scene wearing any metal, and a fragment
quietly swapping to a non-metal to make its own render tidy would have hidden the only thing this
part was ever going to find. **If the shading is not what gets fixed, the fallback is one line in
`_contract.clip` — `material lead_sheet rgb=118,120,124 rough=170 metal=140` with no `brush` — and
I would rather ask for that than edit the contract myself.**

---

## 2. `rotate` has no bounding box, so `repeat { rotate { … } }` costs the whole clip its sampler

Same family as the `around { }` finding in `requests/order.md`, arrived at from the other end.

`Field::build_bounds` deliberately leaves `Op::Rotate` unbounded — the comment in `field.cpp` says
so and gives a good reason ("a bound that is wrong by a little produces a clip with pieces
missing"). But `metric_slack(Op::Repeat)` asks `bounds_of` for the child and returns
`kInfiniteSlack` the moment it is infinite, because it cannot then check that a copy fits inside its
own cell. So *any* rotated shape inside a repeat makes the whole expression infinitely slack, and
infinite slack anywhere means the sampler cannot skip a single voxel of the bounds anywhere.

Measured on thirteen lead rolls, one bank, metre 16, nothing else changed:

| written as | field evaluations | sample time |
|---|---|---|
| `repeat { rotate { capsule } }` | **91,501,146** | 5.4 s |
| `repeat { union { capsule capsule } }`, ends written out | **11,374** | 0.28 s |

and the whole part 8.1 s → 1.6 s. About 8,000× on the bank.

**The workaround is one word: intersect the rotated shape with a box.** An `Intersection` takes the
overlap of its children's boxes, so one finite child hands the whole node a finite box, and `repeat`
is satisfied. That is what `roof_fall_east` did while it still had a `rotate` in it, and it is why
that one was never slow. It is not discoverable — nothing warns, the clip parses, measures, reports
one component and renders correctly, only forty times slower.

**Suggestions, in the order I would try them:**

1. Give `Op::Rotate` a bound after all. The AABB of eight rotated corners is conservative in the
   safe direction (it can only be too big), which is the opposite of the failure the comment is
   guarding against — an over-large box never loses geometry, it only culls less. The comment's
   worry seems to be about bounds that are too *small*.
2. Failing that, have `metric_slack(Op::Repeat)` fall back to the child's *own* extent computed
   through the transform chain rather than giving up at the first unbounded node.
3. Failing both, say it in `clip_script.hpp`'s language summary and in `BRIEF.md`, next to the
   existing note about `around`. Two sentences would have saved this an afternoon.

---

## 3. Scoped `weather` costs 160× and I had to drop it

The gutter is the one place in this building where weathering is unarguably right: a dead level
channel, in the parapet's shadow all day, holding water. One line —
`weather overgrown 0.10 scale=0.7 seed=13 on=roof_moss`, scoped to the channel and with the sumps
differenced out of the scope — and the geometry was unchanged and correct.

`--clip-part part_roof`, metre 8, the same part, the line in and out:

| | field evaluations | sample time |
|---|---|---|
| with the weather line | 845,024 | **17,646 ms** |
| without it | 688,208 | **110 ms** |

Note the evaluation count barely moved: it is not that more of the field is being walked, it is that
each walk got 160× dearer. `apply_weather` builds `occlusion(shape, …)`, `curvature(shape, …)` and
`facing(shape, 1)` on `script.solid` — the *whole clip* — and the two `PaintRule`s it pushes are
pattern rules, so they are asked once per solid voxel, and each asking walks the whole building
several times over. The `on=` scoping works exactly as advertised and is not what costs: the mask is
a `smoothstep` of one distance, and the banish term is an add.

**What would make it usable:** the scope is known before any of that is built. If the three
expensive terms were wrapped so that they are only evaluated where the scope mask is non-zero — or
if the rule carried the scope shape as its `test` shape and the pattern as a secondary condition, in
the way `walls.clip` maxes its patterns against a zone shape — the cost would be proportional to the
weathered area instead of to the whole scene. As it stands, weathering a 34 m² channel costs the
same as weathering the building.

Until then this fragment carries none, and I have said so in the file so nobody adds it back without
measuring. `site.clip` is currently the only fragment in the building that weathers anything.

---

## 4. `tools/views.ps1` dies on any `[WARN]` line, and `-Views inside` never produced a frame

`views.ps1:249` is `$log = & $exe @shot 2>&1 | Out-String`. Under Windows PowerShell 5.1 redirecting
a native executable's stderr wraps each line in an `ErrorRecord`; when the engine prints
`[WARN ] frame  frame 1 took 588 ms` — which it does whenever a frame is slow, so almost always on
the first view of a fresh world — the script terminates there and the remaining views and the
contact sheet are never made. It is intermittent in the worst way: the same command succeeds on the
second run because the world is cached and no frame is slow enough to warn.

I got round it by running the command five times in a row and letting the cache warm up, and for the
interior views by driving `WorldShaper.exe` myself. `-Views inside` never produced a single PNG for
me on any attempt.

Two one-line fixes, either would do: capture with `2>$null` instead of `2>&1`, or wrap the call in
`try { … } catch { }`. The one thing to know if you drive the engine directly instead: **the camera
is in built-world units, not clip metres** — the script prints the ratio ("built at 12/m, so it
stands at 38% size"), and it is `Metre / 32`. A camera at clip (10, 8, 0) at metre 12 is
`--cam "3.75,3.0,0,…"`.

---

## 5. For `drum.clip` — your apron is buried, and you may or may not care

Your file says the drum's foot stops at 11.25 so it is "buried in whatever the roof turns out to
be", and puts a copper apron at 11.70 – 12.15 to dress the joint. The roof turns out to land at
**12.15**, the top of your own plinth, and it cannot land lower. The chain is short and none of it
is mine to move: the gutter must have deck under it, that deck sits on a wall head at 11.90, 0.15 is
the least deck worth having, so 12.00 is the lowest a gutter sole can be, and the fall has to come
down to it from somewhere above.

So your plinth is buried to its top and the apron with it, and what is left is a 0.075 m ring of
copper lying flush in the lead at the drum's foot. It does read — it comes out as a green ring round
the well in `renders\roof-plan32\plan.png` — and an apron flashing lying in the lead is exactly what
an apron flashing is, so I think this is a good joint and I have changed nothing. Your socle stands
0.15 clear above the roof and your congé 0.30, both untouched, and the two solids share 0.05 of
radius (my deck's inner face is a cylinder of 5.80, your plinth runs to 5.85) over the deck's whole
depth, so there is no seam for the manifest's grain to open.

**If you want the copper to read as a skirt rather than as a line, move the apron to 12.15 – 12.60.**
It will then stand on this roof instead of under it. Nothing in my file moves either way, and I have
not assumed you will.

---

## 6. Smaller things, for the record

- **A slope shallower than one voxel per voxel is not a slope.** This roof falls 0.15 m in 8.55 m,
  because that is all the height a 1.10 m parapet leaves. Built as one tilted plane, the sampler
  terraces it into 1.78 m treads — it has no choice — and the 0.045 m lead rolls standing on it
  vanish on every second tread, so they render as dashes. Rebuilt as four exactly level bays with a
  0.0375 m drip between them, every roll sits at one height for its whole length and reads as one
  line. Real leadwork does the same thing for a related reason. It is worth saying in `BRIEF.md`
  that shallow slopes should be built as steps chosen by the author rather than steps chosen by the
  arithmetic.
- **The well is round, not square.** The brief for this part said to leave `x -6..6, z -6..6` clear
  for the drum. By the time I built it `drum.clip` existed and its plinth is a circle of 5.85, so a
  square well of 12.00 would have left 52 m² of open sky straight down into the rotunda — the exact
  failure this part exists to prevent. The well is a cylinder of 5.80 and laps the plinth by 0.05.
  Anybody re-briefing a roof fragment should take the well from whatever is actually standing in it.
- **Nothing here is painted by anybody else and nothing here paints anybody else.** Checked by
  path-tracing `--clip-part part_roof` alone and comparing with the same view of the whole building:
  the deck is the same colour in both.
