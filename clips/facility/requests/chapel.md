# chapel — what the language could not do, and two traps that cost real time

Written from building `clips/facility/chapel.clip`: a baroque oval chapel, 7.20 x 2.25 m in the
clear, in the south-west corner of the ground storey.

## 1. `revolve` and `around` still have no `from=` / `to=`

The brief said another agent was adding them and not to wait, and I did not. Everything partial in
this fragment is a full revolution or a full fan cut by something else, and all three of the
substitutes the brief named work:

- an **oval plan** is `scale { cylinder r=1 } x=a z=b`, exact, and concentric ovals are one scalar
  apart. The whole room, its dado, its three-band entablature and its cupola are seven of these.
- a **half-dome** is a whole ellipsoid with the rest of the air unioned round it — the apse here is
  a cylinder to 3.60 with an ellipsoid cap on top, and nothing needed cutting.
- an **arc of ribs** is four explicit `rotate`s of a bar that is already `mirror`ed about the axis,
  giving eight, intersected with the shell they live in.

What I would still ask for, in order of what it would have saved:

**A constant-offset oval.** `offset` cannot give one, and neither can scaling. The mouldings in
this room project 0.09 on the flanks and 0.288 at the ends because the only way to say "an oval
0.09 inside that oval" is to scale it, and a scaled ellipse is not an offset ellipse. `offset` does
not help because a non-uniform `scale` divides its child's distance by the SMALLEST scale factor
(field.cpp, `case Op::Scale`), so the metric is stretched in exactly the same proportion. It reads
fine — the eye follows a continuous moulding and does not measure its projection — but it is a
thing the language cannot say and a real oval room does say.

**A rib that follows a doubly curved surface.** Cutting ribs out of `difference { intrados,
intrados-lowered-by-0.09 }` gives a shell 0.09 thick at the crown and nothing at the springing,
where the surface turns vertical; what comes out of the thin end is single voxels with no
neighbours. `offset { intrados } by=-0.09` fixes it completely because it measures along the
normal. That is worth writing down as the general answer for any rib, coffer lip or lining on a
curved surface: **offset the surface, do not translate a copy of it.**

## 2. A NON-UNIFORM `scale` REPORTS INFINITE BOUNDS, exactly like `rotate`

`build_bounds` in `src/forge/field.cpp`:

```
case Op::Scale: {
    ...
    if (most - least > 1e-12) { box = everywhere(); break; }
```

So `scale { } x=3.6 z=1.125` is, for the sampler, a shape that is everywhere. windows.clip and
rotunda.clip both carry long warnings about `rotate` doing this and neither mentions `scale`, which
is the same trap with a different name, and an oval room is made of nothing else — this fragment has
eight of them. Every one is wrapped in `intersection { ... box }` with a box it already fits inside.
**That warning belongs next to the `rotate` one in BRIEF.md**, because the next person to build a
curved room will reach for `scale` first.

## 3. `bounds` is last-one-wins, and `include` re-runs it

The contract's box is 34 x 21 x 25 m — 582 million cells at metre 32, about 2.8 GB — and the kernel
killed the first full-detail run of `chapel-probe.clip` for want of memory. Restating `bounds` in
the probe is the fix, but it has to be written **below** `include "../chapel.clip"`, because that
file includes `_contract.clip` itself and `bounds` is a plain assignment (clip_script.cpp:776). A
`bounds` written above the include is silently overwritten and the run is the 2.8 GB one again, with
no warning of any kind. One killed process to find out.

A `--bounds` flag on `clipcheck`, or a warning when `bounds` is set twice, would have saved it.

## 4. Not a language problem, but the thing that shaped this room more than anything else

**Every ground-storey window in this building is punched 1.80 m in from the OUTER wall face — 0.90 m
past the inner face of a 0.90 m wall — and that hole is subtracted from the whole building after
every fragment is assembled.** halls.clip already ended its hall 1.15 m short because of it. Here it
does three things at once:

- the strip `z -6.65 .. -5.70` along the whole south side is a hole nothing of mine can occupy, so
  the oval's south flank had to stand at -5.625 and the room is 2.25 m deep instead of 3.15;
- the arched window at x = -8.10 has its head at 4.725, so an entablature bedded anywhere below that
  comes back with an arched notch a metre wide bitten out of it, which is why the order here is
  6.5 M tall and the cupola only has 0.45 of rise;
- **no glass of mine can stand in a window opening.** `void_windows` is not reduced by anything but
  `part_windows`, so a stained light in the reveal is deleted. The four here sit 0.03 and 0.025 on
  the room side of the cutter, held on all four edges by stone the cutter cannot reach, and there is
  nowhere else in this corner of the building they could be.

None of that is wrong — a fragment that hollows a wall it does not own must be able to do it without
reading anybody's file — but it is the single fact a new interior fragment most needs told, and it
is not in BRIEF.md. **A sentence saying "the ground-storey window cutters reach 0.90 m past the
inner wall face, and they cut your part too" would be worth more than any other line in that file
to the next person who builds a room against an outside wall.**
