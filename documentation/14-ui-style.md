# 14 — UI Style

*The one thing carried over from the deprecated project (decision D68): its **visual language**. No code, no structure, no architecture — the look, the ink rule, and the tooltip behaviour. Everything below is a restatement of the idea in our own terms, to be implemented fresh against our renderer in Stage 15.*

---

## The three constraints, in priority order

1. **As little text as possible, and as little of everything else as possible**, while staying legible. Clutter is the default failure of a game UI; it has to be designed against, not tidied up afterwards.
2. **A child who cannot read must be able to use it.** Icons carry the meaning; words confirm it. If a panel only works once you have read it, it is wrong.
3. **Sober and colour-neutral. Not muted colour — no colour.**

These conflict less often than they look like they will: a control that works without reading is usually also the one with least on it.

---

## The ink rule (the "transparent opposing ink")

**The interface introduces no palette of its own.** Every colour on screen came from the world.

| Surface | Where its appearance comes from |
|---|---|
| Panel background | the world behind it, heavily blurred — nothing else |
| Text | the per-pixel inverse of what is behind each letter |
| Icons | the same inversion; no fill of their own |
| Borders, dividers | contrast against the blur, never a colour |

Why this rather than a theme: a UI with its own palette fights whatever the player is looking at — a sunset, a cave, snow — and the usual fix is to darken the game behind the panel until the palette wins. That trades away the thing the player came for.

### The rule, precisely

Working in **encoded (perceptual) units**, and deciding from **brightness, not per channel**:

```
here    = luma(backdrop pixel)
inverse = 1 - backdrop pixel
swing   = luma(inverse) - here
short   = max(0, FLOOR - |swing|)
push    = short * clamp(swing / FLOOR, -1, +1)
want    = clamp(here + swing + push, 0, 1)      // the brightness the ink must hit
```

`FLOOR = 0.3` in encoded units. It was chosen by measurement, not by eye: a panel over a grey sky put its labels nineteen levels from their background — fine in a screenshot, unreadable in a chair. With the floor, that becomes about seventy-five levels.

Three properties of that formula matter and each was a bug once:

- **No `if`.** A conditional on a continuous quantity is a cliff: as the backdrop drifts past the threshold, every pixel of a label jumps from "the exact opposite of what is behind it" to "flat light", one after another as the gradient crosses. The `max`/`clamp` form is continuous — swept in steps of 0.002, the largest step between neighbours is 3.4× the average, against 0.83 for the version with the `if`.
- **No direction decision.** `push` is odd in `swing`, so there is no "which way has more room" to decide, per shape or otherwise. A per-shape direction produced two distinct failures: a window drifting over the world flipping its whole ink in one frame (worst step 592× the average), and a shape being part-accent because the direction was per shape while the swing was per pixel.
- **Brightness, not per channel.** Per channel puts three changeovers at three different pixels, because a backdrop's red crosses the middle before its blue does. That is how an interface with no colour in it grows a red-and-yellow seam down the middle.

Also: `luma` is the weighted sum of the **linear** channels, expressed in encoded units — for a square-root encoding, a weighted quadratic mean. A plain weighted average of encoded channels agrees for greys and is wrong by a fifth of the promised contrast for a saturated colour.

### Where inversion is blind

Over a mid-grey backdrop an inversion has nothing to say, and a hard floor everywhere is provably impossible: if ink brightness `f` is continuous and at least 0.3 from backdrop `l` everywhere, then `f − l` never crosses zero, which demands `f ≥ 1.3` at `l = 1` and `f ≤ −0.3` at `l = 0`.

So in that one band the contrast is **spent on hue instead of on brightness**: the ink crossfades to the player's chosen accent colour (default pure green — a setting, which is the entire point of it being a colour at all). Brightness alone carries the ink everywhere outside roughly `0.42–0.58` luma — a sixth of the range, centred exactly where inversion is blind.

**Crossfade the hue, never the contrast.** Blending the finished colours instead lands, halfway across, on the average of "too little contrast" and "enough" — measurably a fifth short of legible. Take both ends to the same target brightness first, then mix.

---

## The five permitted colours

A colour that appears in exactly one place cannot be read as a theme. There are five, and no more:

