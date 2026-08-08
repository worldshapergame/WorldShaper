# Requests from the pilasters (`clips/facility/pilasters.clip`)

Four things. The first two are faults already shipped in other people's files and are the reason
this is worth reading before you write another straight moulding or another paint rule.

## 1. A straight moulding run needs `run=`, and without it the section is drawn across the run

`fillet ovolo cavetto bead astragal scotia cyma cyma_reversa` with six numbers take a `run=` key
that defaults to **z**. `src/forge/clip_script.cpp`:

```cpp
const u32 run  = axis_from(keys.word("run", "z")) % 3u;
const u32 proj = (run == 0) ? 2u : 0u;
const u32 high = (run == 1) ? 2u : 1u;
...
p0 = low[proj];  q0 = low[high];  r0 = low[run];
```

So with the default, `proj` is **x**: the first and fourth numbers are the section's *projection*,
not its length. Write a 34-metre cornice along x and forget `run=x` and you have not written a
34-metre cornice — you have written **one moulding whose section is 34 metres across**, swept 0.06 m
along z. The curve then spans the length of the building instead of the depth of the moulding, so
the profile tapers from full at one end of the run to nothing at the other.

Measured, one `ovolo -2.0 0.0 0.0  2.0 0.2 -0.1` meant to run 4 m along x, half a metre sampled off
each end at metre 32:

| written | left end | right end |
|---|---|---|
| no `run=` (defaults to z) | 0.0088 m³ | 0.0031 m³ |
| `run=x` | 0.0078 m³ | 0.0078 m³ |

Nearly three to one along a run that should be constant. It is silent, it never fails to parse, and
at a glance the result looks like a plain fillet — which is exactly what makes it survive a contact
sheet.

**Two files in the building have it now.** I do not own either, so I have not touched them:

- `clips/facility/_order.clip`, `entab_run_*` — eleven members, all six numbers spanning
  x = -17 .. 17 with no `run=`. `entab_run_arch_cr`, `entab_run_bed`, `entab_run_ovolo` and
  `entab_run_crown` are the curved ones and are the ones that go wrong; the `fillet`s are boxes and
  survive by luck. Whoever builds the entablature will place this and get a stack of plain steps
  where the architrave crown, the bed mould, the ovolo and the cyma should be.
- `clips/facility/walls.clip`, `walls_sc_neck_s` and `walls_sc_slope_s` — the south string course's
  astragal and its weathering ovolo. The **east** ones a few lines below are correct, because there
  the run really is z. So the same course is right on two faces of the building and tapering on the
  other two.

What would fix it: `run=` is the only key of the eight mouldings that has a default, and it is the
one that cannot be guessed wrong safely. With six numbers the run axis is *knowable* — it is the
axis the two corners are furthest apart on, and no useful moulding is longer across its section
than along its length. Inferring it, and keeping `run=` only as an override, would have made both
of the above correct as written. Failing that, a moulding whose section is more than (say) ten times
its run is certainly a mistake and could say so.

## 2. A later fragment's paint rule silently repaints an earlier fragment's surface

`BRIEF.md` rule 5 says to paint only your own shapes, and every fragment does. It is not enough,
because two fragments can own *coplanar* geometry: the manifest stacks coats in include order, so
whichever file is included later wins the voxels where they touch.

Concretely: my plinth band stands 0.045 m proud of the wall at 5.80–5.895. The piano-nobile window
aprons and console feet in `windows.clip` stand 0.045 m proud of the same wall over the same
heights. Neither rule names anything but its own shape and both are correct by the letter of the
rule. `windows.clip` is included after `pilasters.clip`, so its `paint marble where=windows_stone`
lands on my band wherever an apron crosses it, and a granite band came back as a **dashed line** —
granite, marble, granite, marble, three times between every pair of pilasters.

It took a coat of `lapis` to see what was happening, and that is the useful part: **when a paint
rule seems not to be reaching, repaint it in something absurd and look.** A measurement will not
find this — the volume, the surface, the component count and the worldbox are all correct.

Worth a paragraph in BRIEF.md next to rule 5, because the rule as written reads as if owning your
own shapes were sufficient. The other half is: *if your surface is flush with a neighbour's, the
later file paints it.* My fix was to move the paint rather than the stone — the granite is now keyed
on the part of my geometry that stands more than 0.09 m proud, which no window comes near.

## 3. `difference { }` cannot be used to carve a paint zone

The obvious way to write "the plinths, but not the band" is
`difference { pilasters_plinths pilasters_band_out }`. It does not work, and the reason is worth
knowing because it will catch the next person too.

`Op::Difference` is `max(a, -b)`. At a point on the surface of A that lies *inside* B, that is
`max(0, negative) = 0` — the shape reports "you are on my surface" for a point it has removed. For
geometry that is harmless: the voxel is outside, the sign is right, only the magnitude is short. For
a **paint rule** it is not, because `below=0.02` is a test on exactly that magnitude, so every voxel
of the removed part still passes and gets painted. My granite bled straight back onto the band.

`intersection { }` with a shape that clears the unwanted surface by more than the rule's own
threshold works, which is what I did. But a note in BRIEF.md's `paint` section — *a paint zone
should be built by intersection, not by difference* — would save somebody the afternoon.

## 4. `tools/views.ps1` dies whenever the renderer prints a warning

`views.ps1` sets `$ErrorActionPreference = "Stop"` at the top and then runs the exe as
`$log = & $exe @shot 2>&1 | Out-String` (line 249). In Windows PowerShell, redirecting a native
command's stderr wraps each line in an ErrorRecord, and with `Stop` in force the first one is fatal.
So a single `[WARN ] frame  frame 1 took 442 ms` — which is normal at `-Metre 32` on a big focus —
kills the script part-way through a contact sheet, with a `NativeCommandError` traceback that looks
like a bug in the clip rather than in the runner.

It is not deterministic, which is the worst part: the same command works when the machine is quiet
and fails when three agents are building at once.

Workaround: copy the script, change that one line to `Continue`, and fix `$root` (which is derived
from `$PSScriptRoot`). One line in the real script would do it — `Continue` at the top, and an
explicit check of `$LASTEXITCODE` after each shot, which is what the `Stop` was presumably for.

## 5. Not a request — a collision between two briefs

The brief for this part and the brief for `portico.clip` both asked for the **antae** at the ends of
the portico wall. `portico.clip` had already stood two, on the outermost column axes at x = ±6.75,
out of the same `ionic_pilaster`. Mine would have gone at ±7.20, where the wall actually ends, and
the two shafts would have overlapped — 6.30..7.20 against 6.75..7.65. I dropped mine; the portico's
are in place and correct. Nothing to fix in the language, but the next time two briefs are written,
one element should be named in one of them.
