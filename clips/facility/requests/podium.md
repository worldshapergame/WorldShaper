# Requests and findings from the podium (`clips/facility/podium.clip`)

## 0. The manifest's `variation` means there is not one uniform brick in the whole facility

This is the one that matters, because it is the exact thing this part was given to put under
load, and one line in `clips/facility.clip` cancels it.

`apply_variation` (`src/forge/sample.cpp:124`) iterates **every non-air voxel of the clip**,
interior included:

```cpp
const VoxelTypeId base = clip.voxels[index];
if (base == kAir) continue;
...
record.red = nudge(source.red, c, signed_slice(h, 0));
```

`hash_voxel(wx, wy, wz, seed)` is per voxel, so each one gets its own perturbed record and its own
minted type, up to `variation.budget` (default 1,000,000). Only `variation.has_by` can stop it:
where the `by` field evaluates to 0, `scale` is 0 and the record is left exactly as it was, so the
voxel keeps its base type.

`clips/facility.clip` ends with

```
variation colour=0.030 rough=0.070 seed=17
```

with no `by=`. So all 50,962,476 voxels of the podium — 1293 cubic metres of which are solid
granite nobody will ever see — are hashed apart into thousands of near-identical granites, and no
brick anywhere in the building can take `Form::Uniform`. The comment at `src/app/main.cpp:2460`
is explicit that this is the case worth budgeting for: *8 bytes for a uniform brick to 2 KB for
one where every voxel differs*, and *a large flat build is almost entirely uniform bricks*. The
facility currently proves the opposite of what that comment says it needs to prove.

Suggested fix, in the manifest, one binding and one key — variation only within 12 cm of a
surface, which is the only place it can be seen anyway:

```
let skin = smoothstep { negate { abs { hollowed } } } from=-0.12 to=-0.02
let all  = displace { hollowed grain_fine } amount=0.012
variation colour=0.030 rough=0.070 seed=17 by=skin
```

`abs` of a signed distance is distance-to-surface, so `skin` is 1 at the face and 0 deep inside;
`by` is clamped to [0,1] before use, and `variation.by` is evaluated on the *undisplaced* shape
here on purpose, so the band does not chase the grain. Nothing visible changes. What changes is
that the inside of every wall, every column and this entire podium collapses back to one type,
which is what makes a brick uniform — and it is also strictly less work for the minting pass,
which currently probes a hash table sixty million times to discover a difference no camera can
reach.

I have not touched `clips/facility.clip`; it is not mine.

## 0b. `--part part_podium` on the manifest reports materials that are not this part's

Measured, `tools/clipcheck.sh clips/facility.clip --part part_podium --metre 8`:

```
materials     63 distinct records
  64    limestone            534580    67.14%
  317   sandstone             59366     7.46%
  1122  plaster              41792     5.25%
  ...
  435   granite               6238     0.78%
```

The podium paints granite over 83% of itself and the report says 0.78%. **It is not a fault in
this file.** `apply_origin` (`src/forge/clip_script.cpp:1149`) translates `script.solid`, every
paint rule and the sampled bounds by the manifest's `origin 0 -3.50 0`, and does NOT translate
`script.parts`. So `--part` substitutes an untranslated shape into a translated world, the geometry
is sampled 3.50 m from the rules meant to paint it, and what survives is whatever other fragments'
zones happen to overlap the shifted podium — plaster, gesso, verde, boiserie, none of which this
part paints. The whole diagnosis and the one-line fix are in `requests/steps.md`, item 5.

Until it lands, measure this part through `clips/facility/requests/podium-cuts-probe.clip`, which
has no `origin` in it and reports what the file actually says:

```
materials     3 distinct records
  125   granite             4949304    77.70%
  126   limestone           1127522    17.70%
  127   marble               292602     4.59%
```

That probe also carries a control arm for each of the two grooves — `probe_pod_rust_only` and
`probe_pod_pave_only` — so the question "does this cut remove exactly what it should" is one
subtraction. The answer today is yes, at metre 16 and metre 32, exactly additive; the numbers are
in the header of `podium.clip`.

## 1. A bug in `_order.clip`'s `entab_run` — every curved member is silently a plain fillet

Not mine to fix; I do not own that file. But whoever places `entab_run` should know.

`is_moulding` in `src/forge/clip_script.cpp` reads `run=` (defaulting to **z**) and then takes
`proj = (run == 0) ? 2 : 0` and `high = (run == 1) ? 2 : 1`. With the default, a six-number
moulding is read as *section in (x, y), run along z*. Every member of `entab_run` is written
along x with no `run=` key:

```
let entab_run_ovolo = ovolo  -17 1.62 -0.34  17 1.545 -0.42
```

so it is read as a section 34 metres across (x = -17 .. 17) and 0.075 tall, swept 0.08 m along z.
The intended reading is a section 0.08 across and 0.075 tall, swept 34 m along x.

