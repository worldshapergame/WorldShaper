# salon — what the language could not do, and what the building could not do

## What I could not say in the language

**There is no way to write a shape once and call it with arguments.** The chandelier has four
hanging tiers that differ only in radius, height and how many copies go round; they are written out
four times, and the four `around` blocks are 40 lines that should have been four. The mirror panel
and the sconce got round this by being drawn at the origin and placed with `translate`, which works
only because their differences are position and nothing else. The moment a repeated thing needs to
differ in a NUMBER rather than in a place, the file has to say it again in full. A `let` that took
parameters would have taken 120 lines out of this fragment alone.

**`mirror` folds `|x|` and there is nothing that says so at the point of failure.** BRIEF.md is
clear — "a shape built on the +x side appears on both" — and it is still the single most expensive
mistake in this file. Written with the sconce's branch at `x = -0.18`, `mirror { branch } axis=x`
asks the branch for its distance at `+0.18`, where the branch is not, and returns EMPTY. No error,
no warning, no dropped-feature line in `clipcheck`. Six sconces measured 146 voxels and the report
would have said "sconces on the piers"; what was actually there was six backplates, no arms, no
tapers and no flames — the same picture the manifest's own note records from playing as *"i cant
tell what they are"*, arrived at from a completely different direction. The console legs had it too.

The cheapest fix in the tool, if anybody wants it: `mirror` knows its child's bounding box, and a
child whose box lies entirely on the minus side of the fold axis is always a mistake. One warning —
`mirror axis=x: this child is entirely at negative x and will fold to nothing` — would have cost
me nothing and saved an hour.

**A paint rule that fires but paints nothing looks like success.** `clipcheck` reports `never
fired`, and that is the right check, but "fired" is not "painted": the four rules that painted the
sconce arms, tapers and flames were evaluated and matched nothing, and the run was silent. The
number that would have caught it is a per-rule voxel count, which `materials` already computes for
the whole clip and could report per rule.

**`repeat` is anchored on the world origin.** Every use of it in this file is
`translate { repeat { seed } ... } 10.80 0 0`, because the room's grid runs 6.75 .. 14.85 and the
origin is in the rotunda. That is fine once you know it; it is the third thing everybody works out
for themselves. `repeat { a } x=1.35 nx=3 at=10.80` would say it in the place it is meant.

**`bounds` in a probe has to come after the includes, and nothing says so.** Every fragment
includes `_contract.clip` — that is the whole point of the contract — and each include re-runs its
`bounds -17 -0.9 -16.5  17 20 8.5`. So a probe that sets its own box above `include "../salon.clip"`
is silently undone and the report says 34 x 21 x 25 as though nothing had been asked for. It matters
because the contract's box at metre 32 is 582 million cells and 2.8 GB, which on a shared machine is
the difference between a 10-second measurement and one that gets OOM-killed three times running. The
same rule presumably bites `metre`, `variation` and `origin`.

**Bounding boxes round every rotation, by hand.** `rotate`, `around` and anything containing them
report infinite bounds, so this file has eight `intersection { ... box ... }` wrappers whose only
job is to give the sampler back a box it could have computed itself from the child's box and the
transform. windows.clip measured the cost of forgetting one at 705 million field evaluations. A
rotation of a box IS boundable — conservatively, by the box of the rotated corners — and the reason
given in field.cpp for not doing it is that a box that is wrong by a little loses voxels. A box that
is deliberately grown by the child's half-diagonal cannot be wrong in that direction.

## What the BUILDING could not do, and it is not mine to fix

**windows.clip's east-front window at z = -2.70 opens into nothing, and its reveal saws through my
north wall.** The east front carries five ground-storey windows, at z = 0 and +-2.70 and +-5.40. The
one at -5.40 is this room's and the room is built round it. The one at -2.70 is 0.36 m north of this
room — but a window reveal in this building is 1.80 m deep, from x 16.18 back to x 14.20, and that
is straight through the east end of whatever wall is at that latitude. Measured on the assembled
building (podium + walls + windows + doors + halls + salon) at metre 8: a hole 0.56 m wide and
1.80 m tall through the salon's north wall, from the room's north-east corner into the pocket
behind, with a glazed sash 1.5 m away at the end of it.

