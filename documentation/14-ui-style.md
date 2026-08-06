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
2. **The animated logo mark** — the one *surface* allowed a hue of its own, because it is a picture, not furniture.
3. **Node-graph wires**, coloured by what they carry. Three hues because the script language has three value types; a fourth hue would have to mean a fourth kind of value. They are the player's own ink rotated by a third of the circle each, so there is still no palette written down anywhere — an interface whose ink is grey has grey wires. Wires are **stated** (full brightness) rather than inverted, because an inverted two-pixel line over glass of its own brightness is a line that is not there.
4. **Command-line parts** — verb, subject, value — using the same three rotations, for the same reason: several unlike things on one line, and telling them apart is the whole task. Joining words take no hue; they are grammar, not a kind of thing. Difference from wires: a command line sits over the *world*, so it keeps the brightness the inversion asked for, except that the swing is capped (≈0.45) and the rest spent on hue — otherwise, over a white sky, all three parts come out as three blacks and black has no hue.
5. **The accent in the blind band**, above.

---

## Icons, words and layout

- **The icon is the control; the label is the fallback.**
- When a button runs out of room, **the drawing gives up room before the word does, and never gives up all of it** — it shrinks to about 0.55 of its nominal size and stops, and only then does the word start shrinking. A button with no drawing at all is not a state the rule can reach.
- The word is drawn at the row's own text size, not a size smaller.
- **Text is centred on its capitals, not on its line box.** A line box carries descender room whether the string has descenders or not, which puts a word like `Fast` visibly high in its button with an empty band beneath. Centring each string's own ink instead would fix that word and break the column, because `Field of view` and `Haze distance (km)` would sit on different baselines. A capital is the same box for every string at a given size, so that is what gets centred — anything with a descender hangs slightly below the middle, which is what a descender is for.

---

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

---

## What this means for our implementation (Stage 15)

The style fits this engine better than it fitted the last one, and one number needs care:

- **We already have the backdrop.** The UI is diegetic (answer D16/O12), so panels are real geometry lit by the real renderer. The blurred glass and the ink both read from the composited frame we are already producing, in the pass right after tone mapping.
- **Cost is the risk.** The old implementation measured about 46 ns per pixel for the ink — several square roots per pixel over the panel area. Our T0 UI budget is 0.6 ms total (`09-performance-budgets.md`), so: work in encoded space throughout, replace the encode/decode roots with a small lookup, compute the blur once at quarter resolution, and run the ink only over panel rectangles — never the full screen.
- **The accent colour is a setting** with a default of pure green, stored per player.
- **Fonts:** a pixel font (answer L3, OFL-licensed). The capital-centred baseline rule applies to it unchanged.
- **Nothing here is copied.** This document is the specification; the implementation is written from it against our own renderer.
