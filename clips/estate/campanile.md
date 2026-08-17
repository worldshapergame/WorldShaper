# campanile — five things the language could not do, and what was written instead

Not a report on the building; `campanile.clip`'s own header is that. This is the short list of
places where the clip is longer or slower than it should be because the language has no word for
what was meant, written down so the next person meets them as known ground rather than as an
afternoon.

## 0. `around` DELETES a child that is not inside its own fundamental sector, and this file lost
##    the motif it exists for that way

Put first because it is not a limit of the language, it is a trap in it, and because it cost this
building four features at once without a word from anybody.

`Op::PolarRepeat` folds the ASKING POINT by angle into one fundamental sector and evaluates the
child there — precisely as `mirror` folds a coordinate and evaluates the child at `|x|`. For
`axis=y` the angle is `atan2(z, x)`, so `count=4` has its fundamental sector centred on **+X**, and
what `around` builds is four copies of *the child intersected with that sector*. A child drawn on
**+Z** — the natural place to draw a thing for the face you are looking at — has nothing inside the
sector, so `around` copies nothing, four times, and the answer is the empty set.

Four members of this file were written that way and all four were absent from every build:

| binding | what it is | `--part` at metre 32, before |
|---|---|---|
| `camp_belf_voids` | the eight biforate openings, the motif of the tower | EMPTY |
| `camp_colonnettes` | the four marble colonnettes in them | EMPTY |
| `camp_louvres` | the twenty iron louvres | EMPTY |
| `camp_clock_marks` | the twelve gilt hour marks | EMPTY |

The belfry was therefore a solid block of masonry with a sealed chamber inside it and a bell in the
dark. Nothing errored, the component count was healthy, the volume was plausible, and the one line
of `clipcheck` that would have said so — a paint rule that matches no voxel — **could not print
until the day this was found**, because the diagnostic counted evaluations rather than matches.

All four are explicit rotations now, which every other building on the estate already prefers for a
different reason (a polar fold declares its metric slack infinite and carries no bounding box).

**What would fix it in the language**: the same diagnostic `pavilion.md` asks for on `mirror` —
`build_bounds` knows each child's box, and a child whose box lies wholly outside the fundamental
sector is *always* a mistake, is cheap to detect, and would have caught all four of these before the
first sample.

## 1. There is no angular sector, and a spiral stair is nothing else

A tread on a spiral is *the matter between two radial planes*. There is no primitive for that:
`around` repeats a shape round an axis but cannot cut one, and `plane` is a half space through the
origin with no way to say which side. So every radial cut in this file is **two boxes 120 m across
with one face on the origin, one of them rotated** — six of them are bound at the top of the clip
as `camp_hs_*` and used forty times between them:

```
let camp_hs_zneg = box -60 -60 -60   60 60  0
let camp_hs_zpos = box -60 -60   0   60 60 60
intersection { camp_hs_zneg  rotate { camp_hs_zpos } y=b }     # the sector 0 .. b turns
intersection { rotate { camp_hs_zneg } y=a  rotate { camp_hs_zpos } y=b }   # a .. b
```

It is exact — a rotated box is an isometry of a box, so the distance stays true and the node keeps
an honest bounding box — but it costs two nodes and a rotation per plane, and the bounding box it
keeps is the box's, not the wedge's, so a sector 11 degrees wide is culled as though it were a
half space. A `sector cx cy cz from=0 to=0.03 axis=y` would be one node, one box, and would take a
hundred lines out of this file.

The same shape is what a splayed window opening is (four sloping planes), what a pyramid roof is
(four more), and what a pennant is (two). The pyramid and the splay are in here for the same
reason and written the same way.

## 2. `offset` cannot hollow anything that is open at one end

`offset { bell } by=0.09` shrinks a solid by 0.09 **in every direction**, so subtracting it from
the solid gives a shell that is closed everywhere — including across the mouth of a bell, where a
0.09 plate of bronze then sits, invisible from outside and audible to nobody. There is no way to
say "shrink the sides and leave the bottom", and `shell` has the same shape of problem from the
other side: it wraps the whole boundary, including the base face of the pyramid roof, which has to
be cut off again with a `difference` afterwards.

What the bell does instead is draw the bore as a **second profile** — the same circle with its
centre moved 0.09 nearer the axis, which offsets the arc by exactly 0.09 at every height, and a box
carried below the mouth so the mouth is open. That is right, and it is only available because a
bell is a surface of revolution. Anything hollow that is not would have to be drawn twice by hand.

## 3. A key may not start a line, and the grammar in BRIEF.md does not say so

Everything about the parser reads as token-based and whitespace-agnostic, and it is, except that
`keys_into` stops at any token that **starts a line** — that is how a statement knows it has ended.
So

```
let a = intersection { b
                       offset { c }
                              by=-0.05 }
```

is not an offset of −0.05. It is an offset with no `by` at all, followed by three tokens of
nonsense, and the errors it produces name a line thirty statements earlier in a different file
because the include splice has already renumbered everything. Wrapping a long expression is the
natural thing to do in a file this size and it is the one thing that silently changes meaning.

A key goes on the same line as the brace it belongs to, or the child gets its own `let`. Worth a
line in `clips/facility/BRIEF.md` beside `repeat needs the shape to fit inside one period`.

## 4. `weather` requests nest, and the cost is multiplicative rather than additive

Measured on this clip at metre 8, against a control arm with the three `weather` lines commented
out and nothing else changed:

| | CPU |
|---|---|
| no weathering | **13.8 s** |
| one request (`desert`, scoped to the whole carved masonry) | **46.1 s** |
| three requests, two of them over that same scope | **147.9 s** |

`clip_script.cpp` replaces `script.solid` with a displacement of itself for each request in turn and
then takes the *next* request's occlusion and curvature over that. Two requests over one shape is
therefore not twice one request — it is the second one's six-sample curvature multiplied by the
first one's, and `overgrown`'s mask is an **occlusion**, which is dearer again. The file's own
comment about the facility (2.4 s without the weathering fragment, 623 s with) is the same
phenomenon seen once.

Three things fix most of it, and all three are in the clip now:

- **scope to the outside only.** `camp_carved` includes the four faces of a 24 m stair well, which
  is more surface than the outside of the tower and is somewhere weather has never been.
  `camp_weather_skin` is the masonry caught in a hollow box.
- **order the requests cheap-first.** `cracks` asks the field no geometric question at all — its
  mask is `cell_edge` and `fbm` — so it costs nothing to put underneath. `overgrown` asks an
  occlusion, so it goes last and on the smallest scope, where `Op::Multiply` stops at the first
  nought factor and the whole chain is skipped.
- **do not ask twice for one thing.** Two plinth steps were two whole passes; they are one scope.

What would fix it properly is for the requests to be gathered and applied as ONE displacement of
the ORIGINAL solid rather than as a chain — every mask is already scoped and already zero where it
does not apply, so summing them is the same answer at a fraction of the cost. That is a change to
`apply_weather` and not to a clip, which is why it is written here rather than made.