It is invisible on a contact sheet, which is the dangerous part. A moulding is its rect
intersected with an ellipse, and an ellipse 34 m wide fills essentially the whole rect except
within a few centimetres of x = ±17 — so the member still occupies its box, still joins its
neighbours, and reads as a plain square fillet. `entab_run_arch_cr`, `entab_run_bed`,
`entab_run_ovolo` and `entab_run_crown` are all affected; the `fillet` members are not, because a
fillet is its rect whichever way you read it.

`entab_profile` (the four-number form, meant for `revolve`) is correct — the default run is right
there. It is only the straight run that needs `run=x` on all four curved members.

## 2. `tools/views.ps1` dies mid-sheet on a slow frame

`views.ps1` sets `$ErrorActionPreference = "Stop"` and then calls the engine as
`& $exe @shot 2>&1 | Out-String`. In Windows PowerShell 5.1, merging a native executable's stderr
into the pipeline wraps each line in an `ErrorRecord`, and with `Stop` in force the first one
terminates the script. The engine prints `[WARN ] frame  frame 1 took 515 ms` whenever a frame is
slow — which, on a fifty-million-voxel part at metre 32, is most first frames. The result is:

```
  south              14.3,1.364,-15.261,450,-14   fills 55%
WorldShaper.exe : [WARN ] frame    frame 1 took 622 ms
At C:\Users\pc\Desktop\WorldShaper\tools\views.ps1:249 char:12
```

and no contact sheet. It is not deterministic — the same command succeeds on a warm cache and
fails on a cold one — which makes it look like the clip is at fault.

Two-character fix: `2>$null` instead of `2>&1` on that line, or set
`$ErrorActionPreference = "Continue"` just around the call. The `$log` variable it captures is
only used to fish out error lines when the PNG is missing, and stderr is already surfaced by the
harness.

Workaround: a copy of the script with `Continue` in it. Every contact sheet in this part's report
was made either that way or by calling the engine directly with `--cam`.

## 3. A clip with no `paint` statement reports `matter none` for every part

Building a small probe clip to find out which corner of a moulding holds the stone, I got

```
sampled box   192 x 192 x 192 voxels   3.000 x 3.000 x 3.000 m
matter        none
```

for every part in it, including a plain `box`. The clip parsed, the parts were all listed, the
field had 44 nodes — there was simply no `paint` line, so no rule matched and nothing became
matter. Adding `paint stone` made all of it appear.

That is defensible behaviour, but `matter none` for a clip that is entirely solid sends you
looking for a geometry mistake. A line in the report along the lines of *"no paint rule matched:
this clip declares no coat"* would have saved twenty minutes. The manifest gets this right with
its `paint limestone` base coat, so it only bites people writing scratch clips — which is
everybody, when something is wrong.

## 4. A moulding flatter than one-to-one reads as a staircase, and BRIEF.md does not say so

Worth a sentence next to the moulding list, because it is the difference between a base that
looks carved and one that looks like the stacked cylinders the brief already warns against.

A voxel grid renders any surface that moves further sideways than upward as a run of terraces.
At metre 32 a moulding 0.45 across and 0.1125 tall is seven terraces two voxels wide each, and it
reads as seven steps; the same 0.45 taken in two mouldings of 0.225 x 0.1125 is still two voxels
per step and still reads as steps. Only at one across to one up do the terraces become one voxel
each way and the eye reads a curve. I rebuilt this part's section twice before working that out,
and the fix was not a better moulding — it was a *smaller projection*, 0.225 instead of 0.45.

The corollary is the useful half: a **flat** overhang costs nothing, because a horizontal surface
voxelizes exactly. The corona here overhangs 0.1125 with a square soffit and it is the crispest
line on the part.

Suggested wording for BRIEF.md, under "Making it Ionic": *a moulding wants to be about as deep as
it is tall. One much flatter than that will voxelize into terraces and read as a little flight of
steps, however smooth the section is.*

## 5. Not a request — a measurement, for whoever tunes the sampler

`part_podium` at metre 32: 50.2 million voxels of matter, 1 component, **240 million field
evaluations** of a 582 million cell box, 9.8 s to sample. So a little under 60% of the box is
being skipped whole, on a shape that is 8.6% matter.

The interesting comparison is against the same slab with no mouldings: `podium_plinth` alone —
one `box`, same footprint, one third of the height — costs 10.5 million evaluations at metre 16,
where the whole part costs 32.7 million. Roughly 3x, for four small curved members and a second
extruded profile. Most of that is honest (three times the matter), but some of it is the `scale`
node inside `elliptic_run`: an ovolo whose radii differ is a round cylinder stretched, and
`Op::Scale` multiplies by its *smallest* factor, so an ellipse stretched 2:1 under-reports
distance by up to half everywhere near it, and the sampler steps half as far as it could. Making
every curve square in section fixed the look and, as a side effect, halved that stretch.
