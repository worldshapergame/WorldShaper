# portico — things I could not do, and things somebody else should look at

Written while building `clips/facility/portico.clip`. Nothing here blocked the part; all of it is
either somebody else's file or somebody else's tool.

## 1. The order's volute detaches at intermediate resolutions (order agent)

`ionic_column`'s outer volute tip floats free of the capital at some sampling metres and some grid
phases. Measured on `part_portico`:

| metre | components | floating |
|---|---|---|
| 8  | 1  | — |
| 12 | 1  | — |
| 16 | 7  | 6 voxels, one per volute |
| 24 | 11 | 18 voxels, two or three per volute |
| 32 | 1  | — |

**It is clean at the contract's metre 32, which is the resolution the building ships at**, so I
have not worked around it. But it is worth fixing, because every fragment that places a column or
a pilaster will see it in a preview render and think it is their fault.

The cause is a tangency in `_order.clip`, not a sampler bug. `vol_face` is a cylinder spanning
z 0.2475 .. 0.2925; `vol_band` is the spiral, tube radius 0.0225, on the plane z = 0.315, so it
spans 0.2925 .. 0.3375. The two touch **on exactly one plane, z = 0.2925, with zero overlap.**
`vol_eye` bridges them at the eye, so the inner end is safe; the outer end of the spiral — at
radius 0.18 from the eye, where nothing else reaches — is attached along a tangent line only. The
strays are all at that radius:

```
metre 16, column on x = ±1.35: strays at (±0.844, 9.719, -11.469) and (±0.844, 9.719, -10.875)
                               eye at     (±1.0125, 9.6975, ∓0.315 of the axis), r = 0.170
```

The fix is one number: move `vol_band` to `z = 0.30` (or give `vol_face` `h=0.09`), so the tube
sinks 0.0225 into its face plate instead of resting on it. That is the same 0.05-overlap rule the
brief applies to everything else, applied inside the capital. It does not change the silhouette.

I did not do it: `_order.clip` is not mine, and a column is placed nineteen times.

## 2. `tools/views.ps1` dies whenever the engine writes a line to stderr

`$ErrorActionPreference = "Stop"` at the top plus `& $exe ... 2>&1` at line 249 means any native
stderr output becomes a terminating `NativeCommandError` and the whole run stops. It cost me two
render passes; both times the only thing on stderr was

```
[WARN ] frame    frame 1 took 250 ms
```

which is not an error at all — it is the path tracer's first frame being slow, which it always is.
Worse, the script had already written some of the tiles, so it half-succeeded silently.

Suggested fix: wrap the call, or set `$ErrorActionPreference = "Continue"` around just that line
and judge success by whether the PNG appeared, which the script already does two lines later.

Workaround I used: call `WorldShaper.exe --pathtrace --cam x,y,z,yaw,pitch --screenshot FILE`
directly and work out the cameras by hand.

## 3. `--clip-symmetry` reports nothing usable once `variation` is on

```
symmetry x   42160 cells differ (0.4613%)
```

42160 is the exact voxel count of the part. Every solid cell "differs" from its mirror, because
`mirror_mismatch` in `src/forge/measure.cpp` compares the full clip cell — which by then carries
the per-voxel `variation` record, and that is random per voxel by design. So the check reports
total asymmetry for a part built entirely out of `mirror { } axis=x`.

It should compare the material identity, or run before `variation` is applied. As it stands the
one tool that would catch a drifting half elevation cannot.

## 4. The soffit's south edge depends on where the entablature puts its architrave

Not a request, a note for whoever builds `entablature.clip`. This soffit stops at **z = -11.15**,
the column axis, and its front 0.90 (`-11.15 .. -10.25`) is plain — that band is meant to be the
underside of your architrave, and the first coffer row starts behind it.

If your architrave over the portico is 0.90 (D) wide centred on the column axis, i.e. its back
face on **z = -10.70**, everything lands: you cover my plain band, and there is a 0.45 reveal
between your back face and my first coffer. If your back face ends up north of -10.25 you will
bury the first row of coffers. Anything south of -10.70 is free.

## 5. The engine binary changed under me mid-session

`build/bin/WorldShaper.exe` was rebuilt at 04:17:58 while I was measuring. The same clip, sampled
at metre 32, went from **1,612,700 field evaluations in 2.45 s** before it to **21,568,214 in
11.2 s** after — 13× more work for identical geometry. `src/forge/sample.cpp`, `field.cpp` and
`clip_script.cpp` were all dirty in the working tree at the time, so somebody is mid-change in the
sampler. I mention it only so that the next person to measure a fragment and find it slow checks
the binary's timestamp before rewriting their clip.
# portico — what I needed and could not have, and three cuts that were not cutting

Written while auditing `clips/facility/portico.clip`, `entablature.clip` and `pediment.clip` for
the one question "does every cut remove exactly what it should". Three of the four things below are
faults found by measurement, not by reading; the fourth is a request to another fragment.

