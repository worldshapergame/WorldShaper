# rotunda — what I needed and could not say, and two things worth measuring again

Written against `clips/facility/rotunda.clip`. Everything here was worked around; nothing is
blocking. §1 is the one real language request. §2 is a bug report I nearly filed and should not
have, kept because the way it was wrong is the useful part. §3 is a line the brief should carry,
§4 is a tool that breaks under load, and §5 is something in a neighbour's file that somebody
should look at.

---

## 1. A circular repeat that does not cost the whole clip its sampler

This is the same request `_order.clip` and `drum.clip` and `dome.clip` have all now made, and it
is the single largest thing missing from the language. `around { } count=n` sets
`metric_slack(Op::PolarRepeat)` to `kInfiniteSlack`; the sampler takes the minimum slack over the
whole expression; so one `around` anywhere means not a single voxel of the clip's bounds can be
skipped anywhere. The workaround — write the quadrant as explicit `rotate` nodes and fold it twice
with `mirror` — is correct, exact and about a thousand times faster, and this file uses it five
times (the coffer ribs, the columns, the niches, the bays, the floor's radiating bands).

**It only works for counts that are a multiple of four.** That is the limitation, and it is the
one that bit this part. The interior entablature is `entab_profile` revolved, and an entablature
of this order has a dentil band: at the order's own 0.135 pitch, on the 28.3 m circumference of a
cornice at radius 4.51, that is **210 dentils**. 210 is not a multiple of four, 210 explicit
`rotate` bindings is not a thing anybody should write, and `around { } count=210` would take the
skip away from the podium and the walls and the steps and everything else in the building. So the
interior cornice has a plain uncarved band where its dentils belong, and it is the one place in
this room where the order is not fully expressed.

What would fix it, in rough order of how much I think it is worth:

- **`around` with a true slack.** The fold is only lossy near a sector boundary, and the amount it
  can lie by is bounded: for a shape lying wholly within radius R of the axis, the fold's error is
  at most the chord between adjacent sectors, `2 R sin(pi/count)`. If `Op::PolarRepeat` reported
  that instead of infinity, `around { } count=210` on a 0.09 dentil at radius 4.51 would report a
  slack of 0.134 — small, sound, and enough to let the sampler skip almost everything. The child's
  own bounds already give R.
- **`around { } count=n arc=0.25` folding into a quadrant**, so the caller can say
  "twenty-four of these, and I promise they are symmetric in x and z" and get the fold done for
  them without the mirror dance. This would remove about forty lines from this file and the same
  again from `drum.clip` and `dome.clip`, and — more to the point — it would remove the trap
  described in the next paragraph.
- Failing both: a `repeat { } around=y count=n` that expands to explicit rotations at parse time.
  Ugly, but it is exactly what three files are now doing by hand.

While that is open, the sign trap deserves a note in `BRIEF.md` rather than in three fragment
headers: **`rotate { } y=+turns` carries +x toward −z, and `mirror` keeps only what is on the
positive side of the fold, so every member of a quadrant that is to be folded in x and then z must
be written with a NEGATIVE angle.** Written the natural way it does not error, does not warn and
does not halve — it deletes. `drum.clip` lost sixteen windows to it, `dome.clip` lost eighteen
ribs, and `_order.clip`'s flutes and egg-and-dart are, as far as I can measure, still written the
other way round.

## 2. A bug report I was about to file against `offset`, and why it is not one

I am leaving this in because the mistake is the useful part, and because the number I nearly put
in front of the pipeline team was wrong by a factor of fifteen.

Two paint rules in this file need to name a surface that a **subtraction** made — the lining of a
niche, the inside of a coffer (see §3 for why). The natural way to write that is to grow the
cutter, `offset { rotunda_coffers } by=-0.045`. I made that change, re-measured `--clip-part
part_rotunda` at metre 8, and the sample time went from **179 ms to 2682 ms for the same 1.72
million field evaluations**. That is a very convincing shape for a bug: identical work, fifteen
times the wall clock, so the sampler must have stopped settling blocks — and `Op::Rotate` is
already known to report no bounding box, so `Op::Offset` doing the same was an easy story to
believe. Boxing the offset "fixed" it to 1018 ms, which fitted the story even better.

**None of it was real.** Five other agents were building this facility at the same time; the
machine sat at 100% for most of the session; and I had compared two runs taken twenty minutes
apart. Measured the only way a shared machine allows — the three variants written alternately in
one run, four rounds each, medians reported:

| the two paint zones written as | median sample | runs | field evaluations |
|---|---|---|---|
| shapes drawn from scratch (shipped) | **749 ms** | 708 756 743 755 | 1,728,878 |
| `offset { void } by=-…` | **784 ms** | 919 766 746 802 | 1,725,873 |
| `intersection { offset { void } box }` | **751 ms** | 751 672 751 776 | 1,725,873 |

`offset` of a bounded shape costs nothing measurable. There is no bug. The script that produced
the table is four lines of Python around `subprocess` and it should probably live in `tools/`.

Two things worth taking from it anyway:

- **A timing quoted from this repository is worthless unless the variants were interleaved.** The
  spread within a single variant here is 919 vs 708 ms — thirty per cent — and that is *inside*
  one interleaved run. Sequential A-then-B on this machine can manufacture any factor you like.
  The field-evaluation count, by contrast, was stable to a tenth of a per cent across everything I
  did all session; when there is a real sampler regression to find, **that** is the number that
  will show it, and it is the one worth putting in a fragment header.
- The zones did stay drawn from scratch, because two ellipsoids really are fewer nodes than
  thirteen banded shells and a twenty-four-fold rib fan. That is a legible-source argument, not a
  performance one, and the comment in the clip now says so.

## 3. `below=0.02` is right for a solid and wrong for a hole, and the brief should say which

The brief's rule 5 — *paint only where your own shapes are, and always `below=0.02`* — is correct
and I have followed it. But it silently assumes the shape you name is the one that **remains**.

A surface made by a subtraction lies OUTSIDE the thing that subtracted it. Every voxel of a
niche's lining is a voxel the cutter did not contain: it is the first one the cutter missed, so
its centre stands up to half a voxel outside — 0.0156 m at the contract's metre 32 and **0.042 m
at the metre 12 everybody previews at**. `below=0.02` catches the first and misses the second. The
symptom is not subtle and it is not obviously a paint bug: the niches came back grey stone with a
green ring round the mouth, which is precisely the half of the lining that happened to fall inside
the cutter, and it looked like a modelling mistake for about twenty minutes.

Suggested wording for `BRIEF.md`, under rule 5:

> A paint rule keyed on a shape you SUBTRACTED will not reach the surface that subtraction made,
> because that surface is outside it. Key it on a shape that CONTAINS the stone you want painted —
> the cutter grown by a module fraction, or a band drawn for the purpose — and let a later rule
> paint back whatever the growth overreached onto.

## 4. `tools/views.ps1` dies on a stderr line, at random

Under Windows PowerShell 5.1, `$log = & $exe @shot 2>&1 | Out-String` at views.ps1:249 turns any
line the engine writes to stderr into a `NativeCommandError`, which terminates the script. The
engine writes `[WARN ] frame  frame 1 took 227 ms` whenever a first frame is slow — which, with
five other agents building at once, is most of the time. The result is a contact sheet with two
tiles on it and no error that names the cause.

Three of my six render passes died this way and the sixth succeeded with identical arguments. It
is one line to fix: `& $exe @shot 2>&1 | Out-String -ErrorAction SilentlyContinue` does not help,
because the failure is in the redirection itself; `$log = (& $exe @shot | Out-String)` and letting
stderr through to the console does. I have not touched the file — it is not mine.

`tools/_rot_sheet.ps1` and `tools/_rot_pt.ps1` are what I used instead: nine views from the middle
of a room on the eight bearings plus one looking up, and the same four views path traced. They
are the interior equivalent of `-Views ring`, and if somebody wants to fold them into views.ps1 as
`-Views room` they are welcome to. The one thing they get right that a naive script does not:
**camera coordinates are in world metres and the clip is built at `metre / 32` of full size**, so
a camera meant to stand at clip height 3.50 has to be written at 1.3125 when `-Metre 12`. Every
camera I aimed by hand before working that out was pointing at the wrong part of the building.

## 5. Not a request — something in the dome that is worth a look

Seen from the rotunda floor, four bronze bars radiate from the oculus rim at exactly N, S, E and W
across the dome's intrados, about 3 m long, with a ring and a vertical rod hanging under the
opening. It reads as a bronze armature nobody drew.

It is not mine, and I checked rather than assumed: with `void_rotunda` bound to a 0.02 sphere —
no coffers, no oculus, nothing of mine cut out of anything — the bars, the ring and the rod are
all still there, pixel for pixel (`renders/rot3/novoid.png` against `renders/rot3/entab.png`).
They are also visible in `--clip-part part_dome` on its own (`renders/rot3/domeonly.png`). My best
guess is `dome_rim` seen with the lantern behind it plus something four-fold in the lantern, but
it is the dome author's file and their call. Four-fold symmetry aligned to the axes is the clue:
almost everything up there is eight-, sixteen- or twenty-four-fold.
