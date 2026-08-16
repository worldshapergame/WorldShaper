# colonnade — what the language could not do, and what it turned out it could

Notes from building `clips/estate/colonnade.clip`: a hundred and sixty-eight columns on an oval,
authored at metre 16. Everything below was found by measuring rather than by reading, and each
one changed how the clip is written.

## 1. There is no angular pattern, so a radiating pavement has to be geometry

`paint <material> where=<pattern>` can key on `stripes`, `checker`, `bricks`, `sine`, `axis`,
`distance`, `fbm`, `cells`, `rasp`, `waves`. `distance` gives a radius, `axis` gives a coordinate,
and there is no way to make a periodic function of either — no `mod`, no remainder — so there is
**no pattern that alternates with angle**, and a radiating pavement is the most ordinary thing a
forecourt has.

What it was written as instead: twenty-four wedges, each the intersection of two half-space
`plane`s through the centre, seven of them drawn from due east to due north and the rest obtained
by two `mirror`s. That works, is exact and is cheap, but it is twenty-five nodes and a paragraph of
comment for what `stripes axis=angle period=...` would have been in one line.

**A `spokes` or `sectors` pattern — value alternating with `atan2(z, x)` — would be used by every
paved court, every rose window and every fan vault on this estate.** It is the only thing in this
clip that had to be built out of the wrong kind of object.

## 2. A non-uniform `scale` is unbounded, so there is no ellipse

An oval forecourt wants an elliptical stylobate, and the obvious way to write one is
`scale { cylinder } x=25.2 z=14.4`. `field.cpp` gives a non-uniformly scaled node the box
`everywhere()` — deliberately, and the comment there argues it correctly: the node reports its
distance scaled by the *smallest* factor, so a cull reading its box would be entitled to drop a
child that could have been nearest. An unbounded child makes its parent unbounded all the way up,
so one elliptical step would have taken the bounding box off this entire clip, exactly the way a
polar `around` does.

`ellipsoid` is a real primitive with a real box, and an elliptical *cylinder* can be faked with a
very tall one — but `sd_ellipsoid` is a bound rather than a distance and a 25 : 20 : 14 ellipsoid is
a slack one.

So the ring is built the way a real one is: straight segments between adjacent columns, twenty-six
of them per arm. That is the right answer architecturally and it is what the brief asked for. It is
worth writing down that it was also the only answer available.

**What would help: an `ellipse`/`elliptic_cylinder` primitive**, or a uniform-scale-and-shear pair
that reports honestly. Not urgent — the segmented ring is better architecture — but a dome on an
oval plan will want it, and so will a groin vault.

## 3. `rotate y=` pins the third axis, and getting its sign wrong is silent

This cost the most time of anything here, so it is first among the traps rather than among the
requests.

`rotate { a } y=t` sends local **+x** to `(cos, 0, -sin)` and therefore local **+z** to
`(sin, 0, cos)`. Once you have chosen the turn that puts +x along the run of a moulded unit, **+z
is decided for you** and there is no second turn left to correct it with. Written the natural way
round — the run taken from station *i* to station *i+1*, the way a chord is normally written — +z
lands on the **inward** normal, and every unit in the clip comes out inside out: the stylobate's
outer step goes into the court, its depth goes out into the lawn, and a hundred and twenty-six of
the hundred and sixty-eight columns stand on nothing at all.

Nothing about it looks wrong in the file, and the shapes are all the right size. What said so was

    components    390 (largest 1647960 voxels, 204402 floating)

and the 204402 was **eleven per cent of the clip**. After the run was reversed:

    components    231 (largest 1869364 voxels, 1140 floating)

The fix is one sign; finding it is the whole afternoon. The generator now asserts
`n . C > 0` for every bay, which is one line and would have caught it before the first sample.

**What would help: a `frame`/`orient` operation** — `orient { a } x=<dir> z=<dir>` — or simply a
line in the grammar note in `BRIEF.md` saying which way +z goes under a y-turn. The grammar says
"in TURNS, not degrees" and nothing about handedness.

