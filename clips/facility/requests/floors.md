# The white floors — what happened, how it was found, and what stops it happening again

2026-08-17. The owner, looking at the built clip: *"the halls and rotunda floor are white and dont
show their patterns"*.

They were. The rotunda's pavement — nine stones, a radiating figure on seven radii, the one large
coloured surface in the building — measured **101,605 voxels of visible face, 100.00% limestone**.
One material. The manifest's base coat. The halls' floors measured 69.06% limestone, with only the
strip beyond x = 12.00 still wearing terracotta.

## It was not any of the four things it looks like

**It was not a dead rule.** Not one floor rule was in `never asked`, `never matched` or
`too coarse`, before or after. Sampled on their own, the two rooms paint perfectly: terracotta
32,166 voxels, lapis 13,952, every rule alive, no limestone anywhere.

**It was not a cut.** The volume of the visible layer is identical before and after the fix —
101,605 in the rotunda and 33,746 in the halls — and the whole building at metre 8 measures
1,840,191 voxels and 779,152 faces on both sides. No stone moved at any point in this.

**It was not `below=` being too small for a transformed shape.** That fault is real and eleven
other rules in these two files had it (see below), but it accounts for a rind, not a floor.

**It was a later coat.** `crypt.clip` is included after `rotunda.clip` and `halls.clip`, and a
later coat covers an earlier one.

## The mechanism, in one paragraph

The building's floor is at y = 1.80. The crypt's ceiling slab runs 1.6875 to 1.80 — its own notes
say so, and say it "meets all of them": the vestibule's floor, the rotunda's, the halls', the
stair's. At the contract's metre 32 a voxel is 0.03125 m, so **the cell that carries the visible
floor face spans 1.78125 to 1.8125 and its centre is 1.796875** — inside `crypt_block`, inside
`rotunda_floor`, inside `halls_block` and inside the podium, all at once. Four fragments' paint
rules match that one cell. Paint is decided at the cell centre and the last rule wins, so the
fragment included last takes the surface. That was the crypt.

Two of its rules reached: `crypt_zone_shell` (tuff, keyed on the crypt's whole mass) and
`crypt_zone_ceiling` (limestone, a box whose top was **1.85 — fifty millimetres above the top of
the very slab it was painting the underside of**). Measured directly:
`intersection { part_rotunda crypt_zone_ceiling }` is 312,756 voxels reaching y = 1.812, and the
same intersection restricted to the single visible layer is 96,944 of the 108,241 cells in it.
Every other crypt shape — the ribs, the shafts, the stairs, the gratings, the damp and the bloom —
measured EMPTY in that layer.

The bisect that found it: build the manifest's include list one file at a time and watch the
halls' terracotta. Everything up to and including `halls.clip` gives 32,166 voxels. Adding
`stair.clip`, `salon.clip`, `chapel.clip` or `ballroom.clip` changes nothing. Adding `crypt.clip`
alone takes it to 4,152 and puts 45,595 voxels of limestone in its place.

## The fix

Two lines in `crypt.clip`, in its paint section only: a `crypt_paint_lid` box topping out at
1.74375 (1.80 − M/8), intersected with `crypt_zone_shell`, and `crypt_zone_ceiling`'s own box top
brought down from 1.85 to the same number. 1.74375 clears the crypt's web crown at 1.725 by 19 mm
so the vault it really owns is still painted, and it stands below the lowest voxel centre the floor
above can have at metre 32 (1.796875) or metre 16 (1.78125) by more than the rule's 0.02 band.

**It moves no stone.** It is a paint zone, not a shape.

## Why `clipcheck` could not have told you

Its three classes — `never asked`, `never matched`, `too coarse` — are all about whether a rule
MATCHED. Every floor rule matched. Each one then lost to a coat laid after it. There is no line in
the report for that and there cannot be one from rule bookkeeping alone: the only thing that shows
it is the finished material share on the surface a player stands on.

**And the obvious probe makes it worse.** The first version of `floors-probe.clip` included only
`rotunda.clip` and `halls.clip` — and it PASSED, on a building whose floors were white, because the
fault is not in either file. An isolating probe is a green light on a broken floor.

`clips/facility/requests/floors-probe.clip` therefore includes the whole manifest and narrows only
the box: the one voxel layer at metre 32 that carries the floor face, whole building, every void
cut and every coat laid. It takes about a minute and the pass condition is one word — **no
limestone**. Nothing in either room paints limestone, so any of it there at all arrived from
somewhere else.

## The other half: `below=` on transform-placed shapes

BRIEF.md rule 5 asks for `below=0.035`, not 0.02, on any paint rule whose shape a `translate`,
`rotate`, `around` or `repeat` put where it is, because a voxel is decided SOLID by coverage and
PAINTED by a rule tested at its centre, and for a moved shape 3 to 16 per cent of its own solid
voxels fall outside its own test.

Both files were audited mechanically — follow every `where=` shape through unions, differences,
intersections, mirrors and rounds, and see whether the chain reaches a mover. **Twelve rules
qualified and eleven were still at 0.02:**

| file | rule | what moved it |
|---|---|---|
| halls | `plaster where=halls_face` | `displace` on the room air, `repeat` on the floor joints |
| halls | `lapis where=halls_fl_loz` | `rotate` + `repeat` + `translate` — three deep |
| halls | `granite where=halls_thresh` | the worn dish is a subtraction; its lining lies outside |
| rotunda | `marble where=rotunda_cut` | `displace` on the wall, bays by `rotate`/`translate` |
| rotunda | `marble where=rotunda_entab` | the giant order, placed round the room |
| rotunda | `marble where=rotunda_columns` | the same |
| rotunda | `porphyry where=rotunda_spokes_a` | seven radii, thirteen `rotate`d boxes |
| rotunda | `verde where=rotunda_spokes_b` | the same |
| rotunda | `verde where=rotunda_niche_zone` | `rotate` of the niche cutter, folded twice |
| rotunda | `marble where=rotunda_dressings` | placed into four bays |
| rotunda | `porphyry where=rotunda_pedestals` | the same |
| rotunda | `gilt where=rotunda_urns` | the same |

The twelfth, `rotunda_thr_zone`/`rotunda_worn_zone`, was already at 0.035 with the reason written
under it — which is how the audit's criterion was checked against the file's own judgement before
it was trusted anywhere else.

`mirror` is deliberately NOT a mover. It folds x to |x|, so a face at 5.40 has a twin at −5.40
standing exactly where the unmirrored one did relative to the grid, and it cannot open the gap.
Eleven rules across the two files are mirrored and nothing else, and none of them was touched.

What it bought, on the visible layer at metre 32: the rotunda's two spoke zones went from 10.27 to
11.81 per cent (porphyry) and 2.93 to 3.48 (verde) — that is the figure's own voxels coming back —
and the halls' lozenges took lapis from 16.83 to 17.34. Small, and exactly where it was predicted.

## What this leaves for somebody else

**The rule that made this possible is still in force everywhere else in the building.** Any
fragment whose own mass reaches y = 1.80 under a room, and which is included after that room, will
take its floor. The crypt was the one that did; the podium and the site are included BEFORE the
rooms and so are harmless by luck rather than by design. Nothing in the language prevents the next
one. A `paint` that could say "only my own part's voxels" — rather than "only where my own shape
is", which is not the same thing when two fragments model the same stone — would end the class.
