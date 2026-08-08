# Requests from the order (`clips/facility/_order.clip`)

## 1. `around { }` costs the whole clip its sampler, and nothing says so

`Field::metric_slack` returns `kInfiniteSlack` for `Op::PolarRepeat` (`src/forge/field.cpp`, the
`case Op::PolarRepeat: return kInfiniteSlack;` in `metric_slack`). Infinite slack anywhere in an
expression means the sampler cannot skip **any** voxel of the bounds, anywhere in the clip — not
just near the polar repeat.

Measured, on `ionic_column` at metre 32, changing nothing but how twenty-four flutes and
thirty-two eggs-and-darts are laid out:

| flutes and carving written as | field evaluations | sample time |
|---|---|---|
| `around { fin } count=24 axis=y` | 85,125,188 | 10.5 s (at metre **16**) |
| seven rotates, mirrored in x then z | 89,701 | 1.1 s (at metre **32**) |

That is a factor of about 950 in evaluations, and the `around` figure is the whole bounding box
sampled exhaustively — 544 x 335 x 400 — which is exactly what "no skipping" means. There are
nineteen columns in this building. Written the obvious way, the order alone would have made the
facility unbuildable, and it would have looked like the flutes were expensive rather than like
one op had switched the optimiser off.

The reason for the infinite slack is sound: a polar fold can report a distance *larger* than the
truth for a point near a sector boundary, because it cannot see the copy in the next sector. But
the overestimate is bounded — it is at most the distance across the sector at that radius, which
is `2 * r * sin(pi / count)`, and the sampler knows `r`. Two things would each fix it:

- **Bound it properly.** A polar repeat that evaluated the two nearest sectors rather than one is
  exact, at twice the cost of the child, and would report the child's own slack. For a 24-fold
  repeat of one small box that is a trade worth making a thousand times over.
- **Failing that, say so.** An op that silently turns off skipping for the entire clip should be
  documented in BRIEF.md's grammar, next to `around`, in the same plain words as the `weather`
  `on=` warning. I have written the workaround into `_order.clip` so the next person does not
  undo it, but the next person to reach for `around` in a *different* fragment will not read my
  file.

I have worked around it: every radial repeat in the order is now explicit `rotate` nodes over one
quadrant, mirrored in x and then in z. Rotations are isometries, so they are exact and each
carries a bounding box. It costs seven lines instead of one and it is nine hundred times faster.

## 2. `tools/views.ps1` cannot frame a part smaller than about 0.2 m

`views.ps1` measures a part's `worldbox` by building it at metre 6 first. Anything thinner than
about a sixth of a metre has no matter at metre 6, so the probe reports no worldbox and the script
throws "the clip did not report a worldbox - it may have failed to build" — for a part that builds
perfectly well. `ionic_baluster` (0.19 m across) does this every time.

Workaround: pass `-Focus` with the box by hand, which skips the probe. A better fix would be to
raise the probe metre until it finds matter, or to fall back to the reported `bounds` and say what
happened, because the error as written sends you looking for a parse error that is not there.

## 3. Not a request, a warning to whoever owns the shaders

Two renders in this session died with

```
[ERROR] shader   cannot open build\bin\shaders\pathtrace.comp.spv
```

and then worked again a minute later on the identical command. Somebody is rebuilding
`shaders/pathtrace.comp` while the rest of us are rendering. Nothing to fix in the clips; if your
contact sheet comes back empty, run it again before you go looking for the cause in your own file.
