# 24 — The complex: the estate, the state rooms, and what building them found

*Written 2026-08-16, while a second line of work was in `shaders/`, `src/gpu/`, `src/app/main.cpp`
and `src/forge/world_stipple.*` at the same time. **This file exists because `13-decision-log.md`
was being edited by that other line and two hands in one log is a lost afternoon.** Every entry
below belongs in the decision log and should be folded into it, in order, the next time the two
lines meet. Nothing here is a new kind of document; it is the decision log for one day of work,
kept somewhere it could not collide.*

---

## 0. What was asked for, and what it turned into

*"Overhaul the facility building with the purpose of showcasing the features of the engine and
path tracer, with tons of new rooms and new different other buildings, all in a baroque, rococo,
neoclassical, renaissance style — you can add more completely new buildings if the new things
don't fit on the original, we can make it into a full complex."*

Six new rooms inside the facility, seven new buildings around it, one addition to the clip
language, and one passage built afterwards to fix a fault the rooms exposed. Fifteen agents, each
in files nobody else was in, each handing back a diff and its own numbers; the merge was not
delegated and neither was the report.

## 1. The decisions, with the reason and the measurement

### D-C1. The estate is a family of clips and not one clip. **Measured, not assumed.**

A clip is a dense array over its bounds — five bytes a cell in the game, about eleven in the
measuring tool with its working copies. `_contract.clip` already cuts the facility's box to the
building plus a metre for exactly this reason: 34 × 20.9 × 25 m at metre 32 is 582 M cells and
about 2.8 GB before a single voxel exists.

An estate is a hundred metres across. Before deciding anything, a 120 × 27 × 120 m box at **metre
8** was built and watched: **199 M cells, 2.2 GB peak**. At the authored metre 32 the same ground
is eleven thousand million cells, and there is no machine on which that is a scene.

So each new building is **its own clip at metre 32 in a box cut to itself**, sharing the module,
the materials and the patterns through `include "../facility/_contract.clip"`, and opened on its
own from the game's clip library. That is not a compromise on "a full complex" — it is the only
shape in which a full complex is a thing anybody can load.

**What this costs, honestly:** the buildings cannot be seen in one frame today. Making them
loadable together is a world-assembly problem (many stamps into one sparse world), not a clip
problem, and it is named in §3 as the next step.

### D-C2. Symmetry is not optional OUTSIDE. The rule was amended rather than broken.

`BRIEF.md` rule 3 said the building is symmetric about x = 0, full stop. It was written when the
only rooms were a vestibule, a rotunda and two halls — all four symmetric, because they are the
same room twice or a room on the axis.

A salon east and a chapel west cannot obey it. Mirroring them means two salons and no chapel.

The rule now reads: **the shell is symmetric and the plan of the rooms is symmetric; what is
inside a room is not required to be.** A wing, a window, a pilaster, a cornice, a stair and the
walls that divide the floor are all shell and all still mirror. Real buildings of this century are
exactly this, and the eye that catches a part sized by taste rather than by the module does not
catch a chapel where it expected a salon, because it cannot see both at once.

### D-C3. `tools/clipcheck` — the clip tool without a graphics stack.

Everything a clip author does is `ws_forge`, which depends on the standard library and on
core/world/game and knows nothing about a device. The only front end for it was linked into the
game executable, so asking whether a file parses cost an SDL fetch, a Vulkan SDK and MSVC.

`tools/clipcheck.cpp` is the same four calls — parse, sample, despeckle, measure — with the report
cut to what an author acts on, built by `tools/clipcheck.sh` with g++ in about forty seconds.
**Deliberately not in the CMake build**: the game already contains this tool, and a second copy of
it in the same build system is two things to keep in step.

Fifteen agents gated on it. Without it none of this work could have been checked at all on this
machine.

### D-C4. Forty-odd materials, each chosen because it is a question the path tracer has not been asked.

