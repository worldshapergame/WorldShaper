# What the vestibule needed and could not have

Four things, in the order they cost me time.

## 1. The order has no reduced order, and nothing interior can use the great one

`_order.clip` gives one column, 9 D = 8.10 m tall. There is nowhere inside this building to stand
one: the tallest interior the plan allows is 4.95 m of clear height, because the great door's head
is at 6.30 and the podium floor is at 1.80. Every interior fragment that wants a column — the
vestibule screen, the rotunda's colonnade if it gets one, a stair — has to reduce it, and the only
tool for that is `scale`.

Halving it works and it looks right (`renders/vest-col2/contact-sheet.png`, eight sides at metre
32: Attic base, entasis, 24 flutes and a legible capital at 4.05 m tall). But it halves the thin
features too, and those were already at the limit on purpose:

| feature | great order | at half |
|---|---|---|
| flute fillet | 0.030 (1 voxel) | 0.015 |
| volute tube | 0.045 (1.4 voxels) | 0.0225 |
| volute face plate | 0.045 | 0.0225 |
| egg-and-dart dart | 0.026 | 0.013 |

At metre 32 the volute is under a voxel and at some phases of the grid it sheds a voxel off the
outside of the scroll: `part_vestibule` reported **3 components, 2 voxels not joined**, one per
column, and the single column at the origin was fine while the same column translated to
(1.35, 1.80, -5.175) was not. I fixed it by growing **the capital only** by 0.006 — a fifth of a
voxel, less than the manifest's own 12 mm of grain — which knits the scroll without moving a
surface anyone can measure. It is in the file as `vestibule_col_knit` with the reasoning beside it.

**What would be better:** an `ionic_column_minor` in `_order.clip`, 4.05 m tall and 0.45 at the
foot, drawn as a small column rather than as a scale model of a big one — which is what a real
reduced order is. Fewer flutes (16, not 24) so the fillet stays 0.030; a volute whose tube is
0.030 rather than 0.0225; no egg-and-dart, or a coarser one. Every thin feature then stays at one
voxel, where the order says it wants them, instead of at half a voxel where it only makes noise.
There is a second caller for it already: whoever colonnades the rotunda.

**Also worth knowing:** `scale` deliberately carries no bounding box, so a scaled subtree cannot be
culled and every voxel of the caller's bounds walks the whole column. Wrapping it in
`intersection { scale { ionic_column } ...  box ... }` puts a sound box back — 4.7 M field
evaluations became 0.9 M for the two columns. That is worth a line in BRIEF.md next to `scale`.

## 2. A moulding stands on a core at its FIRST corner's face — and a backing at its second buries it

`build_moulding` clusters the material around the first corner: the solid's flat side is at `p0`
and its face runs out to `p1`. So the core behind a moulding has to stand at **p0**, and a backing
box drawn at **p1** does not back it, it swallows it whole. I lost the screen entablature's bed
mould that way and did not see it until I read the component count.

The same rule the other way round is what makes a moulding float: draw the body of a dado up to the
foot of its cap and the cap is a ribbon 0.045 off the wall, attached to nothing. Both of my dados
did that. `_order.clip` says it once, for the scotia only ("the core the moulding is cut into has
to stand at least there"); it is true of every one of them and BRIEF.md's moulding paragraph does
not say it. Two sentences there would have saved an hour.

## 3. `repeat`'s keys have to be on the line the brace closes on

This parses as nonsense:

```
let a = mirror {
    translate { repeat { thing }
    x=1.50 nx=1 } 0.75 0 0
} axis=x
```

and reports `unknown shape or pattern 'x'` on the line the key is on, then cascades through the
next four hundred lines of the manifest and into every fragment after it — about 120 errors, none
of which name the real problem. Keys after a `{ }` block seem to have to sit on the same line as
the closing brace. Either the parser should accept them on the next line, or the error should say
"a key must follow its block on the same line".

## 4. `tools/views.ps1` aborts on a WARN and throws away the rest of the run

`views.ps1` sets `$ErrorActionPreference = "Stop"`, and PowerShell turns anything a native
executable writes to stderr into an error record. `WorldShaper.exe` writes
`[WARN ] frame  frame 1 took 233 ms` to stderr on a slow path-traced frame, so the script dies
mid-sheet: I got four of six views and no contact sheet, twice, after waiting for a path trace of
the whole building. Wrapping the `& $exe @shot` call in `$ErrorActionPreference = "Continue"` (or
piping through `Out-String -Stream` inside the script) would fix it.

## Not a request, just a note for whoever builds the rotunda

The vestibule's screen stands on z = -5.175 and **its northernmost stone is at z = -4.95**, which
is r = 4.95 at the centre line — 0.15 clear of the contract's 4.80. Nothing of mine is inside your
circle, so cut it as wide as you like. The north face of my screen entablature (y 5.85 .. 6.75) and
of my ceiling slab (6.75 .. 7.20) are plain planes at z = -4.95: they are your elevation, not mine,
and I left them plain rather than run mouldings out that your own circle would have cut in half.
`void_vestibule` is the room's air less my own stone, so if you fill this ground with wall it will
be carved back out again and your wall will not eat my columns.
