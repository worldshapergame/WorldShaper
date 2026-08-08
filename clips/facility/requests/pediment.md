# pediment — what I needed and could not have, and two traps in the tools

Written while building `clips/facility/pediment.clip`. Nothing here blocked the part; all three have
workarounds and all three are in the file. They are here so the next person does not spend the
afternoon I spent.

## 1. There is no shear, and every raking moulding is one

A raking cornice is the horizontal cornice **sheared**, not rotated. Rotate it and the section is
measured perpendicular to the rake instead of vertically, so at the mitre against the horizontal
cornice every member lands `0.75 x (1 - cos theta)` out of place — 0.018 m at this pitch, half a
voxel at metre 32. That is exactly the size of error a contact sheet cannot show you and a path
trace can, as a lit hairline along a cornice that is otherwise in shadow all day.

The language has no shear. It does have `scale` and `rotate`, and

    rotate { scale { section } y=cos(theta) } z=-theta

**is** a vertical shear, exactly — `f((y cos t - x sin t) / cos t) = f(y - x tan t)`. So the part is
built and the mitre is exact. But it costs, because `Op::Scale` deliberately carries no bounding box
(uneven scaling can report less than its own box distance, which is unsound to cull against — the
reasoning is written into `field.cpp` and it is right). Everything inside the scale loses its box.
Here that is six mouldings and a backing solid, and the only reason it does not hurt is that the
result is immediately intersected with a cut box, which is sound and gives the whole raking cornice
its box back.

**What I would like:** `shear { a } y=x 0.2222` — fold `p.y -= slope * p.x` before asking the child.
It is one line of field code, it is the same class of thing as `twist` and `bend`, and unlike them
it has an exact bounding box (shear an AABB, take the AABB of the result) and a known Lipschitz
constant (the largest singular value of the 2x2, `(s + sqrt(s^2+4))/2`), so it would keep both the
culling and the skipping that `scale` gives up. Every pediment, every raking cornice, every sloped
string course and every ramped coping in every building after this one wants it.

Until then: scale-then-rotate, and intersect the result with a box so it gets a box.

## 2. `tools/views.ps1` dies the moment WorldShaper writes anything to stderr

Line 249:

    $log = & $exe @shot 2>&1 | Out-String

In Windows PowerShell 5.1, redirecting a **native** command's stderr inside a pipeline wraps each
line in an `ErrorRecord` and raises `NativeCommandError`, which terminates the script. WorldShaper
writes `[WARN ] frame  frame 1 took 927 ms` to stderr whenever the first frame is slow — which is
every time you build the whole clip at metre 16 or above. So `-Focus` on the finished building, the
one view that shows whether your part meets its neighbours, fails more often than it succeeds, and
it fails *after* rendering a view or two, so it looks intermittent rather than broken.

It is not affected by `$ErrorActionPreference` set on the command line; I tried.

Reproduces every time with a big enough build:

    tools\views.ps1 -Clip clips\facility.clip -Focus "-10.5,9.4,-14.0, 10.5,15.2,-9.0" `
                    -Out renders\x -Metre 12 -Views quick -PathTrace

Any of these fixes it, and I have not made the change because I do not own the file:

    $log = try { & $exe @shot 2>&1 | Out-String } catch { "$_" }     # smallest
    $log = & $exe @shot *>&1 | Out-String                            # all streams, no wrapping
    & $exe @shot 2> $errFile ; $log = Get-Content $errFile -Raw      # keep them separate

**Workaround I used:** drive the exe directly. The script prints every camera it is about to use
before it renders, so you can copy one:

    build\bin\WorldShaper.exe --clip-file clips\facility.clip --clip-metre 12 --pathtrace `
      --no-vsync --no-update-check --width 720 --height 470 `
      --screenshot-frame 40 --screenshot renders\x\elev.png --cam "0,5.20,-12.0,450,-4"

Two things worth knowing if you do that. The camera is in **built** coordinates, so at `--clip-metre
12` every world number is multiplied by `12/32` — the script prints the scaled focus box for you.
And the yaw is `atan2(dz, dx)` in degrees for the direction you are looking, which is why the
script's south view is 450 and not 90.

## 3. `-Part` and `-Focus` work together, and that is the only way to photograph a thin thing

`views.ps1`'s measuring pass builds at metre 6 to find the part, and anything under about 0.2 m
simply is not there at 0.17 m voxels — you get `the clip did not report a worldbox`. The order agent
hit this with `ionic_baluster` and wrote it up in `requests/order.md`.

What is not written down anywhere: **passing `-Focus` as well as `-Part` skips the measuring pass
entirely** and builds only your part. That is how I photographed a 0.10 m diagnostic slice of the
raking cornice at metre 32, and it is much the best way to look at any small piece — the build is
fast because it is one part, and the framing is yours. The script's own header only offers `-Part`
*or* `-Focus`, so nobody would guess.

## 4. Not a bug, for the record

I chased two things that turned out to be nothing, so nobody else chases them:

- **The stepped top of the raking cornice is voxelisation and not geometry.** A 2/9 slope at metre 32
  steps every 0.14 m, and grazing light turns each tread into a band. I proved it by probing seven
  horizontal slices of the section with `--clip-part` and reading the worldbox: crown 0.68, corona
  0.60, ovolo 0.40, dentil plate 0.22, bed mould 0.18, tympanum 0.14 — the curves are all there and
  all in the right place.
- **`--clip-part` takes any binding, not just `part_*`.** That is the cheapest measuring instrument
  in this repo: bind `intersection { my_part <a thin slab> }`, ask for it by name, and read the
  worldbox. It answers questions about a profile that no render can answer, in two seconds.
- **Camera pitches of +2 and +3 turns look broken and are not.** Three of my renders came back as
  empty sky and all three happened to have small positive pitch; a controlled test at pitch 0, 2, 3
  and 5 from the same point produced four identical frames. The empty ones were cameras placed
  outside the sampled box.
