# doors — what I could not do, and one bug that is going to bite every fragment

## 1. Displacing a CSG tree resurrects surfaces that were subtracted away

This is the big one. It is not specific to doors; it will happen to anybody who sinks a panel, a
coffer, a channel or a flute and then lets the manifest's `displace { hollowed grain_fine }
amount=0.012` run over it. It cost me most of a session and I only found it because the component
count said 129.

**Symptom.** A part that is one clean component undisplaced comes out of the sampler in dozens of
pieces once the manifest's 12 mm of grain goes on, with loose voxels hanging *three or four voxels
clear* of the surface — far further than 12 mm could ever move anything. Written the obvious way,
`part_doors` came out in **129 components with 433 voxels adrift**. Same geometry, same dimensions,
now **1 component, 0 adrift**.

**Cause.** `difference { a b }` evaluates as `max(a, -b)`. Take a point sitting just outside the
leaf's own outer cylinder, in the middle of a panel that has been sunk 0.045 into it. The true
distance to the nearest surface is 0.045 — the panel floor. But the cutter I used was a *shell* of
that same cylinder, so at that point the cutter is also just outside itself and reports about
-0.006, the `max` picks the leaf's own surface, and the field answers **+0.006: the distance to a
surface that no longer exists.** That is a legal conservative bound and nothing is wrong with it
until something adds a signed 12 mm to the answer, at which point the sign flips and the vanished
surface comes back one speckled voxel at a time, floating over the hollow the cut made.

**The three rules that fixed it**, all of them in `doors.clip` where they apply:

1. **A subtracted region must extend clear of the surface it is cut into, never stop flush with
   it.** Cut with the *solid* cylinder, not with a 0.045 shell of it: `difference { panel_box
   cyl }` rather than `intersection { panel_box shell }`. The same solid, and the field now answers
   +0.051 instead of +0.006. This alone took 437 loose voxels down to 7.
2. **A frame is bars unioned, not a box minus a box.** Three fasciae written as
   `difference { outer inner }` shed voxels under the architrave soffit; written as two jambs and a
   head unioned they do not. Proof that it was the construction and not the geometry: the loose
   voxels *did not move* when I changed the arris round from 0.012 to 0.018, a change that moves
   every surface involved past them on both sides.
3. **A raised moulding on a curved surface must be built by offsetting the surface it stands on.**
   `intersection { ring  offset { leaf } by=0.045 }`, never `intersection { ring  some_other_
   cylinder_that_happens_to_be_parallel }`. I tried a shell, then r=5.445, then r=5.4525; each left
   exactly one voxel adrift, in a different place each time. Offsetting the parent means the bead's
   surface *is* the parent's surface plus a constant, and the two cannot disagree.

A moulding built on a subtraction (`cavetto`, `scotia`, `cyma`, `cyma_reversa`) needs stone behind
it that reaches **into** the moulding's own rectangle rather than stopping at the edge of it —
which is what `entab_run` in `_order.clip` already does with its `bk*` fillets, and I did not
understand why until a loose voxel appeared at the tip of a `cyma` that was backed only to the
edge.

**What I would ask for.** None of the above is a workaround for a mistake — every one of those
expressions is a correct SDF and the sampler is entitled to trust it. The problem is that
`Op::Displace` trusts a *bound* as though it were a *distance*. Two possible fixes, in order of how
much I would like them:

- Make `Field::skip_slack` charge a displaced subtree for the looseness of the CSG under it, not
  only for the amplitude of the pattern. A `subtract` or an `intersect` whose children's surfaces
  are not the result's surfaces can under-report by an unbounded amount; that slack is what the
  displacement then converts into geometry.
- Failing that, a diagnostic. The forge already counts components and prints `floating N voxels
  at ...`. It could say *why*: "this clip contains a displacement over a subtraction" is a one-line
  warning that would have saved me hours, and it will save the next twenty fragments too.

## 2. `wedge` keeps the side ABOVE its diagonal

Worth a line in BRIEF.md. `wedge x0 y0 z0 x1 y1 z1 rise=y run=x` is described as "a ramp", and the
natural reading is that it ramps up from the low corner. It does the opposite: its triangle is the
upright edge, the top edge, and the hypotenuse between them, so a wedge used as a pediment gives
the *valley*, not the gable. My first pediment came off the contact sheet as a flat band with a
notch bitten out of the middle. There is also no way to get the mirrored slope out of `wedge`
directly — `mirror` folds to `|x|` and gives a V — so a gable has to be `box` minus `wedge`, or a
half turn about y, or (what I ended up doing, and what I would recommend to anybody) two `plane`s.

## 3. `tools/views.ps1` dies on a slow frame

`$ErrorActionPreference = "Stop"` at the top of the script plus `& $exe ... 2>&1` means that any
line the renderer writes to stderr aborts the whole run — including `[WARN ] frame  frame 1 took
962 ms`, which is not an error and happens whenever the camera is close to a metre-32 clip. Three
of my renders died on it and re-running the identical command worked. A `2>$null` on that line, or
`-ErrorAction Continue`, would fix it. I do not own the file so I have not touched it.

## 4. Vertical mouldings do not work on a south-facing wall

`run=y` forces `proj=x` and `high=z`, so a moulding running up a jamb has its section drawn with
the *bulge* along x. That is right for a wall in the y–z plane (an east or west face) and wrong for
every wall in this building's main elevation, which faces −z. There is no way to write a bead or a
cyma running up a door jamb on the south front without building it along x and rotating a quarter
turn about z, which needs a rotation sign nobody has documented.

I worked around it: the architrave fasciae are plain rectangular members (which is what a fascia
is), and the astragal round the opening is a flat frame grown by its own radius — `round { plate }
by=0.0225` — which is genuinely what a roll is and mitres the head-to-jamb corner for free. The
real mouldings in my part (`cavetto`, `ovolo`, `cyma`) are all in horizontal runs, where `run=x`
works natively.

Suggestion: let a moulding take `high=` as well as `run=`, so `run=y high=x` gives a vertical
moulding projecting in z.

## 5. Things I chose not to do

- **No weathering.** `weather … on=` would work and is scoped properly now, but weathering is a
  second displacement, and this part exists to measure what *one* displacement does to a subtracted
  detail on a curved surface. A second one on top would make the measurement meaningless. Said in
  capitals at the top of `doors.clip` so nobody adds it as a favour.
- **The leaves do not open.** They are modelled shut, bowed, with a sunk meeting joint. If somebody
  later wants a door that swings, the leaf is `doors_gd_leaf_e` and it is drawn about x = 0 with
  its hanging stile at x = 1.35; it would want rebuilding about its own hinge.
- **The plinth band runs x −6.75 to 6.75** along the back wall of the portico, at the outer column
  axes. It is what makes three doorcases one component, and it is a dado, not a wall base course —
  but if `walls.clip` ever grows its own base moulding in that range the two will need reconciling.
  It projects 0.18 from the wall face and stands y 1.71 to 2.25.
