# `--part` and `origin` disagree, and the tool reports the disagreement as a building

Raised by the audit of the five upper parts (dome, drum, roof, parapet, pilasters). It is not a
request for a new feature; it is a request that an existing command stop giving a confident wrong
answer, because five files' worth of checking was nearly done against one.

## What happens

`clips/facility.clip` ends with

    origin 0 -3.50 0

and `apply_origin` in `src/forge/clip_script.cpp` moves three things:

    script.solid           the assembled building
    script.paint[].test    every paint rule
    script.settings.low/high   THE BOUNDS

It does not move `script.parts`, and it cannot: a part is a name bound to a node, the shift is a
`translate` wrapped round the solid, and a name that was folded into that solid long before is
still the node it always was.

`tools/clipcheck` (and `WorldShaper.exe --clip-file ... --part`) then does

    script.solid = piece;

which replaces the shifted solid with an UNSHIFTED node while leaving the shifted bounds and the
shifted paint rules exactly where `apply_origin` put them. The part is sampled 3.50 m out of its
box and painted by rules 3.50 m out of position.

## What it looks like when it bites

    bash tools/clipcheck.sh clips/facility.clip --part part_dome --metre 8

    matter extent 94 x 8 x 94 voxels   11.750 x 1.000 x 11.750 m
    materials     1 distinct records
      64    limestone             16696   100.00%
    worldbox      -5.875 15.500 -5.875   5.875 16.500 5.875  m
    components    1 (largest 16696 voxels, 0 floating)

A twelve-metre saucer four fifths of a metre tall wearing one material instead of six, with a
perfect component count. The dome stands 15.45 to 19.60 and is 85 m3 of six materials; the box
after the shift tops out at 16.50, so 1.05 m of it survived and every rule that would have painted
it was tested 3.50 m lower down.

The same arithmetic, less obviously, on the other four:

| part | true extent | what `--part` reports |
|---|---|---|
| `part_dome` | 15.44 .. 19.59, six materials | 15.50 .. 16.50, one material |
| `part_drum` | 11.25 .. 15.63, eleven materials | right extent, eleven materials — **none of them its own** |
| `part_parapet` | 11.81 .. 14.31, marble and bronze | 11.88 .. 14.38, one material, urns cut off |
| `part_roof` | 11.84 .. 12.22, lead, plaster, bronze | right extent, three materials, two of them other people's |
| `part_pilasters` | 5.81 .. 9.94, marble and granite | right extent, **eleven** materials — gravel, stucco, marble_hot |

The pilaster case is the nastiest, because nothing about it looks broken. Eleven materials on a
part that paints two is not an obviously wrong number; it is the coats of whatever the site, the
podium and the steps are painting 3.50 m below, landing on somebody else's stone.

## What was done instead

`clips/facility/requests/cuts-probe.clip` and the five `cuts-probe-*.clip` beside it: the same
fragments, included in the same order, with no `origin` statement and with the box drawn round the
part. They report the right extents, the right materials, and they run in under a second at metre 8
because the box is a fifth of the contract's.

## What is asked for

Any one of these; the first is the smallest.

1. **`--part` should undo the origin shift, or apply it.** In `tools/clipcheck.cpp`, after
   `script.part(part, piece)`, wrap the piece in the same translate `apply_origin` used:
   `script.solid = script.field.translate(piece, {dx, dy, dz});`. That needs `origin_shift` kept on
   the `Script` after it is applied — it already is — and the part then lands inside its own bounds
   with its own paint rules over it. One line, and it fixes the game's `--clip-file --part` too.

2. **Or record the parts as shifted.** `apply_origin` could walk `script.parts` and re-bind each to
   a translated node. That is more honest — the names then mean the same thing the solid does — but
   it costs a node per part and changes what a `let` means after the fact.

3. **Or refuse the combination.** If a script has a non-zero `origin_shift`, `--part` could say so
   and stop, instead of sampling. That is worth doing even if 1 is done, for the game's own path.

Until one of them exists, this is worth a line in the brief: **a `--part` run against
`clips/facility.clip` proves that a part BUILDS and nothing else.** Every extent, material, volume
and component count in the five headers I touched is quoted from the probes, not from that command,
and each of those headers says so.

## A second, smaller thing

`clipcheck --slice` picks its step from `max(m.size[da], m.size[db])`, the matter extent of the
whole clip. On a probe box drawn round one small feature that is right; on the building it means a
lantern 2.5 m across inside a 34 m clip is drawn at one character to 0.28 m, which is three
characters wide. There is no way to ask for a window. `--slice y@18.92 --window 2.5` or simply an
`--at x0,z0,x1,z1` would have saved a probe file per feature.
