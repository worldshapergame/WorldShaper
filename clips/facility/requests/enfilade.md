# enfilade — what I needed from other files and could not do from mine

Four things. Two of them are holes in the building that this part was built to close and could only
close half of; one is a piece of the brief that the fabric makes impossible; one is a request of the
language. Everything here was measured with `clips/facility/requests/enfilade-probe.clip` or read
out of the file it names, not assumed.

---

## 1. The piano nobile is still not continuous, by 1.415 m

**What is missing:** a floor at y = 6.30 over `x 14.05 .. 15.10` by `z 3.10 .. 4.515`, mirrored to
the west. It is 1.415 m long and 1.05 m wide.

**Why it matters:** the whole point of the upper passage is that a person can leave the ballroom by
its north door at `x 13.95..14.85` and walk to the grand stair. They now can — as far as the north
end of my gallery at `z = 3.10`. `stair.clip`'s gallery is `box 11.635 5.910 4.515  15.235 6.285
6.735`; its floor therefore begins at `z = 4.515`. Between 3.10 and 4.515 there is the open well
over the lower flight and a drop of 4.5 m.

**Why I did not build it.** Two reasons, and the second is the one that decides it.

- It is 1.415 m outside the zone this part was given, in the middle of `stair.clip`'s own declared
  volume (`z 3.15 .. 6.75`).
- `stair.clip` carries a balustrade along the gallery's south edge at `z = 4.59`, and its own header
  says so: *"the open edges are z = 4.50 from x 11.62 to 15.10 ... both carry a balustrade now
  because a landing over a 4.5 m drop has to; take either of them out when there is a floor on the
  other side of it."* A bridge built blind would arrive at a railing. Removing that railing is a
  line in `stair.clip`.

**What it needs:** whoever builds the upper east hall — `ballroom.clip`'s header names them as the
owner of *"the floor from z = -3.00 to the gallery"* — lays the slab from `z = -3.00` to `4.565`
across the whole wing, and `stair.clip` drops the balustrade between `x = 11.62` and `15.10`. My
north doorway is already open for the full clear section, head at 4.50 below and 8.70 above, and it
laps 0.05 into the stair's own volume at `z = 3.15`, so it will butt whatever arrives.

---

## 2. The cabinet's south doorway is prepared and not open

**What is missing:** 0.36 m of `salon.clip`'s north wall, and the same in `chapel.clip`, at
`x 14.05 .. 14.85` (mirrored `-14.85 .. -14.05`), `y 1.80 .. 4.20`.

`salon.clip`'s north wall runs `z -3.36 .. -2.95` and `chapel.clip`'s north face is `-2.95`. Neither
cuts a north doorway. My air laps the 0.05 the brief allows — to `z = -3.00` — and stops.

**Why I did not cut further.** The salon's north-east corner is a pier at `x 14.175 .. 14.76`,
`z -3.54 .. -3.36`, and its file explains at length that the pier exists to plug the hole
`windows.clip`'s `z = -2.70` reveal saws through that wall. A doorway of mine cut on the ballroom's
line at `x 14.05..14.85` would take most of that pier with it and reopen the fault the salon fixed.
Where the door should go is the salon's decision, not mine.

**Consequence, and it is survivable:** the porcelain cabinet is entered from its north end only,
which opens on the 22 m² of floor under the stair at 1.80 and runs west to the rotunda. It is a
cabinet with one door, which is what a *Porzellankabinett* usually is.

---

## 3. The gallery vault has no lunettes, and cannot have

The brief asked for *"a barrel vault of plaster springing at 8.10 with lunettes over each window."*
Both halves of that are impossible in this strip and the reason is one box.

`windows.clip`'s piano-nobile opening is `windows_pn_cut = box -0.675 6.30 -0.18  0.675 8.55 1.80`,
placed on the east wall at `z = 0, ±2.70`. Turned into place that is, in world coordinates, a
rectangle **1.35 m wide, from the floor at 6.30 to 8.55, running from x = 16.18 back to x = 14.20**
— 0.75 m inside a room whose east face is at 14.95. `void_windows` is subtracted from the whole
building, so every solid inside it is deleted after assembly, silently.

A lunette is, by definition, the stone between a window head and a vault. All of it is inside that
box. Sprung at 8.10 the barrel crowns at 8.55 and its haunches stand between the soffit and that
plane; `--part enf_win_u_check` measured **4171 voxels** of them inside the cut — the west haunch of
all three window bays, planed flat at 8.55.

Springing at 8.10 is also 1.80 m over the floor, which is head height at the wall face; the clear
height reached 2.10 m only within 0.335 m of the centre line.

**What I built instead:** the barrel springs at **8.55**, the window head line, and crowns at 9.00
(6.30 + 6 M). The whole vault is above the cut, so the manifest takes none of it; the clear height
is 2.25 at both wall faces and 2.70 at the crown; and the 2.40 m doorway the ballroom and the
library cut at 8.70 passes under a soffit that is 8.82 at its west jamb. `halls.clip` raised its own
springing by one module for exactly this reason and recorded what it cost.

**Nothing is asked of anybody here.** It is written down so the next person does not try it again.

---

## 4. A moulding section drawn across a large projection comes apart, and nothing reports it

Not a fault in another fragment — a property of the moulding primitives that cost an hour and that
the next person will hit.

`ovolo`, `cavetto`, `cyma` and the rest take two corners, *"the first in the stone, the second in
the air"*. If the two corners are far apart across the projection relative to the height of the
band, the profile ends in a tip that is thinner than a voxel, and the sampler leaves it as a
**separate component one voxel square and as long as the run**. Measured here:

```
let enf_g_ovolo_w = ovolo 14.005 2.565 -2.85   14.14 2.655 2.95     # 0.135 out over 0.09 up
--part enf_g_dado   ->   4 components, two of them 187 voxels floating
```

Two things that do **not** fix it, both tried:

- a core box behind the moulding, which `salon.clip` recommends. The sliver is at the outer arris,
  in front of the core, so the core never reaches it.
- `intersection { <the moulding> <a box it should already fit inside> }`. The clip works — shrink
  the box and the extent shrinks with it — but it does not remove the stray, which sits inside the
  box and is genuinely part of the profile.

What fixes it is **drawing the moulding between the two faces it actually joins** rather than from
inside the wall to the full projection:

```
let enf_g_ovolo_w = ovolo 14.095 2.565 -2.85   14.14 2.655 2.95     # 0.045 out over 0.09 up
--part enf_g_ovolo_w   ->   1 component, 0 floating
```

`halls.clip` does it this way already — its dado ovolo runs from the body face to the cap face — and
that is why its dado is clean. It is worth a line in BRIEF.md beside the `below=0.035` note: **a
moulding's two corners are the two faces it joins, and a section whose run is more than about 1.5
times its rise will not survive metre 32.** Every curved member in `enfilade.clip` is now drawn that
way and the part reports one component a side with nothing floating.

---

## 5. Small: `--gap` cannot measure a two-storey part

`--gap y@x,z` reports one run of air along the column and flags `BROKEN` if there is more than one.
Every column of this part has two floors in it, so all of them come back
`0.000 m of air along y (BROKEN)`, which is true and useless. The head heights here are measured
instead by the `stair.clip` method — a probe volume 2.10 m over each floor, intersected with the
part, which must measure empty — and those bindings are `enf_head_g` and `enf_head_u` in
`requests/enfilade-probe.clip`. A `--gap` that took a y range, or reported the largest run rather
than refusing, would replace four bindings with one flag.
