# stair — what the grand stair needed and could not get

Four things. The first two are engine gaps I worked around. The third and fourth are bugs I found
in files I do not own, and both of them are the silent kind: they cost no components, no floating
voxels and no error, and they delete work rather than adding anything you can see.

---

## 1. `walkability` cannot see a floor under an overhang, so it cannot check head height

`Walkability walkability(const Clip&, i32 max_step, i32 head_room)` in `src/forge/measure.cpp`
takes a `head_room` argument, and it looks like the answer to the brief this fragment was given.
It is not. The function builds a **heightfield**:

```cpp
for (i32 y = clip.size[1] - 1; y >= 0; --y) {
    if (clip.at(x, y, z) == kAir) continue;
    ...                      // measure clear air above THIS voxel
    break;                   // and stop, whatever is below
}
```

For each column it finds the **topmost** solid voxel, asks whether there is `head_room` of air over
it, and stops. Every floor that has something above it — the whole of the passage under this stair,
the whole of every room in the building with a storey over it — is not the topmost matter in its
column, so it never becomes a surface, so it is never tested and never counted in `surfaces`.

The consequence for the facility as it stands: **the only head heights the engine checks are the
ones outdoors.** The vestibule, the halls, the rotunda and this stair are all invisible to it. A
soffit at 1.4 m over a floor would report exactly the same numbers as one at 2.4 m.

**What I would ask for.** Either a second pass that walks *down* each column and treats the top of
every solid run as a floor and the bottom of the run above it as a ceiling — which is about fifteen
lines and gives head height everywhere, indoors included — or a separate `clearance(clip, over)`
that reports the worst floor-to-ceiling gap and where it is. The reporting line already has room
for it beside `worst rise`.

**What I did instead.** `stair.clip` builds its own probe and it is a permanent binding:

```
.\build\bin\WorldShaper.exe --clip-file clips\facility.clip --clip-metre 32 ^
    --clip-part stair_probe_head
```

It is the union of six volumes — 2.10 m over the floor of the passage, over every one of the
twenty-three treads, over both landings and over the gallery, each pulled 0.03 in from the
balustrades so it tests the walking width and not the handrail — intersected with the stair itself.
**It reports an empty clip.** Five sub-probes (`stair_probe_p_pass`, `_ra`, `_rb`, `_land`, `_gall`)
report the same one volume at a time, so a failure can be located rather than only seen.

It caught four real defects that nothing else did, listed at the end.

---

## 2. `tools/views.ps1` aborts on the renderer's own frame-time warning

`$ErrorActionPreference = "Stop"` at line 56, and at line 249 the renderer is invoked as
`& $exe @shot 2>&1 | Out-String`. In Windows PowerShell, redirecting a native command's stderr
wraps each line in an `ErrorRecord`, so the moment the engine prints

```
[WARN ] frame    frame 1 took 340 ms
```

the script dies with `NativeCommandError` and writes nothing. This does not happen for a small
`-Part` at a low metre, because the frames are fast. It happens **every time** for `-Focus`, which
has to build the whole facility, and for any `-Part` above about metre 16. So the one mode the
brief tells everybody to use for checking that their part meets its neighbours is the one mode that
cannot run.

**The fix is one line:** `$log = & $exe @shot 2>&1 | Out-String` becomes