The old list is a stone building seen from outside and complete for that. A state room of the same
century behind the same shell is gilt on gesso, wax, silvered glass, lacquer, silk, porcelain and
cut crystal, and **not one of those was in it**.

| material | the question it asks |
|---|---|
| `mirror` rough=6 metal=252 | a path that never terminates; a bounce budget spent rather than exhausted |
| `crystal` ior 1.62 | four hundred small refracting solids in front of one light |
| `alabaster` translucent=210 | stone you can see INTO; `marble` at 110 is only half of it |
| `porcelain` translucent=90 lacquer=12 | translucency AND a clearcoat at once, which one lobe cannot fake |
| `velvet` rough=252 sheen=14 | the retro-reflective end, and the opposite failure mode from the mirror |
| `glass_ruby`/`_blue`/`_gold` with `absorb=` | a coloured VOLUME, not a coloured surface |
| `ormolu` 48, `gilt` 64, `gold_leaf` 40 | three golds a stop apart, comparable inside one frame |

### D-C5. Partial sweeps in the language: `revolve from=/to=`, `around from=/to=`, `arc`.

Every apse, niche head, half-dome, arch ring, curved colonnade and oval room is a revolution or a
radial repeat through less than a full turn, and the language could express none of them. The way
round was a full turn intersected with half-spaces: two extra nodes, no bounding box, and no way
at all to say "seven columns spread over a hundred and forty degrees".

Zero is along the first cross-axis and grows toward the second — the sense `atan2` already grows
in and `around` already spaced its copies in. The sweep runs the increasing way, so `from=0.75
to=0.25` is the half turn through the seam. A full turn is stored as exactly 1.0 and takes the old
path. Over an arc `around` gives **n copies and n−1 gaps, first ON `from` and last ON `to`**; over
a whole turn, n copies in n sectors, unchanged.

**The trap, which §4 of `20-clip-forge.md` already records twice:** a node whose returned
MAGNITUDE is wrong rather than its sign. Outside the wedge the honest answer is the distance to
the cut plane or the end cap, not to the surface of the full revolution. Return the latter and
every normal near the cut is wrong while the shape still looks right in a slice — which is exactly
how four hundred voxels of moss once landed in the wrong place. Against a brute-force minimisation
over the sweep angle the worst error is **1.7e-08 at 400,000 steps** and falls as the step does,
so the residual is the brute force's. Gradient magnitude across the cut: **1.000000**.

**The bounding box of a partial sweep is the whole turn's box, on purpose**, and said so at both
sites. A tighter one wants four cases and a seam, and a box tighter than the truth is a silent
hole in the world rather than a slow one.

**Control arms** from a binary built before anything was edited: `part_rotunda`, `part_dome` and
`sampler.clip` **byte-identical** across the change; boxless-node breakdown unchanged at 923 of
3744 and 13 of 47.

### D-C6. The terrace joins after the voids, and not the way a room does.

A pot, a seat and an armillary sphere standing on the lead deck stand in somebody's air exactly as
a sconce does, so a room's void must not cut them — that is D608's whole lesson and putting the
terrace in `inside` would have repeated it. But unlike a fitting it can never be in a doorway,
because every door and window in this building is at least two and a half metres below the deck.
So it joins beside the fittings and does **not** go through the door-and-window difference:
passing it through one could only ever remove something by accident.

### D-C7. The connecting enfilade, which fixes a fault of the dispatcher's own making.

The state rooms were zoned into the south strip of each wing. The interior grand stair arrives at
6.30 on the **north**. The halls' groin vaults fill everything between, up to y 7.00. **There was
no floor at 6.30 joining the two, so a person could climb the stair and go nowhere.**

The same 1.05 m strip — x 14.05…15.10, all that is left between the hall's east wall and the main
block's interior face — is also where `windows.clip`'s east lights had been opening onto solid
unbuilt block, one of whose reveals sawed a 0.56 × 1.80 m hole through the salon's north wall.

