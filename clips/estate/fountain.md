# Requests from the fountain court

Five things the language could not do, or did the opposite of what its documentation says, while
building `clips/estate/fountain.clip`. They are in the order I would fix them. The first is the one
that cost the most time; the last two are documentation rather than code.

---

## 1. There is no tapered prism, so an obelisk is four `plane`s and a lookup table

An obelisk is the simplest classical solid there is: a square shaft with a batter and a pyramidion.
The language has `prism`, which takes `r`, `h` and `sides` and has no taper; `cone`, which is
circular; and `scale`, which stretches a shape rather than tapering it. `intersection { prism cone }`
gives a square shaft whose corners get chamfered further up, which is a different object.

So each of the eight faces of an obelisk is a `plane`, and because `Field::plane` normalises the
normal but leaves `offset` alone, the offset is not a dimension anybody can read — it is

    0.45 * 6.75/6.7516665 + 0.45 * 0.15/6.7516665 = 0.459886

for a shaft that steps from a half-width of 0.45 to one of 0.30 over 6.75 m. Four of those and a
bounding box per obelisk. It works, it is exact, and it is eight lines of arithmetic in a file whose
whole discipline is that every number traces back to M = 0.45.

**What I would like.** Either a second radius on `prism` —

    prism 0 0.45 0  r=0.45 r2=0.30 h=6.75 sides=4 axis=y

— or a `taper { }` one-child operation taking a rate and an axis. A frustum is exact for any convex
section and its distance is no harder than a cone's. An obelisk, a chimney, a pylon, a truncated
pyramid and a tapered pier are all the same missing primitive, and every estate clip that wants one
is currently writing planes.

---

## 2. A moulding section is a RING, and nothing says so

`bead 0.24 2.10 0.42 2.40` inside a `revolve` is solid between radius 0.24 and 0.42 and empty inside
0.24. That is correct — a moulding is a section stuck on a core, and `_order.clip` uses it exactly
that way — but the grammar in `BRIEF.md` describes mouldings as "sections, not solids of their own:
put them inside a `revolve` for anything that goes round a column" and then gives no example of the
core. Written as

    box 0 1.90 -1  0.34 2.10 1
    bead 0.24 2.10  0.42 2.40
    box 0 2.34 -1  0.26 2.85 1

— core, moulding, core — the stem comes out as a solid foot, a hollow tube 0.30 long, and a
disconnected shaft above it. **It builds, it renders, and the only thing that reports it is
`components`.** It cost me two of these (the fountain's lower stem and the urn) and the same shape
of mistake put the urn's cavetto foot in the wrong place a third time.

**What I would like**, in descending order of how much I would use it:

- one line in `BRIEF.md`'s moulding block: *"a moulding is a ring about the axis and has nothing
  inside its first corner — union it onto a core that runs the full height."*
- or a `revolve` that warns when the profile it is given has no matter at radius 0.

---

## 3. `stairs` only climbs toward +z

`sd_stairs` is called with `/*run*/ 2u, /*rise*/ 1u` hard-coded in `clip_script.cpp`, so a flight
always runs along z and always climbs with it. A flight that descends northward, or one that runs
east–west — a terrace with steps down on all four sides is the ordinary case in a garden — has to be
built either as `rotate { stairs } y=0.5`, which moves the flight's own coordinates out from under
you, or as a stack of boxes.

I built the two ten-riser flights beside the cascade with `stairs` because they happen to climb north,
and the three three-riser flights down off the perimeter walk as boxes, because two of them run in x.
The boxes are fine and they are exactly 0.18 and 0.32 — but there are now two constructions in one
file for the same object, which is the thing the module rule exists to prevent.

**What I would like.** `stairs ... run=x` — the key already exists on `wedge` and does exactly this
job there — and a `down=` or a negative `run` for a flight that descends along its axis.

---

## 4. `weather` scope: the cost note in `requests/site.md` is fixed, but the SHAPE of the scope is
   still the author's problem, and now for a second reason

The 211x cost site.clip measured is gone — `apply_weather` now stamps `place` onto every coat it adds
and puts the scope first in the mask's `multiply`, and three weatherings on this clip cost about what
no weathering costs. Thank you.

What is left is that every weathering test reads `cavity`, and `cavity` is 1 for a voxel that is
merely buried, so **a scope that contains solid gets weathered all the way through it**. Measured
here, all three times:

| scope | coat | voxels painted |
|---|---|---|
| box round the whole cascade wall | `lichen` | 15,695, nearly all inside the masonry |
| box 0.45 deep over the apron | `sand` | 55,990, i.e. the top half of the ground slab |
| disc round the basin kerb | `weed` | 85,491, i.e. the inside of the kerb |

None of it is visible and none of it is wrong in the picture. It is wrong in the `materials` report,
which is the instrument this repository uses to decide whether a coat fired on the right amount of
surface — 11% `sand` reads as a catastrophe and is in fact a buried nothing. The fix on my side was
to cut every scope down to a skin: two rings for the basin, a 0.40 m band and the niche void for the
grotto, and a box 0.16 m tall for the apron. That works, and it means the scope is now shaped by what
the weathering costs rather than by where the weathering goes.

**What I would like.** Multiply every weathering coat's test by the same surface test the mesher
already has, or expose `weather ... surface_only=1`. A coat that cannot be seen should not be minted.

---

## 5. Two small ones

**`level=` on `desert` is a dial with no name.** `sea` uses `level` as a tide line, which is
documented and obvious. `desert` uses it inside `smoothstep(-y, -level - 1.5*scale, -level)`, so at
the default `level=0` the drift term is a flat 1.0 at y = 0 and the coat fires on every flat piece of
ground in the scope regardless of `amount`. Lowering the level is the only way to make `desert` light
on a horizontal surface, which is not what a reader would guess `level` means. Worth a line in §6 of
`20-clip-forge.md`.

**`union { a b } smooth=0.02` still does not parse**, and `BRIEF.md` line 159 still says it does.
`requests/halls.md` §2 reported this and it is unchanged. It is a one-line fix in the comment. I lost
nothing to it because halls.md told me, which is the argument for these files.