```powershell
$log = (& $exe @shot 2>&1 | ForEach-Object { "$_" }) -join "`n"
```

or simply `$ErrorActionPreference = "Continue"` around that call. I do not own the file.

**What I did instead.** Drove `WorldShaper.exe --screenshot --cam "x,y,z,yaw,pitch"` directly for
every in-place and close view. Two things worth writing down for whoever does that next:

- the camera coordinates in a `views.ps1` log line are in the **scaled** world. The script shrinks
  the clip by `Metre / 32`, so at `-Metre 16` a point at world x = 11.0 is at 5.5 in a `--cam`
  string.
- `yaw` is `atan2(dz, dx)` in degrees measured from +x, and `pitch` is elevation, negative looking
  down. `--cam "6.5,1.3,2.75,180,2"` stands at world (13.0, 2.6, 5.5) and looks west.

---

## 3. `offset { } by=` — the sign in BRIEF.md is the wrong way round, and three fragments have taken it

`Op::Offset` evaluates as

```cpp
case Op::Offset: return eval(n.child[0], p) + a[0];      // field.cpp:1110
```

so a **positive** `by` demands that a point be that much further inside before it counts — it
**shrinks** — and a **negative** `by` **grows**. BRIEF.md's grammar line is

```
let name = offset { a } by=-0.05    # shrinks or grows without rounding
```

which shows the negative value first and does not say which is which. I read it as "negative
shrinks", wrote `by=-0.06` for a paint zone meant to sit inside a pier, and every pier under this
stair came back solid porphyry from plinth to soffit. That one is loud. The three below are not.

**`clips/facility/_order.clip:135`**

```
let shaft_floor = offset { shaft_solid } by=-0.0375
let order_shaft = intersection { shaft_solid union { shaft_floor shaft_fins } }
```

`shaft_floor` is meant to be the shaft shrunk by the flute depth, so that the intersection keeps
only the twenty-four fins standing proud of a sunk surface. With the sign as written it is the
shaft **grown** by 0.0375, which contains `shaft_solid` entirely, so the union is a superset of the
shaft and the intersection is `shaft_solid` unchanged. **The columns are not fluted.** The same
construction and the same sign appear at line 181 for the capital:

```
let cap_ech_floor = offset { cap_ech_solid } by=-0.022
let cap_echinus = intersection { cap_ech_solid union { cap_ech_floor cap_carving } }
```

so **the echinus has no egg-and-dart either**. Both are the largest named engine load in the
building — "the thinnest matter", nineteen columns of it — and both are currently smooth.

**Measured, at the contract's metre 32, by asking for the two bindings on their own:**

| binding | across | voxels |
|---|---|---|
| `shaft_solid` | 0.938 m | 150,844 |
| `shaft_floor` | **1.000 m** | **178,120** |
| `cap_ech_solid` | 0.938 m | 924 |
| `cap_ech_floor` | **1.000 m** | **2,052** |

The "floor" is bigger than the thing it is supposed to be sunk into, in both cases, by exactly
twice the offset. It contains its own parent, so the union contains its own parent, so the
intersection is the parent and the carving is a no-op. It is one character in each of two lines.

**`clips/facility/steps.clip:167`**

```
let steps_shell_core = offset { steps_mass } by=-0.045
let steps_shell      = difference { steps_mass steps_shell_core }
```

`steps_shell` is meant to be a 0.045 rind of the flight, so that the joint slabs cut only the
surface. Grown instead of shrunk, `steps_shell_core` contains `steps_mass`, the difference is
**empty**, `steps_joints` is empty, and the great steps have no joints cut in them at all. That
fragment names those joints as one of its loads ("a groove is the hardest thin matter there is").
It also means `steps_flight` is `steps_mass` untouched, so nothing there is wrong-looking — just
absent.

**Measured:** `--clip-part steps_shell` and `--clip-part steps_joints` both report
**0 components, 0 voxels** at metre 32.

---

## 4. `box ... round=r` GROWS the box by r, and it costs this fragment 15 mm of head height

Measured, not inferred: `box 6.75 3.915 3.15  8.10 4.14 6.75 round=0.015` reports a worldbox whose
top voxel centre is 4.1406, which is above the 4.14 the box was written to. The flat faces move out
by `r` and the arris is turned with radius `r` through the corner you wrote.

That is the right behaviour for a round and `halls.clip` documents it for its transverse arch. It
is not documented in BRIEF.md, where `round=` appears on `box` in the grammar with no note at all,
and it is the difference between a half landing whose soffit is at 3.915 and one at 3.900. This
stair has fifteen millimetres of clearance; that is all of it.

**Ask:** one line in BRIEF.md's grammar block —

```
let name = box x0 y0 z0  x1 y1 z1  round=0.02   # two opposite corners. round= GROWS the box by
                                                # its own radius; write the numbers short if the
                                                # faces have to land somewhere exact.
