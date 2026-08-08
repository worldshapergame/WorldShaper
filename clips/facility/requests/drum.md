# Requests from the drum (`clips/facility/drum.clip`)

Four things, in order of how much they cost the building. The first one is not a request, it is a
bug report against a file I do not own, and it is the largest single defect I found today.

---

## 1. `_order.clip` builds TWO flutes, not twenty-four, and TWO eggs, not sixteen

**Every column in this building is currently unfluted and every capital is currently plain**, and
the file says in capitals that those are the largest share of the engine load in the whole scene.

`mirror { a } axis=z` answers a point by asking the child at `|z|`. That means it keeps the child's
**positive** half and reflects it; whatever the child has on the negative side is not copied, it is
**discarded**. And `rotate { a } y=+turns` carries +x toward **−z**. So a quadrant written with
positive angles — which is how anyone writes it — lands entirely on −z and the second mirror
deletes all of it except the one member that straddles z = 0.

Measured, `--clip-part` at metre 64, geometry unchanged:

| binding | what it should be | reported extent | what actually survives |
|---|---|---|---|
| `shaft_quad` | 7 fins over a quadrant | 0.562 × 7.203 × 0.562, on +x and **−z** | the quadrant, on the wrong side |
| `shaft_fins` | 24 fins all round | 1.094 × 7.203 × **0.031** | **2 fins**, at 0° and 180° |
| `cap_carve_quad` | 5 eggs + 4 darts | 0.594 × 0.109 × 0.594, on −z | the quadrant, on the wrong side |
| `cap_carving` | 16 eggs + 16 darts | 1.062 × 0.078 × **0.094** | **2 eggs**, no darts |

A z extent of one voxel at metre 64 is the signature: everything that was rotated is gone.
`order_shaft = intersection { shaft_solid union { shaft_floor shaft_fins } }` therefore evaluates
to `shaft_floor` — a smooth shaft shrunk 0.0375 — with two vertical ribs on it. There are nineteen
of them in the building.

**The fix is to negate the four angles** in `shaft_fin_b..g` and the four in `cap_egg_b..e` /
`cap_dart_a..d`, so the quadrant lands on +x **and** +z where the mirrors can see it. Nothing else
changes; the geometry, the cost and the argument for not using `around` are all still right. I have
done exactly this in `drum.clip` and my sixteens come out as sixteens.

`cap_volutes` is **not** affected — `vol_one` is translated to +z before it is folded — so the
capitals still have their scrolls, which is most of why nobody has noticed.

I have not touched `_order.clip`. It is not mine.

## 2. Neither half of that is written down anywhere, and both fail silently

The clip reported **1 component**, a plausible volume and a plausible surface area with two flutes
on a column and with sixteen windows built as none. My first contact sheet was a smooth drum with
one pilaster on it. That is the only thing in the toolchain that said a word.

Two lines in `BRIEF.md`'s grammar would have saved me an hour and would save the order nineteen
columns:

```
let name = mirror { a } axis=x    # folds the coordinate ONTO ITS POSITIVE SIDE: the +x half of
                                  # `a` appears both sides and the -x half is DISCARDED
let name = rotate { a } y=0.25    # in TURNS. +y carries +x toward -z
```

A cheaper diagnostic would also help: `--clip-part` already knows the worldbox, and a quadrant that
comes back on the wrong side of an axis it is about to be mirrored across is exactly the kind of
thing a person cannot see and a machine can. Even just printing `worldbox` before the material
histogram rather than four screens into it would have caught it.

## 3. `paint ... below=` believes a `difference`, and a `difference` lies at a distance

This one painted a ring of verdigris right round the inside of the rotunda and I only found it in a
path trace.

`difference { a b }` is `max(a, -b)`. That is a true distance near the surface and nowhere else:
stand deep inside the subtrahend and `-b` is a small positive number with no relation to how far
away the shape is. My copper apron was a 5.895 cylinder less a 5.625 one, half-heights 0.0225 and
0.0625, and for **every point inside the drum** — 1.1 m from any copper — the expression is

```
max(d - 0.0225, 0.0625 - d)      d = |y - 12.1725|
```

which bottoms out at exactly **0.0200** at d = 0.0425. `paint copper where=drum_apron below=0.02`
matched it, on a plane, at every radius, right across the interior. Invisible from outside; it
survives all eight elevations; a path trace from inside shows it as a hard teal line.

It is not really a bug in `difference` — an SDF built from `max` is allowed to underestimate — but
it *is* a trap that the paint rule walks straight into, and the brief already spends three
paragraphs on `below=0.02` without mentioning it. Two suggestions:

1. **One line in BRIEF.md rule 5**: *"key a paint rule on a shape whose field is a real distance —
   a primitive, a union, a revolve. A `difference` with a big subtrahend reports a small positive
   number a long way from anything and `below=` will believe it."*
2. If it is cheap, `paint` could test `below` against a shape's *bounding box* first and skip the
   voxel when it is outside the box by more than the threshold. That is conservative for every op
   that has a box, and it would have killed this instance outright.

My workaround is the right construction anyway: the apron is now two revolved sections, and
`revolve` is exact, so it cannot lie. Same reason the whole drum body is one revolve.

## 4. Two toolchain things, both already known, both still biting

- **`views.ps1` dies on any stderr line.** Reported in `requests/windows.md` §3 and still true:
  `$ErrorActionPreference = "Stop"` plus `& $exe @shot 2>&1` promotes the routine
  `[WARN ] frame  frame 1 took 642 ms` into a terminating error, usually after two views of nine.
  I ran every render inside `for (…) { try { … } catch { } }` until the contact sheet appeared,
  which works but throws away a whole build each time.
- **The world cache is not safe for concurrent agents.** Several runs died with
  `[WARN ] cache could not rename 'clips\facility.clip.world.part' into place`, which is two of us
  building the same clip at once. Same workaround. A per-process temp name would fix it.

---

## What I could not do, and chose not to do

- **No weathering.** `weather … on=` is scoped properly now and a drum is the most exposed stone in
  the building, but every coat is a displacement, the manifest already spends 12 mm of `grain_fine`
  on everything, and my glazing bars are 0.030 across. A second displacement eats them and they are
  half of what this part carries. Written into `drum.clip` so it is not read as an oversight.
- **No `void_drum`.** The manifest's `hollowed` difference does not name one and the manifest is not
  mine to edit, so the sixteen windows are cut inside `part_drum`. That is the better answer anyway
  — a drum window is not a room, it hollows nothing belonging to anybody else, and a fragment that
  needs no line in the manifest is a fragment somebody can add a neighbour to without reading it.
- **The pilasters are not `scale { ionic_pilaster } 0.3333`.** I tried it and threw it away: at a
  third scale the flute fillets are 0.010 and the volutes 0.0075, a third and a quarter of a voxel
  at the contract's metre, so sixteen copies of the real order would have cost sixteen spirals and
  a repeat each to render mud. What survives at 0.30 m wide is a shaft, an astragal, an echinus and
  an abacus, so that is what is cut, to the same 9-diameters rule.