1. **A destructive decision is red.**
2. **The animated logo mark** — the one *surface* allowed a hue of its own, because it is a picture, not furniture. It is drawn from a seed, so which colours it takes is a choice the seed makes out of twelve schemes, with the player's own logo colour biasing the family rather than replacing it (D463, `23-shell-and-libraries.md` §1). The dark end of every one of those schemes has a floor, and that is a legibility rule rather than a taste: the title room is nearly black, so a scheme free to take its floor to a quarter puts some of the combinations on the screen as a shape you have to look for.
3. **Node-graph wires**, coloured by what they carry. Three hues because the script language has three value types; a fourth hue would have to mean a fourth kind of value. They are the player's own ink rotated by a third of the circle each, so there is still no palette written down anywhere — an interface whose ink is grey has grey wires. Wires are **stated** (full brightness) rather than inverted, because an inverted two-pixel line over glass of its own brightness is a line that is not there.
4. **Command-line parts** — and the clip script, which is that argument at length — using the same three rotations, for the same reason: several unlike things on one line, and telling them apart is the whole task. Joining words take no hue; they are grammar, not a kind of thing. A command line sits over the *world*, so it keeps the brightness the inversion asked for, except that the swing is capped (≈0.45) and the rest spent on hue — otherwise, over a white sky, all three parts come out as three blacks and black has no hue. `ui_ink_tinted` in `shaders/ui.glsl` is that rule and `ui::tint_of` is the one place the rotation happens; the shader is HANDED the three colours rather than rotating them, because a wire drawn from host RGB beside a word coloured from a shader's own rotation is one place for the two to disagree.

   **This colour and the one above mean the same three things** (D755). Nothing here required that, and it is what turns two schemes into one: green is a shape in the editor's script and a shape on the wire out of it, blue is a value in both, red is a material in both. What is not one of the three takes no hue at all — the grammar because it is grammar, a comment because it is not the document's meaning, and a **name** because it is the one thing on the line the author chose and the ordinary inverted ink is the strongest mark this interface has.
5. **The accent in the blind band**, above.

---

## Icons, words and layout

- **The icon is the control; the label is the fallback.**
- **A drawing has no resolution** (D474). Every icon is a signed distance field in a continuous
  64-unit cell, sampled at whatever the screen has, so one source is a clean shape at sixteen device
  pixels and a detailed one at ninety-six. They were a thirteen-by-thirteen grid of booleans, which
  is a resolution and a coarse one: at the size a row sets an icon on a 1366×768 desktop a cell is
  one and a quarter pixels, so every diagonal was a staircase, every circle a lump, and there was no
  antialiasing anywhere because a boolean has no edge to soften.
- **A stroke is a letter's stroke, and it has a floor.** An icon is nominally 19 interface pixels
  against text set at two device pixels per glyph pixel, which makes a letter's stroke eight units
  of the cell; `kIconStroke` is a shade under it. An icon lighter or heavier than the word beside it
  reads as a different interface. The floor is against the *sampling*, not the cell: an icon shrunk
  to 0.55 of nominal gets a heavier drawing rather than a fainter one, which is the opposite of what
  scaling a fixed width down does.
- **Outlines for containers, fills for figures and for what is solid.** A folder, a page and a bin
  are containers and filling one closes the inside that says what it is; a person, a book, a pupil
  and a nib are masses.
- **At most four features across, because sixteen device pixels is the size that decides.** Three of
  the drawings were remade for this and not for taste: three figures came out as three dots, five
  books came out as a bar chart, and a socket a quarter of a block wide came out as a rounded square
  — which already meant something else two cells away.
- **A press is eased and overshoots** (D475). The countdown a press keeps is linear; a drawing given
  it directly snaps to its pressed pose and slides home at one speed, which reads as a slide rather
  than as an answer, because the eye reads acceleration and not position. So it leaves fast, comes
  home slowing, goes about a tenth past home and settles — and every drawing is written to take that
  overshoot as a counter-swing rather than as a shape turning inside out.
- **The whole vocabulary can be photographed.** `--icon-sheet --title-shot FILE` draws every icon at
  four sizes and across five steps of its own animation. Without it an icon is only ever on screen
  where a window happens to put it, and the size that decides is the one nothing ever looked at them
  at — the same failure this document names for the tool previews.
