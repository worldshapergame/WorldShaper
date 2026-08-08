# dome — what I needed and could not say, and two things that are silently wrong

## 1. `offset by=` is the opposite sign to what everybody has assumed, and it has broken the order

`Op::Offset` evaluates as `eval(child, p) + a[0]`. Adding a **positive** number to a distance
pushes the surface **inward**, so `offset { s } by=0.25` makes the shape *smaller*. Measured on a
sphere of radius 1 at metre 32:

| written | matter extent | what it is |
|---|---|---|
| `offset { sphere r=1 } by=0.25` | 1.500 m | radius 0.75 — **shrunk** |
| `offset { sphere r=1 } by=-0.25` | 2.500 m | radius 1.25 — grown |

`BRIEF.md` documents it as `offset { a } by=-0.05  # shrinks or grows without rounding`, which
does not say which is which, and the obvious reading of "offset outward by 0.135" is wrong.

**This has already cost the Ionic order its flutes and its egg-and-dart.** `_order.clip` builds
both by shrinking a solid and putting the original surface back on raised fins:

```
let shaft_floor = offset { shaft_solid } by=-0.0375
let order_shaft = intersection { shaft_solid  union { shaft_floor shaft_fins } }
```

`by=-0.0375` *grows* `shaft_solid` by 0.0375, so `shaft_floor` strictly contains `shaft_solid`,
so the union contains it, so the intersection is `shaft_solid` unchanged — **a smooth shaft with
no flutes at all**. The same two lines appear for the echinus (`cap_ech_floor`, `by=-0.022`), so
there is no egg-and-dart either. Both need `by=+0.0375` and `by=+0.022`. Neither reports an error
and neither looks obviously wrong from far away, which is why it has survived.

I hit the same thing building the ribs — written the intuitive way they were carved *into* the
shell and then unioned with it, so they vanished without trace and the dome came out perfectly
smooth. It took a render to notice.

**What I would like:** either rename it (`grow=` / `shrink=`), or at minimum change the one line in
`BRIEF.md` to `offset { a } by=0.05   # POSITIVE SHRINKS: it is added to the distance`.

## 2. A positive turn about y goes toward −z, and `mirror axis=z` then deletes the shape

`Op::Rotate` about y transforms the point by `R(+θ)`, so the *shape* lands at `R(−θ)`: a shape on
+x rotated by `y=+0.125` ends up on **−z**. `mirror { } axis=z` asks its child at `|z|`, which is
never negative, so a shape that lives entirely at z<0 is asked about where it does not exist and
disappears. No error, no warning.

Measured — two fins, one at φ=0 and one rotated, folded by `mirror { mirror { } axis=x } axis=z`:

| written | components built |
|---|---|
| `rotate { fin } y=0.125` | **2** (only the φ=0 fin survives) |
| `rotate { fin } y=-0.125` | 6 (both fins, all four quadrants) |

`_order.clip` uses positive turns for every one of these:

- `shaft_fin_b` … `shaft_fin_g` (`y=+0.0416667` … `+0.25`) — so the shaft has **2 fins, not 24**
- `cap_egg_b` … `cap_egg_e` and `cap_dart_a` … `cap_dart_d` — so the echinus has **2 eggs, not 16**

All of them need their sign flipped. (`acro_leaf_b`…`acro_leaf_e` are fine: rotation about *z*
carries +x toward **+y**, the other way round from y-rotation, because the y row of the matrix is
transposed relative to the x and z rows. That is the standard right-handed convention and it is
correct — it is just not the same sign, and nothing says so.)

Note this is doubly fatal in `_order.clip`: even with the rotations fixed, item 1 means the
intersection that carves the flutes is a no-op. Both have to be corrected together, and the flute
count wants checking on a contact sheet afterwards, not assumed.

**What I would like:** one sentence in `BRIEF.md` under `rotate` — "a positive turn about y
carries +x toward −z; under a `mirror axis=z` fold, build your quadrant with *negative* y turns" —
and, if it is cheap, a warning when a child of a `mirror` has a bounding box entirely on the folded
side, because that is always a mistake and it is detectable from the bounds alone.

## 3. `tools\views.ps1` dies on a stderr warning from the renderer

The script sets `$ErrorActionPreference = "Stop"` and then pipes a native command's stderr with
`2>&1`. In Windows PowerShell 5.1 that wraps each stderr line in a `NativeCommandError`, so any
`[WARN ] frame  frame 1 took 439 ms` from `WorldShaper.exe` — which happens whenever the first
frame of a metre-32 build is slow, i.e. intermittently — aborts the whole run partway through the
contact sheet. Same arguments, same clip, works one minute and fails the next.

Fix is one line: `$ErrorActionPreference = "Continue"`, or drop the `2>&1` on line 249 since the
log is only scanned for "error" on failure anyway. I did not change it because I do not own it; I
worked from a patched copy in my scratchpad.

## 4. Things the language could not say (worked around, no action needed)

- **No `annulus` / hollow revolve.** A dome is a shell, and saying "the space between these two
  surfaces of revolution" takes two solids and a `difference`, which is fine — but it means the
  intrados is a separate binding that anybody editing the extrados has to remember to move too.
  A `shell { } thickness=` exists but takes a uniform thickness, and a masonry dome is D thick at
  the haunch and M at the crown, which is the whole point of it.
- **No way to ask for a paint rule scoped to a pattern *and* a shape.** I wanted mottled patina on
  the copper. `paint verde where=<pattern> above=0.6` would paint a third of the building, so
  instead I keyed on `displace { my_shape grain_broad } amount=0.18` with a negative `below=`,
  which works exactly right — outside my shell the distance is large and positive and no amount of
  noise brings it under the threshold — but it took a while to see that it would. Worth a line in
  the BRIEF: *to paint a noisy patch of your own surface, key on your shape displaced by a pattern,
  not on the pattern.*
- **The sun's elevation is not settable from `views.ps1`.** The shaft through the oculus only
  reaches the rotunda floor between about 74 and 79 degrees of sun elevation — that is geometry,
  not a defect — and there is no way to ask for a particular hour, so I could not photograph the
  one image this part exists to produce at the angle it is designed for. `-Sun <elevation>,<azimuth>`
  passed through to the renderer would be worth more to this building than any new solid.