I closed it from my side, with a pier 0.18 deep standing in front of the cut at z -3.54 .. -3.36 —
the cut stops at -3.375 and cannot reach past it — and gave the north-west corner the matching pier
so the wall reads as pier / five panels / pier. **But the pocket itself is still open**: the strip
x 13.95 .. 15.10, z -2.95 .. +2.95 is inside the block and nothing builds it, so the windows at
z = 0 and z = -2.70 look into unbuilt interior. That is a plan question, not a room question, and it
belongs to whoever owns the fabric between this room and the east hall.

**halls.clip stands 0.025 m short of my north wall.** My brief gives the east hall as
x 6.00 .. 14.00, z -2.875 .. 2.875, and `halls_block` as it stands today is x 4.95 .. 13.95,
z -2.925 .. 2.925 — so halls is being rebuilt by somebody else while I write this. My zone stops at
z = -3.00 and the brief allows 0.05 of lap, so my north wall runs to z = -2.95. Today's hall block
starts at -2.925. **There is a 0.025 m slot between them**, running the length of the wall, buried
between two solid walls where no light can reach it and nothing can fall through it. If the rebuilt
hall's south face lands on -3.00 or south of it — which is what my brief describes — the slot closes
with 0.05 of lap and there is nothing to do. If it lands north of -2.95, somebody has to move one
number, and it should be the hall's, because my zone forbids me from reaching further.

## The fault that no number in `clipcheck` reports

**The trim ran straight across the doorway, and every measurement said the room was fine.** The
shell is cut by the room's own hollow where it is built; the skirting, dado cap and cornice are
unioned on afterwards and they do not stop at the wall face. So this file put **822 voxels of
skirting, dado cap and cornice across its own door** — three bars at ankle, waist and head height in
an opening 0.90 wide — and the component count was 1, the volume was plausible, the material list
was complete, the `never fired` list was empty and the plan slice at 0.125 m looked right. It is the
same fault halls.clip's header spends five paragraphs on, found the same way it was: by asking
`intersection { part_salon <the opening> }` and measuring THAT. The cornice was doing the same thing
to the top of the round-headed window at x = 8.10, taking a lens 0.48 wide and 0.045 deep out of the
arch, because the cornice foot is at 4.68 and the arch crowns at 4.725.

**That intersection is the check, and it should be a flag on the tool.** `clipcheck <file> --clear
<x0,y0,z0,x1,y1,z1>` — "report any matter inside this box" — would have caught it in one run, and it
is three lines of code next to the code that already computes an extent. Every room in this building
has doors and windows in it and every one of them can make this mistake in silence.

## What I would not do differently, and why, in case somebody is tempted

- **The pier glasses are 0.90 wide with 0.45 of pier between them, and not one long mirror.** Two
  unbroken mirror walls facing each other is a degenerate corridor: the recursion carries a flat
  colour and tells you nothing about the bounce budget. Broken into panels, every reflected image is
  interrupted by real gilt geometry at every bounce.
- **The chandelier's drops are stacks of overlapping octagonal prisms, not beads threaded on wires.**
  A wire thin enough to look like a wire is under a voxel, so at metre 32 the drops let go of it and
  the part reports four hundred floating components. Overlapping prisms are one continuous solid
  from the ring to the pendeloque and still read as cut glass because every other one is turned by
  half a facet.
- **The drops are 0.052 to 0.083 m across and stay that way.** They fragment below metre 32 — see
  below — and making them survive metre 16 would mean making them 0.09 minimum, which deletes the
  test the brief asked for.

## Where this fragment is honest about its resolution

`components 1` and no floating voxels at **metre 32**, which is the contract's own metre and the one
the building is sampled at. Below that it comes apart, in the places the load note says it will:

    metre 32    1 component,   0 floating
    metre 16    27 components, 43 floating   (0.018% of 239,074) — crystal pendeloques at 1.7
                                              voxels and sconce branches at 1.4
    metre 8     10 components, 16 floating   (0.06% of 28,514)  — the same, plus the chandelier
                                              candles

Everything on that list is deliberately between one and two voxels at metre 32 and therefore under
one voxel at metre 16. The alternative is a room whose thin matter is three voxels thick, which is a
room that asks the sampler nothing.