- When a button runs out of room, **the drawing gives up room before the word does, and never gives up all of it** — it shrinks to about 0.55 of its nominal size and stops, and only then does the word start shrinking. A button with no drawing at all is not a state the rule can reach.
- The word is drawn at the row's own text size, not a size smaller.
- **Text is centred on its capitals, not on its line box.** A line box carries descender room whether the string has descenders or not, which puts a word like `Fast` visibly high in its button with an empty band beneath. Centring each string's own ink instead would fix that word and break the column, because `Field of view` and `Haze distance (km)` would sit on different baselines. A capital is the same box for every string at a given size, so that is what gets centred — anything with a descender hangs slightly below the middle, which is what a descender is for.

---

## Windows, and the two things in them

The structure this language is applied to — the title, the docked windows, the libraries and the
editors — is `23-shell-and-libraries.md`. Three of its rules are style rather than structure and
belong here:

- **Two families, and the side is the meaning.** Parameters open on the left, libraries on the
  right. A player learns *where to look* rather than *what to read*, which is the same argument the
  icons are made of. Every window is docked, resizable and re-dockable to any edge; nothing floats,
  and nothing ever covers the thing it is about.
- **Every number is a slider, and a double-click types into it.** A typed value is not capped: the
  range is where the handle goes, not where the value can. A value that would break the game is
  refused *and says what it would have done* — a refusal that does not explain itself reads as a
  bug. Values that are not numbers are not sliders: a choice is icons, a switch is a switch.
- **An icon animates to say what it does, never to say it is being pointed at.** The duplicate icon
  splits; the delete icon opens. That is the same rule as *motion answers a press and nothing else*,
  applied to the drawing rather than to the control.

---

## The typeface (D437–D440)

**Three pixels wide, five tall for a capital, three for an x-height, one row below the baseline for
a descender.** Six rows and three columns is the whole cell, and the size is the *first* constraint
at the top of this document made arithmetic: as little of everything as possible, while staying
legible. Every pixel is a voxel somebody has to build, so a letter's size is the price of a word.

Three by five is the floor for an alphabet carrying **both cases**, and the floor is where the
shapes stop being *distinct* rather than where they get harder to read — at three by four the
capitals lose the row that holds B's, E's and S's middle bar; at two wide there is no interior left
at all, so nothing tells an `o` from an `n`. Five letters are allowed past it (`M`, `W`, `m`, `w`
are five wide, `N` is four): three strokes need two gaps between them, and a diagonal with one
column to fall through is a horizontal bar.

- **What it costs, against the five-by-seven it replaced:** a capital is 15 cells rather than 35,
  and an `o` is 9 rather than 25. A sign in the world is well under half the wall.
- **"Unmistakable" was measured, not asserted.** Every pair of the ninety-five printable characters
  was compared cell by cell; seven letters were redrawn because they came out ONE pixel apart —
  `0`/`U`, `2`/`Z`, `&`/`8`, `H`/`K`, `=`/`z`, `l`/`!` and `u`/`v`. What remains one pixel apart is
  the set where the one pixel *is* the meaning: `.` against a space, `,` against `;`, `.` against
  `:`, `O` against `0` (the barred zero — a slash needs a diagonal and there is one column to draw
  it in), and `O` against `Q`.
- **The interface and the world are lettered in the same face.** `shaders/ui.glsl` carries a packed
  copy so text can be drawn on the card, generated from the same source by `tools/font-table.ps1`
  and checked against it by a test. Two copies of a drawing is one place for them to disagree.
- The capital-centred baseline rule above applies unchanged.

### Markdown, because the alternative is a second way to say the same thing

Text in this game is written by players — on signs, in the sentence a tooltip gets, in whatever a
mod puts on a panel — and three pixels across leaves no room to say "this is a heading" by choosing
a different face. So structure is said with size, weight and marks, which is the vocabulary
markdown already carries, and what people already type when they want it.