## 4. Weathering compounds, and five statements is not five times one

`clip_script.cpp` already documents that a weathering deformation is expensive (2.4 s against
623 s on the facility) and fixes it by cutting the solid at the scope. What is not written down is
that the statements **chain**: each one's shape is the previous one's *displaced* solid, so the
second pays for the first's curvature, the third for the second's, and so on. Measured on this
clip at metre 4, same geometry, only the `weather` lines differing:

| statements | metre 4, CPU | metre 8, CPU |
|---|---|---|
| none | 2.8 s | 11.4 s |
| two | — | 53.7 s (4.7x) |
| five | 33.2 s (**12x**) | — |

Two is what `surface.clip` uses on a bigger building and two is what this uses. It would be worth a
line in the brief: *weathering statements multiply, they do not add.*

## 5. Two things the documents say do not exist, and they do

Both were assumed absent from `BRIEF.md`'s grammar section and both are in `clip_script.cpp`:

- **`around` takes `from=` and `to=`**, in turns, and spans them inclusively: `around { a }
  count=9 axis=y from=0.1 to=0.35` is a nine-column arc. The estate brief says "the language has no
  partial-arc form yet". It has one. (It is still the wrong tool here, for the bounding-box reason
  in `_order.clip`, but the note is wrong as written.)
- **`revolve` takes `from=` and `to=`** as well, which is how a niche head, an apse or a half dome
  should be written.

There is also an **`arc`** primitive — a torus over part of a turn, with `ring=`, `tube=`, `from=`
and `to=` — which is not in the grammar list in either brief. An arched gate would want it.

## 6. What a metre-16 build of this actually costs, because the brief asked

Measured on the machine this was built on — four cores, shared with thirteen other agents' builds,
load average 20 to 30 throughout, so the wall figures are wall figures and not the machine's best:

| | box | matter | components | peak RSS | wall |
|---|---|---|---|---|---|
| metre 8 | 425 x 148 x 252 = 15.9 M cells | 1.89 M voxels | 215, 316 floating | 222 MB | 63 s |
| metre 16 | 849 x 296 x 504 = 127 M cells | 15.1 M voxels | 465, 646 floating | **1.74 GB** | **565 s** |

**Nine and a half minutes and 1.7 gigabytes at metre 16, and that is with the box already cut to
the building plus a metre.** Two things in that are worth someone's attention rather than mine:

- **The peak is 1.74 GB for a 127 M cell box — thirteen and a half bytes a cell**, against the five
  the game's own array uses. The estate brief's own measurement (199 M cells, 2.2 GB) says eleven.
  Whatever the measuring tool holds beside the volume is more than twice the volume.
- **Eight times the cells cost nine times the time**, which is the honest scaling and means nothing
  is quadratic here — but it also means metre 32 on this box is 1.0 thousand million cells and
  something like an hour and a half and fourteen gigabytes. That is why the clip says metre 16 in its header
  and why the estate is a family of clips rather than one.

## 7. Small things

- **`repeat` needs its seed centred in the cell.** A shape translated to 0.5625 inside a 1.125
  period straddles the boundary and the field goes infinitely slack. Translate the *row*, not the
  seed. This is in the briefs, and it is still the easiest line to write by accident.
- **A moulding's `run=` defaults to `z`**, which means `fillet -17 0 0  17 0.18 -0.06` draws its
  *section* 34 m wide in x and extrudes it 0.06 in z. For a fillet that happens to be the same
  solid either way, so `entab_run` in `_order.clip` is right by luck; for the `cyma_reversa` and
  `ovolo` in the same block it is a 34 m long S-curve rather than a cornice profile. Every moulding
  in this clip that runs along x passes `run=x` explicitly.
- **`displace` under half a voxel is dropped**, with a warning, and the warning is right. At
  metre 16 the facility's `amount=0.012` grain is 0.19 of a voxel and does nothing; anything big
  enough to survive (0.032) is over half the thickness of a baluster neck. A clip authored at
  metre 16 should not carry the facility's grain line at all, and this one does not.