```

Every fragment in this building uses `round=` on load-bearing faces.

---

## 5. The upper floor is at 6.30 and `halls.clip` has filled to 6.525

Not a request, a note for whoever builds the piano nobile.

Twenty-five risers of 0.18 from `podium_top` 1.80 lands on **6.30**, which is exactly where
`windows.clip` puts the sill of every piano-nobile window (`windows_pn_cut` is `y 6.30 .. 8.55`).
Two independent derivations agreeing on 6.30 is as close to proof as this building offers, so the
upper floor is 6.30.

`halls.clip` fills its block "over the haunches to a flat 6.525 — the level a floor would be laid on
if anybody ever builds above these rooms", which is 0.225 proud of it. Nothing collides: halls owns
`z ±2.925` and this stair is entirely north of `z = 3.15`, and the two never share a column, so the
walkability check never sees the 0.225 as a step. But a floor laid on halls' 6.525 will meet this
stair's gallery 0.225 too high, and one of the two has to move. 6.525 is not a multiple of 0.18
above 1.80 and cannot be reached by a stair built to the contract's riser.

## 6. Where a future upper floor meets this stair

The gallery is `x 10.02 .. 15.25` by `z 4.50 .. 6.75`, top **6.30**, bedded 0.15 into the east wall
at 15.10 and the north wall at 6.60. (This section used to say `x 11.62`, which was the figure before
the gallery ran west to meet the head of the upper flight; the slab has been 10.02 since.) Its open
edges — the ones carrying a balustrade now, because a landing over a 4.5 m drop has to — are:

- `z = 4.50`, from `x 10.02` to `15.10`
- `x = 10.02`, from `z 4.50` to `4.95` (the head of the well)

Butt an upper floor to the first of those and take that balustrade out. **Leave the second**: it is
over the open well and it is the only thing between the piano nobile and the passage below.

Both edges stand at least 0.90 m clear of the nearest window reveal, which is the whole reason they
are where they are; anything added at 6.30 within 1.80 m of an outer wall is inside `void_windows`
and will be deleted by the manifest without a word.

**Done, 2026-08-17.** The floor was butted to the first edge and that balustrade was taken out, from
`x = 13.50` eastward; the second is untouched. See §7.

---

## 7. THE STAIR ARRIVED NOWHERE, AND A HEAD-HEIGHT PROBE CANNOT SEE THAT

This is the finding, and it is the one worth reading. Everything in §1 above is about **what is in
the way**, and this file's `stair_probe_head` answered that perfectly for a stair that went nowhere:
**a 2.40 m box standing over a hole in the floor is empty.** So is a doorway that opens onto a
four-metre drop. Every clearance number in `stair.clip` was true, `--part part_stair` reported one
component a wing, and from the top of the stair a person could reach **no room at all.**

The second half of a walkable route is a **floor** measurement, and it has to be made against the
assembled building because a route crosses fragments. It now lives in
`clips/facility/requests/stair-route-probe.clip`:

```
bash tools/clipcheck.sh clips/facility/requests/stair-route-probe.clip --metre 16 --part sr_floor_all
bash tools/clipcheck.sh clips/facility/requests/stair-route-probe.clip --metre 16 --part sr_head_all
bash tools/clipcheck.sh clips/facility/requests/stair-route-probe.clip --metre 16 --part sr_cut_check
```

`sr_floor_all` is a skin **0.10 m thick under the whole walking line, MINUS the assembled building**.
Anything it reports is floor that is not there.

### What is on the piano nobile, and what could reach what

Four rooms stand at `y = 6.30` and no others — the two stair galleries (this file), the two picture
galleries (`enfilade.clip`), the **ballroom** on the east front and the **library** on the west.
`vestibule.clip` stops at 6.30, `rotunda.clip` has no gallery, `halls.clip` is solid to 6.975, and
`terrace.clip` is on the roof with no stair to it. Before this change:

| from | to | why not |
|---|---|---|
| stair gallery | picture gallery | **no floor**, `x 13.50..15.25` by `z 3.115..4.50`, and a balustrade across the only line it could have crossed on |
| stair gallery | ballroom | the same hole; the ballroom's only door is at the far end of the picture gallery |
| stair gallery | library | the same hole, west wing |
| ballroom | picture gallery | this one worked, and it was the only pair that did |

Both other files had already written it down and neither could fix it. `requests/ballroom.md` §5:
*"the doorway is where it would arrive … and it opens onto air."* `enfilade.clip`'s own header:
*"Between them, x 14.05..15.10 by z 3.10..4.515, THERE IS NO FLOOR … Bridging it means taking out
stair.clip's own south balustrade at z = 4.59, which is that file's line and not mine."*

### Measured, at metre 16, on `stair-route-probe.clip`

| part | before the bridge | now | what it is |
|---|---|---|---|
| `sr_floor_all` | **1,922** | **174** | missing floor under the walking line |
| `sr_head_all` | **2,218** | **56** | matter standing in the walking line |
| `sr_cut_check` | — | **EMPTY** | nothing of this part inside `void_enfilade`, `void_ballroom` or `void_library` |

The 1,922 was four pieces: **874 in each wing** where the bridge now is, and the two thresholds in
§8.3. The bridge closed 1,748 of them and the remaining 174 are not mine.

The 56 are one component and every one of them lies inside `sr_door_w`, so the other four legs are
empty by construction — the five boxes are disjoint — and `sr_head_foot`, `sr_head_rake` and
`sr_head_gall` were each confirmed EMPTY on their own as well. **All 56 are `part_library`'s own
cornice frieze crossing its own doorway** (§8.5), which `sr_who_lib` reports and `sr_who_enf`,
`sr_who_wal`, `sr_who_hal` and `sr_who_fit` deny. Nothing of any other fragment stands anywhere on
the route, in either wing, at either level.

Two readings on the way there are worth keeping because they are what the boxes had to be tuned
against, and both were somebody's real member rather than a probe artefact:

- **168 voxels a wing** in `sr_head_brg` when its east limit was 15.06 — enfilade.clip's window
  architrave standing 0.27 m outside its own room, in the stair well (§8.4). Marble, which is what
  `enf_arc_face` paints, and 2 voxels a wing at metre 8 where a 13 mm sliver nearly vanishes.
- **364 voxels of `wax`, `taper`, `gilt` and `ormolu`** in `sr_head_all` when the picture-gallery box
  was written 14.00 rather than 14.05 — enfilade.clip's girandole arms and their flames, which reach
  14.045 and which that file's own walking band is written to clear. Attributed by the material
  breakdown rather than by a separate run, which is all that was needed to move a box 45 mm.

### What was built

`stair_brg_slab`, `x 13.50 .. 15.25` by `z 3.05 .. 4.65`, top 6.30 and soffit 5.895, lapping 0.065
into `enfilade.clip`'s floor slab at the south and 0.15 into the gallery at the north; the gallery's
south balustrade cut back to `x = 13.50` with a corner newel on 13.59; a balustrade of four
balusters and a moulded rail down the bridge's west edge, dying into `enfilade.clip`'s wall at one
end and into that newel at the other. The route is **1.3975 m in the clear** across the bridge.

---

## 8. FIVE THINGS ANOTHER FRAGMENT HAS TO MOVE, ALL OF THEM MEASURED

None of these can be done from `stair.clip`. They are in the order that matters.

### 8.1 `ballroom.clip` and `library.clip` — BOTH upstairs doors are about 0.87 m, and 1.00 is the figure

Both rooms write a door over a metre wide and neither of them **is** one, and they fail it for
opposite reasons. Measured with `sr_door_e` and `sr_door_w`, and attributed with `sr_who_*`, which is
the same box asked of one part at a time so that "something is in the way" becomes "whose".

**The ballroom is blind on the CORRIDOR side.** `ball_door_void` is `x 13.725 .. 14.85` — 1.125
(2.5 M), and that file widened it from 0.90 for exactly this reason. `enfilade.clip`'s west wall face
stands at `x = 13.95` from `z = -2.965` northward, straight across the west 0.225 of it. **Aperture
13.95 .. 14.85 = 0.90 m.**

**The library is blind on the ROOM side.** `library_door` is `x -15.00 .. -13.95` and the whole 1.05
of it is clear in the corridor — its east jamb is flush with the halls block, so nothing of
enfilade.clip is in it. Then `library_room`, that file's own air, begins at `-14.85`: the west 0.15 of
the doorway opens onto the library's own 0.25 m west lining, floor to vault. And `lib_low_ends`, the
board that closes the press carcase where the doorway cuts it, takes `-13.995 .. -13.950` over
`z -4.08 .. -3.40`. **Aperture -14.85 .. -13.995 = 0.855 m.**

A 1.00 m box in the west door caught **488 voxels at metre 16**, and `sr_who_lib` reports the same
488 — every voxel of it `part_library`'s own stone. `sr_who_enf`, `sr_who_wal`, `sr_who_hal` and
`sr_who_fit` are all EMPTY.

**The asks, one number each:**

- `ballroom.clip`: **move the east jamb from 14.85 to 14.95**, and the opening is `13.95 .. 14.95` =
  1.00 m exactly with both jambs real. That file has written down what makes it awkward — *"the
  doorcase reaches 0.1125 past each jamb and its east edge was already at 14.9625, with only 0.1875
  of wall left to the room's east face"* — so the case's east ear has to be suppressed and returned
  against the east wall, which is what a doorcase in a corner does anyway. **The west jamb can move
  to 13.95 in the same change** and lose nothing: those 0.225 are blind already.
- `library.clip`: **move the whole opening 0.15 east, to `-14.85 .. -13.80`**, so its west jamb lands
  on that room's own west face instead of 0.15 behind it. `-13.80` is inside `halls.clip`'s block,
  which is why that jamb was put at `-13.95` in the first place — so the alternative, and it is
  probably the better one, is **`-14.85 .. -13.85` and a 0.10 rebate in the halls block above 6.30**,
  which §8.2 asks for anyway and for the same 0.10. Either gives 1.00 m. `lib_low_ends`' east board
  then wants to follow the jamb.

### 8.2 `enfilade.clip` + `halls.clip` — the only route on this floor is 1.15 m and the figure is 1.20

The picture gallery is every millimetre between `halls.clip`'s block face at 13.95 and `walls.clip`'s
inner face at 15.10, and enfilade.clip says so in as many words. **1.20 does not fit between those
two faces**, so this is arithmetic rather than a measurement, and it is the whole 6.05 m length of
the only way from the stair to either state room. What is left after the members that stand in it —
the base course cap and the impost to 13.995, the girandoles and sconces to 14.045, the east window
architrave to 15.047 — is **1.00 m, and `sr_head_gall` proves that much is clear to 2.40 m tall.**

It meets the standard for an *opening* and it is 0.20 under the standard for *circulation*, which is
what a 6 m passage is.

**The ask, and it needs both files:** `halls.clip`'s block above `y = 6.30` is fill — its vaults
crown at 6.525 and it is flat to 6.975 for a floor nobody has laid — so its east face there can come
back from 13.95 to **13.85** at no cost to any room. If `enfilade.clip`'s upper west wall follows it,
the strip is **1.25 m** and the route is over the figure everywhere. The niches are the constraint on
that file's side: they are 0.225 deep off 13.95 and the wall's back is at 13.585, so moving the face
to 13.85 leaves 0.04 behind a niche unless the back moves with it.

**The bridge does not have this problem and that is why it is 1.60 m in the clear** rather than
matching the archway: west of 13.95 there is nothing at all, so `stair.clip` took the 0.45 the rest
of the route cannot have. Its slab is 1.75 wide, 0.15 of that buried in the east wall, and
**1.3975 m of it is walkable past its own balustrade.** The pinch is therefore unambiguously one
fragment's rather than shared between three.

### 8.3 `ballroom.clip` and `library.clip` — the floor slab does not touch `enfilade.clip`'s

`ball_floor_flat` is `box 6.00 6.00 -6.65 15.15 6.30 -3.00 round=0.012`, so it ends at
**z = -2.988**. `enf_slab` is `box 13.60 6.00 -2.95 15.20 6.30 3.10 round=0.015`, so it begins at
**z = -2.965**. **There is 23 mm of nothing between them**, right under both doorways, and the same
on the west with `library.clip`. BRIEF.md rule 4 asks for 0.05 of lap and says why: *"do not butt it
exactly, because a surface displaced by weathering can open a hairline gap."*

It is what `sr_floor_all` still reports — **174 voxels at metre 16, 96 west and 78 east** — and it is
the only thing it reports. **And it is real rather than an artefact of a deep field**, which is what
the control arm is for: `sr_thresh_e` is the same box intersected with the building instead of
subtracted from it, and it comes back **234 voxels in TWO components**, split on the joint. The floor
either side of both upstairs doorways is not one solid.

**The ask: one of the two laps the other by 0.05.** Either the room slabs run on to `z = -2.90`, or
the enfilade's slab runs back to `-3.05`; it does not matter which, and it is one number in one file.

### 8.4 `enfilade.clip` — the east window architrave stands 0.27 m outside the room it dresses

`enf_arc_u_seed` is built `mirror { } axis=z` about each window axis and translated to `z = ±2.70`,
so the jamb for the north light lands at **z 3.3745 .. 3.4805** and its head runs the whole bay at
**y 8.542 .. 8.648**. enfilade.clip's own north face is **z = 3.115**. Both of them therefore stand
in the stair well, 0.045 proud of `walls.clip`'s face, reaching **x = 15.047**.

They cost this route nothing — the bridge is 1.3445 m clear past them and `sr_brg_e` is written to
15.04 for that reason — and nothing else in the building is up there. **It is written down rather
than asked for**, because the member is right and only its z extent is a surprise: anybody who later
widens the bridge east, or hangs anything in the stair well above 6.30, will meet it.

### 8.5 `library.clip` — the lower cornice frieze crosses its own doorway at 2.015 m

The last 56 voxels on the whole route, and the only ones left in either wing.

`lib_lco_frieze` is `difference { box -14.90 8.325 -6.65 -6.70 8.415 -3.40 round=0.010, box -14.22
8.28 -5.97 -7.38 8.46 -4.08 }` — a ring, and its **north run is `z -4.08 .. -3.40` across the whole
of `x -14.90 .. -6.70`**, which is straight over the doorway. `round=0.010` puts its soffit at
**8.315**, so the clear height where a person enters that room is **2.015 m** against the 2.10 an
opening is held to. `lib_lco_back`, `lib_lco_bed_n` and `lib_lco_corona` are on the same line above
it. It is `gilt`, and `sr_who_lib` says every voxel of it is `part_library`'s.

**That file's own head-height table does not have this number in it.** It records *"under the lower
cyma 8.415 - 6.30 = 2.115, the lowest projecting moulding in the room"* — which is the cyma's
springing and not the frieze's soffit 0.09 below it — and *"through the door 8.700 - 6.30 = 2.400"*,
which is the doorhead. Its own `--gap y@-14.40,-3.20` reading of 2.375 was taken at `z = -3.20`,
**inside the wall thickness, where the cornice is not.** The cornice is at `z -4.08 .. -3.40` and the
wall is `-3.45 .. -3.00`; a column 0.25 m further south would have found it.

This is the fault `enfilade.clip` warns about at the head of its own trim section, word for word:
*"salon.clip measured 822 voxels of its own skirting, dado cap and cornice standing straight across
its own doorway — three bars at ankle, waist and head height — which nothing in a component count, a
volume or a material list will ever report."*

**The ask: break the cornice's north run over the doorway**, the way that file's own trim runs stop
short of every other opening, and return it on the two jambs. `ballroom.clip` does not need this: its
cornice is at 9.585 and its door head at 8.70.

---

## 9. THIS FILE HAD THE RIND BRIEF.md RULE 5 IS ABOUT, AND IT WAS ALREADY IN THE REPORT

Not a request — a fault of my own, found on the way and fixed in the same change, written here
because the way it hid is the useful part.

All five paint rules said `below=0.02`, and the reason written beside them was the manifest's 12 mm
of grain — which **the sampler drops**, and says so in a WARN line on every single run. So the number
was never sized against the thing it named. What actually pushes a voxel outside its own rule is
coverage against centre: a voxel is solid if ANY of it is in the shape and painted by a rule tested at
its **centre**, and for a shape a transform put where it stands, 3 to 16 per cent of its own solid
voxels fall outside its own test at 0.02.

**Almost nothing in this file is drawn where it stands.** Both flights' nosings are chains of
translates, so are the wear ellipsoids, so is the second pier; all 51 balusters and every newel are;
and the raking plinths, the wall strings and the upper flight's waist are `translate { rotate { } }`.
The granite, the marble and the bronze rules are now **0.035**.

**MEASURED, TWO ARMS, BY THE METHOD `requests/ballroom.md` SETS OUT** — and that method is now a file
rather than a paragraph: `clips/facility/requests/stair-rind-probe.clip`. The stair on its own — the
contract, the order, windows.clip and stair.clip, nothing else — over a base coat of `lawn`, in a box
narrowed to the stair so a metre 16 run is seconds. Any `lawn` voxel inside `part_stair` is a voxel of
this fragment's own geometry that no rule of this fragment reached. The two arms differ only in
`stair.clip`:

| `--part part_stair` | voxels | material records | `lawn` |
|---|---|---|---|
| metre 32, at HEAD (`below=0.02`) | 1,653,274 | 5 | **824** |
| metre 32, with the bridge and 0.035 | 1,714,196 | **4** | **none** |
| metre 16, at HEAD | 211,514 | | **3,294** (1.56%) |
| metre 16, with the bridge and 0.035 | 219,164 | | **1,680** (0.77%) |

**At the contract's own metre the rind is gone** — `lawn` is not a record in the after arm at all, and
the part is 60,922 voxels bigger at the same time. At metre 16 it halves and does not vanish, and that
is the arithmetic working exactly as BRIEF.md rule 5 describes rather than a residue worth chasing:
0.035 is **1.1 voxels at metre 32 and 0.56 of one at metre 16**, so it clears the half-voxel of
coverage-against-centre error at the metre this building is sampled at and not at half of it. The
contract's metre is 32.

*`--part part_stair` on the whole facility is NOT the instrument for this, and reading it that way
cost an hour.* It reported `limestone 838` and `limestone 394` and both look like the same fault; the
838 does not move when the rules change, because on the assembled clip the stair's voxels are also in
range of podium.clip's and walls.clip's rules where it beds into them. The base-coat arm above is the
only version of this measurement that answers the question asked.

---

## 10. WHAT THE BRIDGE COSTS THE COMPONENT COUNT, SAID PLAINLY

The whole building at **metre 8** goes from **660 components to 664** and from 1,840,191 voxels to
1,841,035. The +844 is the bridge less the balustrade taken out of the gallery. **The +4 is four
single voxels** — the report's own floating total goes 6,318 to 6,322 — and the same two arms say
where they come from and, more usefully, that at the metre this building is sampled at they are not
there at all:

| `--part part_stair` | metre 8 (in the building) | metre 16 | **metre 32** |
|---|---|---|---|
| before | 34 | 82 | **416** |
| after | 38 | 84 | **416** |

**At the contract's own metre the bridge and its balustrade cost nothing: 416 components either
way.** The rise appears at metre 16 and doubles at metre 8, which is the signature of geometry finer
than the grid: a baluster's neck is 0.063 across — two voxels at metre 32, ONE at metre 16, half of
one at metre 8 — so it severs at any metre coarser than the contract's and the cap above it becomes a
piece of its own. There are four more balusters on the piano nobile than there were, in a second run
where there was one, and that is the whole of the four.

`enfilade.clip` writes the same thing up for its own count in the same words: *"78 AT METRE 8 IS THE
GEOMETRY BEING FINER THAN THE GRID AND NOT A FAULT ... Judge this part at 32, which is the contract's
own metre."*

**And 416 is what this stair measures at metre 32, which is not what this file has been claiming.**
The last line of the section below says *"`--clip-part part_stair` at metre 32 now reports 1 component
per wing"*. In the rig above — the contract, the order, windows.clip and stair.clip, box narrowed to
the stair, nothing clipped — the stair **at HEAD, before any change of mine**, reports 416, with the
two largest floating pieces 149 voxels each at the top of the gallery's own handrail. The claim does
not reproduce and it is left standing in that section only because it is that section's own history;
the number to quote is 416, and the useful fact about it is that it does not move.

**The porphyry rule stays at 0.02 and must.** It is the one rule here that is a *stencil* rather than
a solid — a zone got by shrinking the pier by 0.06 — and BRIEF.md says a stencil should not have the
larger number. It has 0.06 of margin and 0.035 plus half a voxel at metre 32 is 0.051 of it.

---

## What the probe caught that nothing else did

Kept here because each one passed every other check in the building — one component, no floating
voxels, no parse error, and a contact sheet that looked like a stair.

1. **The upper flight was a solid block.** Its eleven nested tread boxes were nested from the wrong
   end — all starting at the same western edge instead of ending at the same eastern one — so the
   tallest box covered every other and the flight came back as a 2.16 m cube with a flat top. It
   measured 25,124 voxels, which is a perfectly plausible number for a flight of stairs.
2. **The waist was measured perpendicular instead of vertically.** A rotated box whose bottom face
   sits 0.405 below its local origin puts that face 0.405 away *along the normal*, which is 0.465
   straight down. The passage was 2.055 m high.
3. **The half landing's east lip stood on the flight below it**, making the twelfth tread 0.22 deep
   instead of 0.32 — a 0.18 step in the middle of a stair.
4. **`round=` growing the landing slab** put its soffit at 3.900 and the clearance at exactly 2.100
   over a floor that is itself grown to 1.818 by the podium's own round.

And two the *walkability* number caught, both of them the handrail rather than the stair:

5. A rail 0.18 tall with its fillet the widest member reads, in the one voxel column that grazes its
   edge, as 0.7375 above the landing — inside the 0.75 that `measure.cpp` treats as a step somebody
   meant to take. **This handrail, not the great south steps, was the worst rise in the facility.**
   It is now M/2 tall with the ovolo the widest member, and the shallowest a column under it can
   read is 0.835.
6. A newel 12 mm proud of its rail — which is what `round=0.012` does to a newel written to the rail
   height — is a 7-voxel rise beside a building whose worst step is meant to be 6.

`--clip-part part_stair` at metre 32 now reports **1 component per wing, worst rise 6 voxels
(0.19 m)**, which is the great steps' own riser and not a millimetre more.