---

## 1. THE TWENTY-FOUR FLUTES ARE NOT IN THE BUILDING — `_order.clip:135`

    let shaft_floor = offset { shaft_solid } by=-0.0375
    let order_shaft = intersection { shaft_solid union { shaft_floor shaft_fins } }

`Op::Offset` is `d + a[0]` — `field.hpp:162` says so in as many words — so `by=-0.0375` GROWS the
shaft by 0.0375 instead of shrinking it. A union containing a grown copy of the shaft contains the
shaft, so the intersection is the shaft, and `order_shaft` is a plain turned column.

    ./build/clipcheck clips/facility/requests/portico-flute-probe.clip --metre 32 --slice y@5.00

is a thirty-voxel circle with not one notch in it. Nothing else reports it: the clip parses, the
extent is right to the millimetre, the component count is 1, every paint rule fires and the volume
is what a plain shaft's volume should be. **A cut that removes nothing is invisible to every number
except the one nobody takes.**

`cap_ech_floor = offset { cap_ech_solid } by=-0.022` on line 181 is the same mistake, so the
egg-and-dart on nineteen Ionic capitals is not carved either. I have not touched that one — it is
0.022 m, under a voxel at metre 32, so it is a smaller loss and it needs the capital rebuilt rather
than one line added.

**Asked for:** the sign, in `_order.clip`, on both lines. `by=0.0375` and `by=0.022`.

**And then a second thing, which is why the sign alone is not enough.** `offset` on a `revolve` does
not give a shrunken solid. A revolve measures distance in the profile's own plane, and this
profile's inner edge IS the axis, so on the axis the field reads 0 rather than -0.47; shrink the
solid and the first 0.0375 m round the axis falls outside the result. The shaft comes out with a
0.075 m bore drilled down the middle of it — invisible from outside, which is the only reason it
would ever ship. It was in my first version of this fix and a slice at mid-height found it.

What works is shrinking the ARC and revolving that, which is what portico.clip now does:

    let portico_shaft_floor = revolve { intersection { shaft_band  offset { shaft_arc } by=0.0375 } } axis=y

The band still reaches r = 0, so the axis stays solid, and the floor still runs parallel to the
entasis, which is the whole reason the order reached for an offset rather than a straight cutter.

**Until `_order.clip` changes**, portico.clip cuts the flutes its six columns should have had out of
`ionic_column` locally, using the order's own `shaft_band`, `shaft_arc` and `shaft_fins` and no new
dimension. If the order is fixed, that subtracts a set which is already absent and changes nothing.
The eighteen pilasters and this file's two antae were never affected: `ionic_pilaster` cuts its
flutes with plain boxes and has always been right — so for a year a fluted anta stood at the end of
a colonnade of six unfluted columns.

---

## 2. THE PORTICO'S TWENTY COFFERS WERE FILLED IN BY THE ENTABLATURE

Both files are mine, so this is fixed rather than requested; it is here because it is the clearest
example in the building of a class of fault that no single fragment can see about itself.

portico.clip cuts twenty coffers out of its own soffit slab and says, correctly, "this part removes
nothing from anybody". entablature.clip's `entablature_pbody` was a solid block over the whole
portico from 9.90 to 11.90 and said, correctly, that below 10.80 it "is the same volume
portico.clip's soffit slab already occupies, which is what makes the two certainly one piece". Both
sentences are true. The manifest unions the two parts, and a union after a difference is a refill:

    intersection { part_entablature portico_soffit_slab }   219600 voxels — the WHOLE slab
    intersection { part_entablature portico_coffers }        13216 voxels — the WHOLE cut
    the portico's own ceiling, part_portico  ∩ its box      206456 voxels
    the same box out of the finished `built`                234240 voxels

13216 is exactly what the `difference` takes out and exactly what the union put back. The ceiling in
the built facility was a flat pan: no coffers, no twenty gilt rosettes, and `gilt` — which appears
nowhere else on the outside of this building — was not in the exterior at all. It was a paint fault
in the same breath, because part_entablature is painted marble after portico.clip's rules, so the
limestone ceiling that exists to be compared with the marble columns 0.05 m away came out marble.

The fix is in entablature.clip: the portico block is cut off the ceiling volume, keeping a 0.05 lap
inside the slab's edge on every side so the two are still one solid by sharing stone.

**The general point, for whoever writes the next fragment:** "my cut cannot reach anybody" and "my
cut survives assembly" are different claims, and only the second one matters. The first is provable
inside one file. The second is not provable inside any file, and until `--cuts` existed the only way
to ask it was to intersect your own void with everybody else's part and read the volume.

---

## 3. THE SIDE DOOR'S VOID TAKES A BITE OUT OF BOTH ANTAE — for doors.clip

    let doors_sd_void_e = box  4.50 1.80 -7.95   6.30 4.95 -6.50