Read: headings, bullets (nested by indent), numbered items, task boxes, quotations, fenced code,
rules, `**bold**`, `*italic*`, `` `code` ``, `~~struck~~`, `[label](target)`, and `\` to escape.
Each emphasis is a property of the glyph rather than a second typeface:

| Emphasis | How it is drawn | Why |
|---|---|---|
| bold | **in the world**, the drawing smeared one whole pixel sideways and one wider. **On a panel**, smeared *half a cell* at about seven tenths of the ink, and no wider | at three across there is no second weight to draw — but see below, because the same rule is wrong in the two places |
| italic | the rows above the x-height leaned one pixel right; on a panel that one pixel is spread continuously over the cap height rather than stepping at the x-height | one pixel is the whole lean available; two is a diagonal. A step puts a notch in the side of every `l`, and a panel has the resolution to avoid it |
| underline | a row *below* the descender | so it never touches a `p` |
| strike | along the middle of the x-height | where the hyphen already is |
| code | set monospaced | a box around a word costs more pixels than the word |
| heading | `#` ×3 the size, `##` ×2, `###` and deeper bold | a heading twice the size does not also need to be heavier |

**Why bold is two rules and not one** (D457). *Smeared one pixel sideways, one wider* is right about the
world and wrong about a panel, and it took looking at it to see which. At three columns across, a
counter is exactly one cell — so smearing a whole cell fills the middle of `o`, `e`, `a`, `g` and
`y`, and a bold word comes out as a row of blocks rather than as a heavier word. It was legible in
this document and unreadable on the screen.

On a panel a glyph pixel is *several* device pixels — four, at the size a row is set at — so there
is a fraction of a cell to smear into, and half of one is what bold takes. The stroke gains half its
own width and the counter keeps half of itself, so it is still a hole. The added half is drawn at
about seven tenths of the ink rather than at full strength, so what it adds reads as weight on the
stroke rather than as a second column of the letter: **a bold letter has to be the same letter.**

Two consequences worth stating. It **degrades honestly**: at one device pixel per glyph pixel there
is no fraction to be had and the smear is a whole cell again, which is exactly the case a letter
made of voxels is always in — so the world keeps the original rule, and keeps it for a reason
rather than by omission. And on a panel an emphasis **costs no width**, so `**this**` in the middle
of a sentence does not move the rest of the line as it is typed.

One thing that is not the emphasis and cancels it: a heading drawn *fainter* than the rows under it.
A heavier letter that is also dimmer reads as a row that has been disabled. Weight and strength are
two different statements and only one of them is emphasis.

Not read, each a decision rather than a gap: **tables** (a column rule is a third of a letter at
this size — a list says the same thing in the room that exists), **images** (nowhere to fetch a
picture from inside a world; the emoji block covers the case that matters), **raw HTML**, **setext
headings**, and **reference links**. `_` inside a word is not emphasis, so `snake_case_names`
survive being written down.

---

## The caret

**One caret, everywhere text is typed, and it pulses** (D758). A name being renamed, a number being
typed into, a document in the editor: three carets would be three rhythms, and the eye reads two of
them as one of them being wrong.

It **pulses rather than blinking**, between about a third of the ink and all of it, and never all
the way out. A hard blink is a thing that is sometimes not there, and a line that is sometimes not
there is indistinguishable from a field that has stopped taking characters — which is the one
question a caret exists to answer. Cosine rather than a square wave, for the reason the icons
overshoot: the eye reads acceleration, and a line that eases is a line that is alive where one that
snaps is one that is broken.

It is the one motion in this interface that does not answer a press, and that is not an exception to
*motion answers a press* so much as its reason: it is not saying that something happened, it is
saying where you are.

## Tooltips (the one place there are words)

**A pointer resting on a row for about a third of a second gets one sentence saying what that setting does.** Nothing else in the interface explains itself in words, and this does not either until somebody asks — the panel is exactly as wordless for everybody who does not rest a pointer on anything, which is what makes reading opt-in rather than furniture.

- **On demand; a hover is the asking.** A finger cannot rest — a touch is already a press — so this is something a mouse gets and a touchscreen does not. That is not a second code path: a touch host simply never produces a hover.
- **One-of-several answers say their own sentence.** Three buttons on a row are three questions, and the row's description can only answer the first. So a tooltip can sit on an individual option as readily as on a row; an option with nothing of its own falls back to the row's.
- One sentence. Not a paragraph, not a heading and a body.

---

## Feedback

The bar is *more than feels necessary* — tweaking is a core activity, and a control that responds weakly makes tweaking feel like paperwork.

- **Visual** on press, change, commit and rejection.
- **Motion** that shows the value moving, not just its new position.
- **Sound** on every meaningful event, synthesised rather than sampled.
- **Immediate** — feedback belongs to the frame the input happened in.

And the deliberate silences, written down because "we forgot" and "we decided not to" look identical from outside:

