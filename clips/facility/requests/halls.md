# halls — what I could not do, and four things the next person should know

`clips/facility/halls.clip`, the two side halls east and west of the rotunda. Everything below is
either a defect I found in somebody else's file, a trap in the language or the tools, or a note
for whoever builds next to me.

---

## 1. Two mouldings in the building are flat slabs, and the numbers look right

`run=` on a moulding is not cosmetic and it does not default to "whichever axis is longest".
`clip_script.cpp` reads it as:

| `run=` | across the face (`p`) | up the face (`q`) | along the run (`r`) |
|---|---|---|---|
| `z` (the default) | **x** | y | z |
| `x` | **z** | y | x |
| `y` | x | **z** | y |

So a moulding running 34 m along x written **without** `run=x` is asked for a section 34 m across
and 0.2 m tall. `build_moulding` obliges: it builds a quarter-ellipse with semi-axes 34 and 0.2,
which inside its own box is indistinguishable from a rectangle. You get a flat band. It does not
warn, it reports one component, it measures a sensible volume, and at metre 12 it photographs
exactly like the moulding you meant.

Two places have it, and I checked every moulding in every fragment to find them:

**`clips/facility/_order.clip:331-346` — the whole of `entab_run`.** All fifteen members. The fix
is to add `run=x` to each; the numbers are already in the right order for it, because they were
written as (x, y, z) corners with x as the length.

**`clips/facility/walls.clip:203-206` — the north-face string course**, `walls_sc_neck_n`,
`walls_sc_band_n`, `walls_sc_step_n`, `walls_sc_slope_n`. The *east*-face runs four lines below
are correct, which is what makes this worth reporting: the same course is built right on two
faces of the building and flat on the other two, so the mitre at every corner is between a
moulding and a slab.

Measured, so nobody has to take my word for it:

```
--clip-part walls_sc_slope_n     matter extent 32.062 x 0.250 x 0.188 m,  1.0825 m3
--clip-part vestibule_dado_s_ov  matter extent  7.250 x 0.062 x 0.062 m,  0.0283 m3   (run=x, correct)
```

The first is a 32 m quarter-round lying on its side. The second is a 7.25 m ovolo.

A one-line guard in `build_moulding` would end this: if `|p1-p0|` is more than, say, twenty times
`|q1-q0|` **and** more than the run's own length, the author has the axes crossed. I did not touch
`src/`; this is a request, not a patch.

## 2. `union { a b } smooth=0.02` does not parse, and BRIEF.md says it does

`Parser::call` reads positional numbers, then `keys_into`, then the block. Keys after the block
are only picked up again by the ONE-CHILD operations (`round`, `offset`, `translate` and the rest,
which call `keys_into` a second time). A union, difference or intersection reads its keys **before**
the brace and nowhere else. So it is

```
let webs = union smooth=0.02 { a b c }        # right
let webs = union { a b c } smooth=0.02        # wrong: "unknown statement 'smooth'", on a later line
```

`clips/facility/BRIEF.md:143` writes it the wrong way round in a comment
(`let name = union { a b c }     # smooth=0.1 rounds the joins`). Worth correcting, because the
error it produces names a line several statements further on and does not mention `union` at all.

## 3. `tools/views.ps1` throws away a whole contact sheet if the exe writes one line to stderr

`$log = & $exe @shot 2>&1 | Out-String` (line 249). Under Windows PowerShell 5.1, redirecting a
native command's stderr wraps each line in an ErrorRecord, and a `[WARN ]` line — `frame 1 took
812 ms`, or `cache could not rename ... .world.part` when two agents build at once — becomes a
terminating NativeCommandError. The script stops mid-loop, the views rendered so far are on disk
but `contact-sheet.png` is never written.

It is survivable (run it again; the world cache is warm, it gets further, and three or four goes
gets a sheet) but it cost me a good hour of "did that render or not". Wrapping the call in
`try { } catch { }`, or setting `$ErrorActionPreference = 'Continue'` around it, would fix it.

## 4. Notes for whoever builds next to the halls

**The space I left, on purpose.** Each hall is a solid block `x 4.95 .. 13.95`, `|z| <= 2.925`,
`y 1.75 .. 6.975`, with the room cut out of it. That leaves, per side and untouched by me:

- `z 2.925 .. 6.60` north and `z -6.60 .. -2.925` south — where `stair.clip` has since put itself
  (`part_stair` measures `z 3.125 .. 6.75`), with a 0.20 m gap to my wall that nothing needs.
- `x 13.95 .. 15.10` beyond the east end, 1.15 x 5.85 m, **and it has daylight**: the three
  ground-storey windows in the end wall at `z = 0, +-2.70` are cut 1.80 m in from `x = 16.00`,
  which is to `x = 14.20`. That is the reason my hall stops at 13.95 instead of the brief's 15.10
  — a hall reaching the wall has three windows in its east end and cannot be the closed room the
  load asks for. Whoever wants a lit room in the wings, that strip is it, and it is already glazed.
- everything above 6.975.

**The one thing I punch through somebody else's wall.** `void_halls` includes an arch void from
`x = 4.05` to `5.45`, `|z| <= 1.125`, `y 1.80 .. 5.625` (semicircular head, impost 4.50). 4.05 is
0.75 m inside the contract's rotunda radius of 4.80, so it clears a rotunda wall of any thickness
up to 1.35 m without this file knowing what that wall is. If the rotunda ever grows thicker than
that, the arch stops short and I have to change; that is the single assumption this file makes
about anybody.

**The impost cornice moved after fittings.clip read me.** It was 3.60 .. 3.825; it is now
4.05 .. 4.50, because at 3.60 its soffit stood 1.80 m over the floor — 0.30 under the brief's
2.10 head height, with a 0.27 projection, at exactly head height on the wall a person walks along.
Raising the springing one module fixed it. `fittings.clip:448` still says "impost cornice 3.825"
in a comment; nothing of theirs breaks — the change gave their sconces 0.34 m more clearance
rather than less — but the comment is stale. Their statue's shoulder at `y = 4.545` now passes the
east-wall corona (top 4.50, face `x = 13.23`) with **0.045 m** to spare, which is real but thin;
if that statue ever grows, check it.

**Six corbels were here and are gone.** I put a sconce corbel on each respond, at the bay ends;
`fittings.clip` had already hung its sconces at the bay centres with benches under them, which is
the better answer. The wall between the dado cap at 2.70 and the impost soffit at 4.05 is plain
and is theirs.

**Photographing `part_halls` alone shows furniture that is not there.** `fittings.clip` sinks its
bench and sconce-plate paint zones to `|z| = 2.115`, which is 0.09 m inside my wall face at 2.025.
Paint rules are evaluated whatever `--clip-part` selects, so a render of my part on its own has
bench-shaped and sconce-shaped patches of marble and bronze painted flat on my wall. It is
invisible in the assembled building, where their geometry stands in front of it, and it is not
worth changing — but it wasted twenty minutes of mine hunting for geometry I had not built, so it
is written down.

**Do not put a window in either hall.** They are closed on purpose; it is half of what the
fragment is for. Both, not one, because the symmetry rule does not have an exception in it and a
window on one side of this building and not the other is the one mistake visible from the air.
