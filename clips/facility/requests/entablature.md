# From the entablature

Four things found building `clips/facility/entablature.clip`. The first two are bugs in shipped
geometry that no contact sheet can catch, because in both cases the wrong answer still looks like
a cornice. The third is a hole in the moulding vocabulary. The fourth is the render tool.

I have worked around all four; nothing here blocks me.

---

## 1. A six-number moulding that runs along x needs `run=x`, and four in `_order.clip` do not have it

`clip_script.cpp` reads a moulding's corners in the frame `run=` chooses:

```cpp
const u32 run  = axis_from(keys.word("run", "z")) % 3u;
const u32 proj = (run == 0) ? 2u : 0u;      // across the face
const u32 high = (run == 1) ? 2u : 1u;      // up it
```

With the default `run=z` the *projection* axis is x. So a member written to run 34 m along x —

```
let entab_run_bed = cyma_reversa -17 1.25 -0.22  17 1.32 -0.14        # _order.clip:336
```

— is read as a section **34 metres wide and 0.07 m tall**, swept 0.08 m along z. The arcs
`build_moulding` strikes have a p-radius of 17 m, so over the whole length of the run the ellipse
is within a millimetre of its own tangent and the moulding degenerates into half of its bounding
box. The profile still steps in the right places, so it renders as a plausible cornice and the
volume is nearly right; every curve in it is simply gone.

Four members of `entab_run` are affected — `entab_run_arch_cr`, `entab_run_bed`,
`entab_run_ovolo`, `entab_run_crown` (lines 334, 336, 339, 341). Adding ` run=x` to each is the
whole fix and changes nothing else.

**The same bug is in `walls.clip`**, and it is worth reporting because of *how* it is half wrong:
`walls_sc_neck_e` and `walls_sc_slope_e` run along z, so the default `run=z` is right for them and
the east and west string courses are correct. `walls_sc_neck_n` and `walls_sc_slope_n` run along
x, so the astragal and the ovolo on the north and south fronts are square. The building has a
moulded string course on two elevations and a boxed one on the other two, which is exactly the
kind of fault that survives every check we have. (walls.clip is not mine and I have not touched
it; last seen at lines 203 and 206.)

Suggested guard: `build_moulding` could report an error when the section's width `|p1 - p0|`
exceeds the run length `|r1 - r0|`, because no real moulding is wider than it is long. That single
test catches every instance of this and cannot fire on anything legitimate.

---

## 2. `entab_profile`'s bed mould and architrave crown have their corners the wrong way round

Independent of (1), and visible the moment the curves are switched back on.

A moulding's solid hugs the `(p0, q0)` corner: the face runs from **p1 at the q0 end** to **p0 at
the q1 end**, and `p0` is where the core it is cut into has to stand. `entab_bed` is written

```
let entab_bed = cyma_reversa 0.22 1.25  0.14 1.32                     # _order.clip:302
```

so its face is 0.22 at the bottom and 0.14 at the top. But the frieze below it stands at 0.14 and
the dentil plate above it at 0.22, so the moulding steps *in* exactly where the cornice steps
*out*; and with `entab_back_bed` backing only to 0.155 there is an open gap between 0.155 and the
moulding's own face over the lower half of its height. The section is not closed. The same is true
of `entab_arch_crown` (line 300), which projects 0.20 at its foot and returns to 0.14 at its head,
overhanging the top fascia instead of springing from it.

Written the other way round both are ordinary members and both close:

```
let entab_arch_crown = cyma_reversa 0.14 0.60  0.20 0.65
let entab_bed        = cyma_reversa 0.14 1.25  0.22 1.32
```

`entab_ovolo` and `entab_crown` are correct as written. `entab_profile` is the section a caller
would put into `revolve` for a circular entablature — the drum will want it — so it is worth
fixing there and not only in the straight run.

---

## 3. A cyma recta cannot be written

`cyma` swells toward `q0` and `cyma_reversa` toward `q1`, and in both the swelling half is the one
whose face is at `p1` — the **full projection**. So the convex half is always at the wide end.
Naming the other diagonal of the same rectangle with the other kind gives the identical solid:
`cyma p0 q0 p1 q1` is exactly `cyma_reversa p0 q1 p1 q0`.

That covers the cyma reversa (convex at the top, wide at the top) which is what an architrave
crown and a bed mould want. It cannot produce the **cyma recta** — convex at the *narrow* bottom,
concave at the wide top — which is the standard crowning member of a cornice and the single most
visible moulding on this building: 94 m of it, in silhouette against the sky.

I built mine out of two members instead, which works and is exact:

```
let entablature_crown_lo = ovolo   -17 1.925 -0.60  17 1.85  -0.66 run=x
let entablature_crown_bk = fillet  -17 1.92  0      17 2.00  -0.66 run=x
let entablature_crown_up = cavetto -17 2.00  -0.66  17 1.925 -0.72 run=x
```

Two suggestions, either would do:

- give the S a `swell=` key naming which end is convex, independent of which end is full; or
- add `cyma_recta` as a parser composition of exactly the ovolo/cavetto pair above, since a cyma
  recta is by definition two quarter-arcs meeting at mid height with a vertical tangent.

The second is three lines in `clip_script.cpp` and no new node type.

---

## 4. The mouldings take no `round=`

`build_moulding` builds its section rectangle with `f.box(centre, half, 0.0)` and there is no key
to say otherwise, so every moulding in the building has dead sharp arrises — including `fillet`,
which is nothing but that rectangle. BRIEF.md's last rule is "every stone edge gets a small round
(by=0.01 to 0.02); sharp arrises read as extruded", and a cornice is mostly fillets: three
fasciae, a frieze, a dentil plate, its cap, and the corona whose soffit edge is the sharpest
shadow line on the elevation.

There is no way to round one in place either. `round { X } by=r` and `offset { X } by=-r` are both
constant offsets of the same distance field, so `round { offset { X } by=-r } by=r` is exactly `X`
— the erode-then-dilate that would round convex arrises without changing the size is not
available.

I worked around it by writing the eight plain members as `box … round=0.010` instead of `fillet`,
which is the same six numbers and the same solid. The four curved members are still sharp where
they meet the members above and below. `keys.number("round", 0.0)` passed into `build_moulding`'s
`f.box(...)` call would fix all of them, and would cost one line.

---

## 5. `tools/views.ps1` dies whenever the renderer prints a warning

`$ErrorActionPreference = "Stop"` at the top, and then

```powershell
$log = & $exe @shot 2>&1 | Out-String                                  # views.ps1:249
```

In Windows PowerShell, redirecting a native executable's stderr wraps each line in an ErrorRecord;
with `Stop` in force, one `[WARN ] frame  frame 1 took 1120 ms` terminates the script and no
contact sheet is written. It fired on roughly one run in three for me, always on the slower builds
— metre 32, or the whole clip — which is precisely when the pictures are worth the most.

`$log = & $exe @shot *>&1 | Out-String` inside a `try { } catch { }`, or simply not redirecting
stderr (it is only used to print errors when the PNG is missing), fixes it. I did not change the
file because everybody is using it right now.