One passage on each floor, mirrored, fixes both. It is a *Porzellankabinett* below and a picture
gallery above, because a 1.05 m width is what those rooms actually were.

---

## 2. What building it found — every one of these is SILENT

None of the following produces an error, a warning, or a number in the report that moves. Each was
found by somebody measuring something they did not have to.

**`mirror { } axis=x` folds to the POSITIVE side, and a child drawn on the minus side folds to
NOTHING.** No error. `components 1`. An empty `never fired` list. It removed six sconces from the
salon — backplates only, no arms, no tapers, no flames — and ate a set of desk legs in the library
twice. The only check that finds it is an `intersection` against a box where the thing should be.

**`below=0.02` does not reach anything placed by a transform.** A voxel is decided solid by
coverage and painted by a rule tested at its centre, so `translate`, `rotate`, `around` and
`repeat` leave **3 to 16 per cent** of their own solid unpainted. The ballroom's ceiling trellis
came back with 1021 limestone voxels strung along every rib. **0.035 is what actually reaches.**
`BRIEF.md` still says 0.02 and must be corrected.

**A room drawn to exactly 2.10 m measures 2.0625.** A voxel is solid if any of it is stone, so two
planes 2.10 apart that do not land on voxel boundaries lose one at each end. The crypt's soffit
went to 1.6875 — 1.80 less M/4, a boundary at both metre 16 and metre 32 — and measures 2.125 to
2.156 across the room. **Any room drawn to the minimum has this**, and none of the others checked.

**A moulding runs straight across its own doorway** unless something stops it. Skirting, dado cap
and cornice crossed the salon's entrance at ankle, waist and head height: 822 voxels, and no
number in the report says so.

**`rotate y=t` pins +z to (sin, 0, cos).** Written the natural way round it lands on the INWARD
normal. 126 of the colonnade's 168 columns stood on nothing — 390 components, 204,402 floating,
eleven per cent of the clip — with nothing in the file looking wrong.

**Weathering statements multiply, they do not add**, and the obvious theory about why is wrong.
Five coats cost 33.2 s of CPU at metre 4 against 2.8 s with the lines deleted. The terrace tested
the natural explanation — that the scope's own field is walked per voxel — by rewriting both
scopes from 48 nodes to 2 and measuring again: **29.4 / 28.6 / 30.2 s against 2.3 / 2.7 / 1.9**.
**The control killed the theory.** `roof.clip`'s diagnosis stands: occlusion, curvature and facing
are asked of the WHOLE clip per solid voxel. Three independent measurements of it now.

**A `weather` scope containing solid weathers all the way through it** — occlusion is 1 for a
buried voxel — so 11% of the fountain was `sand` before every scope was cut back to a skin.

**`displace` by a `bricks` pattern is intrinsically speckly, and turning it down makes it worse:**
31 / 41 / 59 / 37 / 21 components at amounts 0.05 / 0.035 / 0.025 / 0.045 / 0.09, against 1 for a
one-axis stripe.

**A non-uniform `scale` reports infinite bounds, exactly like `rotate`.** An oval room is made of
nothing else, so every shape in the chapel is boxless and the union cull cannot help it. There is
therefore no elliptical primitive and the colonnade's oval is 26 straight segments per arm.

**`round { }` on a union can cut it into pieces** — `by=0.006` turned one component into five in
the ballroom while adding volume.

**A stained light cannot sit in its own window opening.** Every ground-storey window cutter is
punched 1.80 m in from the outer face, 0.90 PAST the inner face, and `void_windows` cuts a room's
own fragment too. The chapel's four lights sit 0.03 on the room side of the cutter, held on all
four edges by stone the cutter cannot reach. It is also why the chapel is 2.25 m deep and not the
3.60 it was zoned: the strip z −6.65…−5.70 is a hole it does not control.