- **Motion answers a press, and nothing else.** Resting a pointer on a control must not run its animation. Crossing a settings panel to reach one row would otherwise make every row on the way report as though it had been used; an interface whose motion means *this happened* is worth what it is because it is rare.
- **There is no hover sound anywhere and there will not be one.** A sound reports a change, never a contact. Pressing the answer already chosen is silent, because nothing happened.
- **One sound a frame** — the one that says the most. A single press produces several true statements at once; playing all of them is a muddle two frames long for one action.
- Silent on purpose: hovering, typing a character (the character appearing is the feedback), scrolling, dragging a window, resizing, and escaping out of a value you were typing.
- **Off is exactly silent** — zero samples, verified by a test, not merely quiet.

## Marks in the world: the shape vocabulary

The tool previews are the one part of the interface that is drawn *in the scene* rather than on a
panel, and the rule that governs them is the same one as everywhere else stated for geometry: a
statement is told apart by its **shape**, not by its shade. Three statements, three shapes, and no
two of them can be confused at a glance (D363–D366):

| Shape | Statement |
|---|---|
| A hollow **ring** on each face of one voxel | *You are pointing here.* The cursor, on screen every frame, whatever tool is in hand and whatever it is in the middle of |
| An **X** on each face of one voxel | *You dropped a constraint point here* — the shape is the key that drops it |
| An outlined **box** with tinted faces | *This is what is about to happen.* The only one of the three that is a decision |

Four rules that fall out of it, all of which were learned by building the other way first:

- **Bounded by the voxel.** Never grown to a legible size in pixels. A marker with a floor in
  pixels stops being part of the world and becomes an overlay: at range it covers voxels it does
  not mean, and a mark that lies about which voxel it means is worse than one you cannot see. The
  only thing measured in pixels is line *width*, which is a different quantity — a line thinner
  than a sample flickers, and thickening it changes nothing about what the shape encloses.
- **On the faces, in world space.** Not billboarded. A shape painted on the cube foreshortens, goes
  edge-on, and is carried by whichever faces you can see, so it says which voxel *and* which way it
  is turned. A face **fades** as it turns edge-on — full strength by about 17° off it, gone only
  within two degrees of exactly edge-on — because a circle drawn on an edge-on face flattens into a
  line along the silhouette and three of those give the marker a box around it. A fade and not a
  threshold: a cut-off makes a face vanish while it is still legible, so turning slowly past a
  marked voxel loses it with a snap.
- **Hollow.** The surface being marked stays visible through the middle of its own marker, which is
  the whole job: saying "this one" without hiding the thing being lined up against.
- **A mark is depth-tested; a box is not.** A preview box is deliberately drawn through geometry,
  because carving happens inside rock and a box you cannot see the far side of is one you have to
  guess the size of. A mark on a single voxel is the opposite case — it is telling you what you are
  pointing at, and one that shines through a wall points at something you cannot reach.

Colour still follows the ink rule, with one addition: a marker takes the **material in hand**,
plainly, because it is not a decision about that material. A box takes a decision — the material's
own colour where it can be seen and the inverse where it is buried when placing, and the reverse
when carving, so the two halves of one box are never the same shade and a delete never looks like a
build. Refusal keeps red, as granted above.

---

## What this means for our implementation (Stage 15)

The style fits this engine better than it fitted the last one, and one number needs care:

- **We already have the backdrop.** The UI is diegetic (answer D16/O12), so panels are real geometry lit by the real renderer. The blurred glass and the ink both read from the composited frame we are already producing, in the pass right after tone mapping.
- **Cost is the risk.** The old implementation measured about 46 ns per pixel for the ink — several square roots per pixel over the panel area. Our T0 UI budget is 0.6 ms total (`09-performance-budgets.md`), so: work in encoded space throughout, replace the encode/decode roots with a small lookup, compute the blur once at quarter resolution, and run the ink only over panel rectangles — never the full screen.
- **The accent colour is a setting** with a default of pure green, stored per player.
- **Fonts:** a pixel font (answer L3), and it is **ours** — three by five, drawn in
  `assets/font/`, so there is no licence to carry and no glyph we cannot change. The typeface
  section above is its specification; the capital-centred baseline rule applies to it unchanged.
- **Nothing here is copied.** This document is the specification; the implementation is written from it against our own renderer.