Measured, in the finished building at metre 16:

    intersection { part_portico void_doors }
      118 voxels, 28.8 litres
      x -6.375 .. 6.375   y 1.750 .. 2.250   z -7.688 .. -7.438

That is the west corner of the east anta's base and its mirror on the west, both of them: a piece
0.225 m wide and 0.45 m tall out of a plinth that is 1.35 m square, at the foot of the order, in
plain view from the top of the steps.

Neither element is wrong on its own. The anta stands on the corner column's axis at x = 6.75, which
is where an anta goes; its base plinth is the order's own and projects 0.675 from that axis, so it
reaches x = 6.075. The side door is 1.80 m wide centred on 5.40, the middle of the portico's outer
bay, so its opening reaches x = 6.30. They overlap by 0.225 m and the void wins.

What makes it a bite rather than a doorway is the void's SOUTHWARD reach. The file's own comment
says "0.45 southward is air under the portico" — it is not: the antae stand in that air, 0.18 m
proud of the wall, and the void runs to z = -7.95, which is 0.27 m past their front face.

**Asked for:** bring the two side voids' south face back from -7.95 to the wall face at -7.50. The
leaves stand at z -7.545 .. -7.35 and are written back into the void anyway, so nothing about the
door changes; what is left of the overlap is then 0.05 m of the anta's shoe buried inside the wall,
where it cannot be seen. I cannot make this change from portico.clip: narrowing the base would break
the order and moving the anta would move a shared datum.

---

## 4. `--part` and `origin`, which cost me the first two hours

Already raised and already fixed in `requests/origin-and-parts.md` — noted here only so the next
person reading this file knows why every probe manifest beside it carries the line

    origin 0 3.50 0

That cancels the manifest's own `origin 0 -3.50 0` so a probe's `bounds` mean what they say. It is
harmless now that `--part` is fixed, and it is still necessary in a probe that binds its own shapes,
because `bounds` written after the include are shifted by `apply_origin` exactly as the manifest's
are.

The symptom, for the record, was `--part portico_soffit` reporting **EMPTY** — which reads exactly
like a cut that removed the whole slab.

---

## 5. Not a bug, and worth the two lines

**A groove 1.15 voxels wide is a dotted line.** The ashlar joints on the pediment's tympanum were
cut at 0.036 m first, which is 1.15 voxels at metre 32 and comfortably over the half-voxel the
displacement checker calls usable. Measured against the finished building it removed 570 voxels
where the arithmetic says about 1300: a voxel centre has to fall INSIDE the groove to be taken, and
at 1.15 voxels' width the phase decides whether one does, so the joint came out as a row of dashes.
At 0.045 (M/10, 1.44 voxels) a centre always falls in and the line is continuous.

Half a voxel is the threshold for a DISPLACEMENT, which moves a surface that is already there. For a
CUT that has to read as a line, the number is one and a half.

---

## 6. `entablature_relief` is reported as MATCHING NO VOXEL, and it is not a fault

The fixed paint diagnostic names `paint bronze where=entablature_relief` among the rules that were
asked and matched nothing. It is the right answer to the question the diagnostic asked, and the
question depends on `--metre`:

    metre 32   bronze 3638 voxels in the frieze band   (the contract's own resolution)
    metre 16   bronze  530 voxels
    metre  8   nothing, and the tool says why itself

The swags' tube is 0.045 m across with its axis lying IN the frieze face, so 0.0225 m stands proud —
0.72 of a voxel at metre 32 and 0.18 of one at metre 8. At metre 8 the relief is not matter, so a
rule keyed to it matches nothing, and painting it anyway would be colouring stone that has no swag
on it. clipcheck says exactly this in its own note under the list; it is worth reading before
anybody "fixes" the rule.

**Not changed, deliberately.** The header of entablature.clip says DO NOT DEEPEN THE RELIEF and
gives the reason: 0.0225 is M/20 and the part exists to ask whether sub-voxel relief with a material
edge survives the pipeline. Raising `below=` from 0.02 until the rule matched at metre 8 would paint
a bronze band three times the width of the swag at metre 32, which is the resolution that ships.

    ./build/clipcheck clips/facility/requests/entablature-relief-probe.clip --metre 32

---

## 7. `void_library` takes a skin off the entablature's inner face — for library.clip

    clipcheck requests/entablature-probe.clip --metre 16 --cuts
    void_library   built   0.186 m3   0.186 only-it   entablature_ring 0.19

One voxel thick, at x -14.31 .. -7.31, y 10.38 .. 11.19, z -6.69 .. -6.63. The entablature ring's
inner face is at z = -6.60 because that is where walls.clip puts the wall's inner face and this ring
is the top 2.00 m of that wall; the library's void reaches 0.06 m past it, so it takes a sliver out
of the wall head inside the room.

It is small and it is inside a room, so it is a note rather than an alarm. I have not moved the
entablature's core to meet it: +-6.60 is the wall's number, not mine, and thickening the ring inward
would push stone into every room at that level to fix one.
