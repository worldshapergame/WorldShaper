# theatre.clip — what the language could not do, and what it turned out it could

Six things came up building `clips/estate/theatre.clip`. Two are corrections to what the briefs
say, three are missing operations with a workaround each, and one is a trap that cost half the
build time and is worth a line in somebody's notes.

## 1. `revolve`, `around` and `arc` DO have a partial-arc form now

Both `clips/facility/BRIEF.md` and `clips/estate/BRIEF.md` say the language "has no partial-arc
form yet" and tell you to use `around` or a revolved profile cut by a half space. That is out of
date. `clip_script.cpp` reads `from=` and `to=` on all three:

```
let apse = revolve { section } axis=y from=0.75 to=0.25
let colonnade = around { column } count=7 axis=y from=0.0 to=0.5
let hoop = arc 0 2 0 ring=1.35 tube=0.045 axis=x from=0.0 to=0.5
```

`sweep_range` in `field.cpp` handles the wrap, an arc that runs through the seam at zero, and the
`from == to` case (which it reads as a whole turn, deliberately). A partial `around` puts its first
copy on `from` and its last on `to` INCLUSIVELY, which is different from the whole-turn spacing and
is the form you actually want for a colonnade across a facade.

This file still cuts its cavea with a half space rather than with `revolve ... from= to=`, and that
is a choice rather than ignorance: `metric_slack` for a partial `Revolve` is its child's, same as a
whole one, so both are exact — but the half space also bounds the SEATING'S BOX, which the partial
form does not (a partial revolve's bounds are still computed from the whole revolution in the cases
that matter here). Either would have been correct. The half space was cheaper.

## 2. There is no angular pattern

The pattern set has `axis of=y` (a raw coordinate), `distance cx cy cz` (a radius), `sine`,
`stripes`, `checker`, `bricks` and the noises. **There is nothing that returns an ANGLE.** So a
radiating pavement — the oldest paving pattern there is, and the one a semicircular orchestra
wants — cannot be painted from a pattern at all. It has to be built as matter: sixteen thin boxes
through the centre, rotated by four doublings, subtracted as joints.

That works and it is not even expensive, but it means the pattern is geometry rather than colour,
so it cannot be used as a paint key on somebody else's shape and it cannot be varied with radius.
An `angle about=x,z` pattern returning turns in [0,1) would be four lines in `field.cpp` and would
give radiating paving, fan vaults, sunburst inlay and the flutes of a dome for free.

`around { }` cannot substitute, because `metric_slack` for `Op::PolarRepeat` is
`kInfiniteSlack` — one polar fold anywhere in a clip and nothing in that clip settles.

## 3. There is no taper

A herm is a square shaft that is WIDER AT THE TOP THAN AT ITS FOOT. `scale` multiplies each axis by
a constant and cannot vary with height; there is no `taper`, no `loft` and no two-section sweep.
What works is two tilted half spaces folded once each:

```
let tap_x = mirror { plane 1 -0.05 0 at=0.18 } axis=x
let tap_z = mirror { plane 0 -0.05 1 at=0.18 } axis=z
let shaft = intersection { box -0.30 0 -0.30  0.30 1.80 0.30  tap_x tap_z }
```

which reads as "|x| and |z| at most 0.18 plus a twentieth of the height". It is exact and it costs
five nodes. Two things to know if you copy it: `Field::plane` normalises the normal but not the
offset, so `at=0.18` with a normal of `(1, -0.05, 0)` is really 0.18022 — a fifth of a millimetre,
and below the voxel everywhere, but write it down rather than discover it. And a `plane` has no
bounds at all (see 6), so the intersection MUST include a box.

## 4. `repeat` can only make an odd number of copies

`nx` counts copies **either side** of the original, so a `repeat` produces 2n+1 and there is no way
to ask for twenty-four of anything. Twenty-four footlights came out as eleven from a `repeat`, one
placed by hand, and a `mirror` over the pair:

```
let row = mirror {
    union {
        translate { repeat { one } x=0.675 nx=5 } 3.7125 0.90 0.09
        translate { one } 7.7625 0.90 0.09
    }
} axis=x
```

A `count=` alternative to `nx=`, anchored at the original rather than centred on it, would say the
same thing in one line and would not need the reader to check the arithmetic.

## 5. A pattern cannot be turned into matter without spending the whole clip's skip budget

The berceau is meant to be a tunnel whose walls let light through **thousands of small gaps**. The
obvious way to get gaps is to threshold a `cells` field and subtract it:

```
let gaps = remap { cells size=0.27 seed=5 } from=0 to=1 low=-0.06 high=0.10
let hedge = difference { arch gaps }        # DO NOT
```

That parses and it does make holes. It also makes `metric_slack` infinite for the whole solid —
`field.cpp`'s `metric_slack` returns `kInfiniteSlack` for every pattern op, and one infinite child
under a union or a difference poisons everything above it — so no box in the clip settles and the
sampler walks every voxel of a 262-million-cell array one at a time.

So the gaps here come from `displace` alone, which stays metric because the amplitude is bounded.
Displacement can only move a surface, though: it thins the 0.45 m hedge to between 0.18 and 0.72,
and it cannot punch a hole without also risking a piece of hedge floating free of the rest, which
costs a component and is exactly what `clipcheck` is watching for. **The berceau's walls are
therefore ragged rather than perforated**, and that is a limit of the tool and not a decision.

What would fix it: a `solidify { pattern } at=0.55` op with an honest Lipschitz bound (a cell noise
of size s has a gradient no steeper than about 1/s, which is a bound worth having), or simply
letting `difference` take a pattern child without charging the whole tree for it.

## 6. `plane` has an infinite bounding box, and an intersection keeps it

This is the trap, and it is worth writing down because it is silent and it is expensive.

`bounds_of(Op::Plane)` is `everywhere()`. `Op::Intersection` takes the intersection of its
children's boxes — so `intersection { revolve plane }` reports the box of the **whole revolution**,
both halves of the circle, not the half the plane leaves. Every sample taken anywhere in that box
is charged with evaluating a shape that stops fourteen metres away, and `union_children` will not
flatten an intersection, so it cannot be taken apart either.

Cutting the cavea, the coping, the paving, the berceau and every paint key in this file with a half
space written as a **box** instead of as a `plane` is two lines changed. Both arms, same command,
`--metre 8`, taken from two copies of the file in a scratchpad so the control is not a counter read
from inside the change:

| | CPU | result |
|---|---|---|
| `let th_south = plane 0 0 1 at=0.05` | 141.5 s | 179 components, largest 941593 voxels |
| `let th_south = box -17 -1 -17  17 10 0.05` | 81.3 s | 179 components, largest 941593 voxels |

The same clip to the voxel, in 57% of the time. Wall clock is not quoted: the machine was running
fourteen other builds at a load average near forty, and CPU time is the only figure that survives
that.

Nothing needs fixing in the language for this — a box is the right tool and it is already there.
What would help is `bounds_of` learning that an intersection with a half space can clip the box it
already has, which is four comparisons and would make the obvious spelling the fast one.
