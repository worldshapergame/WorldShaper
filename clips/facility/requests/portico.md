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