**Unchased, and written up in `requests/crypt.md`: a reproducible wrong answer from the sampler.**
Shrinking one box by 0.02 on all six faces grows the part it is differenced out of from 182.11 to
208.10 m³ and pushes its worldbox **0.17 m outside that box**. No warning, still one component.
Beside it: **`round=` on a box does not grow the box**, which contradicts what `stair.clip` and
`halls.clip` are written on.

---

## 3. What is not done

- **The buildings cannot be seen together.** D-C1 explains why a single clip cannot hold them. The
  next step is world assembly: many clips stamped into one sparse world at given origins, which is
  the world's problem and not the clip's. `--clip-at` already exists for one stamp.
- **`ws_tests` has not been run on any of this.** There is no Vulkan SDK, no SDL and no Windows on
  the machine it was built on, so CMake will not configure and doctest is never fetched. The
  thirteen new cases for the partial sweeps pass under a stand-in doctest header — 220,032 checks
  in `test_field.cpp`, 140 in `test_clip_script.cpp`, no failures — which is evidence and is not
  the harness. **Everything here needs one `build.bat` and one `ws_tests.exe` on a Windows machine
  before it is trusted.**
- **Nothing has been RENDERED.** Not one contact sheet, not one screenshot. Every claim in this
  file is about matter — extents, volumes, components, material shares, head heights and slices.
  How any of it LOOKS is unverified, and the one acceptance test this project has is a player
  going to look for a bug and not finding it.
- ~~The whole building has not been measured at metre 32.~~ **Done — with a control arm, and §4
  below is what it says.**
- **`BRIEF.md` still says `below=0.02`.** It is wrong for transformed geometry and the correction
  is named in §2 but not yet made, because two of the fifteen agents were still reading that file.


---

## 4. The assembled building at metre 32, against the building this work started from

Taken last, on an idle machine, at the contract's own resolution — the only one where a book
spine, a glazing bar, a grating bar, an armillary hoop and a crystal drop exist at all. **Both
arms measured on the same binary**: the "before" arm is `clips/facility.clip` as of `aa39c99`,
the commit this work began at, built from the same fragments minus the six new rooms. A counter
taken from inside a change is not a control, and this project has a rule about that.

| | before | after | |
|---|---|---|---|
| volume | 3885.06 m³ | **3715.76 m³** | −169.30, and it is meant to fall |
| exposed surface | 9550.16 m² | **11,860.33 m²** | **+2310 m², a quarter again** |
| faces | 9,779,366 | 12,144,980 | |
| distinct material records | 92 | **203** | |
| components | 1088 | **1122** | **+34** |
| floating voxels | 2482 | **2576** | **+94** |
| solid voxels | 127,305,669 | 121,758,182 | |

No errors in either arm, and — in both — **not one paint rule in the building fired on nothing**.

**The volume falls and that is the crypt.** It hollows 597 m³ out of a podium that had been solid
stone since the day it was drawn and puts about 114 m³ of columns, ceiling and sarcophagi back. A
build whose volume ROSE here would mean the undercroft had not cut, which is exactly the kind of
failure that leaves every other number looking right.

**The surface rises by a quarter**, which is the six rooms being made of matter rather than of
paint. `exposed faces` is the only measurement in this project that notices texture, and 2310 m²
of new face inside a building whose footprint did not change is what two thousand book spines,
1630 crystal drops, 576 porcelain vessels, forty crypt columns and three iron gratings actually
cost.

**The number that matters most is +94.** Six new rooms, containing some thousands of features that
are one voxel through at this resolution and do not exist below it, added **ninety-four stray
voxels** and thirty-four components to the assembled building. The residue was already 2482 before
any of this work. That is the answer to "did the new rooms break the building", and it is the one
claim in this whole day that could not have been made honestly without building the other arm.

**What it does not say.** Nothing here is about how any of it LOOKS. Not one frame has been
rendered, and the acceptance test this project actually has is a player going to look for a bug
and not finding it.
