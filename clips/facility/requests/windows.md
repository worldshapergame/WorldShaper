# windows — what I found, what I assumed, and what I could not do

## 1. `rotate` has no bounding box, and one inside a `repeat` turns the sampler off for the whole clip

This is the finding worth reading. It cost me a hundred-fold in sampling time and it will cost the
next person the same, silently, because nothing reports it.

`build_bounds` in `src/forge/field.cpp` deliberately gives `Op::Rotate` (and `Scale`, `Twist`,
`Bend`) no bounding box — the comment says so, and the reasoning is right: a bound that is wrong by
a little produces a clip with pieces missing. But `metric_slack(Op::Repeat)` asks for the child's
bounds to check that a copy fits inside one period, and:

```cpp
if (child.infinite()) return kInfiniteSlack;
```

So a `repeat` around anything containing a `rotate` reports infinite slack. `sample.cpp` takes
`min(metric_slack(root), skip_slack())` for the whole shape, so **one rotate inside one repeat,
anywhere, means no box in the clip can ever settle and the sampler walks every voxel of the
bounding array.**

Measured, on `part_windows`, at the contract's metre 32, with the geometry byte-identical before
and after:

| | field evaluations | sample time |
|---|---|---|
| fanlight rays written as `translate { mirror { union { rotate … } } }` | 705,143,256 | 386.4 s |
| the same thing wrapped in `intersection { … box }` | 15,915,848 | 3.7 s |

The array is 582 M voxels and the bad version made 705 M evaluations — 1.2 per voxel, i.e. no
skipping at all, which is the signature to look for. The fix is one line: intersect the rotated
thing with a box it already fits inside, so the intersection reports the box and the repeat can
measure it. It changes not one voxel.

**Two suggestions, in order of preference:**

1. Give `Op::Rotate` a real bounding box — the AABB of the child's eight rotated corners. It is
   always a *superset* of the true extent, never a subset, so the "wrong by a little loses voxels"
   objection does not apply to rotation the way it applies to displacement. The union cull tests
   `squared_distance_to(box, p)` and only skips when the running answer already beats it, which is
   sound for any conservative box. `Mirror` already does exactly this and for exactly this reason.
2. Failing that, make `metric_slack(Op::Repeat)` fall back to the *declared period* rather than to
   infinity when the child cannot be measured: if the author wrote `x=2.70`, charging the slack as
   if the child filled the period is conservative and finite.

Either way it would also be worth having `--clip-part` print the root's slack next to `cost`. The
number that would have told me in one second — "slack infinite" — is computed and thrown away.

## 2. `_order.clip`'s straight-run mouldings are built in the wrong plane

Not mine to fix, and I do not use them, but `entab_run` in `clips/facility/_order.clip` will not be
the cornice its author drew. A moulding's six-number form takes `run=` (default `z`), and from that
derives `proj = (run == 0) ? 2 : 0` and `high = (run == 1) ? 2 : 1`. `entab_run` travels along **x**
and does not say `run=x`, so the parser reads the section in (x, y) and sweeps it along z:

```
let entab_run_ovolo = ovolo  -17 1.62 -0.34   17 1.545 -0.42
```

builds a quarter-ellipse of radii 34 x 0.075 centred at x = -17 — a band that is full height at one
end of the building and pinches to nothing at the other — swept 0.08 along z. `entab_run_bed` and
`entab_run_crown` are the same. The `fillet` members are unaffected because a fillet is just its
box, which is why it very nearly looks right. The fix is `run=x` on the four curved ones, after
which their two corners are read as (z, y) and want re-ordering to match.

I hit the same trap on my own sill mouldings and only caught it because a quarter round came out
1.80 m long instead of 0.09 m tall.

## 3. `tools/views.ps1` aborts whenever the engine prints a frame warning

`$ErrorActionPreference = "Stop"` at the top plus `& $exe @shot 2>&1` at line 249 means that any
line WorldShaper writes to stderr — including the entirely routine `[WARN] frame  frame 1 took
658 ms`, which it prints on the first frame after a cold world build — is promoted to a terminating
NativeCommandError and the whole run dies, usually after writing one PNG of nine and no contact
sheet. It is not deterministic: it depends on whether the world happened to be cached.

Workaround: run it once to warm the cache, ignore the failure, run it again. A real fix is one line
— `2>&1` on a native command in PowerShell 5.1 should be `--%`-guarded or the call wrapped in
`$ErrorActionPreference = 'Continue'`.

## 4. The one assumption this file makes about anybody else

**The outer wall of the main block is at most 1.80 m thick.** Every opening is cut 1.80 m (4 M)
inward from the wall face at x = ±16.00 / z = ±7.50. `walls.clip` has since landed at 0.90 thick,
so there is 0.90 m of margin and the cut runs 0.90 m past the inner face into the room. If an
interior fragment builds a lining or a partition hard against the inside of the outer wall it will
find window-shaped holes in it — which is what it should find, but it is worth knowing.

Nothing else here needs anything from anybody. The dressings stand on the wall face and bite 0.09
into it, so they meet whatever the wall turns out to be.

## 5. Two places where my stone and the doorcases touch, on purpose rather than by accident

`doors.clip` puts the great doorcase 4.05 wide (x ±2.025, corona at y 7.485 – 7.65) and the side
doorcases from x 3.96 to 6.84 up to y 5.85. I have taken the ground storey out of all three portico
bays on the south front in response — a window inside a doorcase is not a window — but on the piano
nobile above them two members still overlap:

- the great doorcase's corona (x to ±2.025) and the architrave of the window at x = ±2.70 (inner
  edge 1.89) share 0.135 m over a 0.165 m band of height;
- the side doorcase's crown (to y 5.85) and the consoles and apron of the window at x = ±5.40
  (feet at y 5.625 and 5.31) share about 0.2 m of height.

Both read as the doorcase cornice returning against the window above it, which is what a real front
does, and a union of two overlapping solids has no seam. But if doors.clip would rather they did
not touch, the numbers to move are its ±2.025 and its 5.85, not mine: mine are fixed by the 2.70
bay and by the pilaster clearance (`pilasters.clip` sized its bays around a window pediment of
half-span 0.90, which is exactly what this is).

## 6. What I did not build, and why

- **No weathering.** `weather … on=` is scoped properly now and it would suit a stone front, but
  every coat is a displacement and the glazing bars are 0.030 m across. The manifest's own 12 mm of
  `grain_fine` is already 40% of a bar. A second displacement on top of it would eat them, and the
  bars are the load this part exists to carry. Deliberate omission, written into the file.
- **No coloured stone in the tympana.** I wanted porphyry panels in the pediments. Telling a
  tympanum apart from the raking cornice around it needs its own placement tree (there is no shape
  test that separates them on all four fronts at once), and a fifth placement tree is four more
  unions of thirty translates for one accent colour. The five materials the part does carry —
  marble, granite, bronze, oak, glass — are separated by one box at the string course line, which
  costs nothing.
- **The raking cornice of a triangular pediment stair-steps.** A 1-in-2 rake on a 32-to-the-metre
  grid is two voxels per step and there is nothing to be done about it in this engine. It reads a
  little like a dentil course. The segmental pediments do not have the problem, which is one more
  reason to alternate them.
