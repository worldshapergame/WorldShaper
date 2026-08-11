# 23 — The shell: the title, the windows, the libraries, the editors

*`14-ui-style.md` is the visual language — no colour of its own, ink that inverts what is behind it,
icons carrying the meaning and words confirming it. **This is the structure that language is applied
to**: what the game opens on, where a window is allowed to live, what a library is, and why the
visual editor and the script editor are one thing seen twice.*

*Stage 15 builds most of it. Two pieces cannot land there and the stage list in §8 says which and
why. Decisions D441–D456.*

---

## 0. The one thing that changes on the first day

**The game opens on a title, not in a world.** Today `Application::run` builds the facility before
the first frame exists and there is no way to not do that — which was right while the only thing to
look at was the renderer, and is wrong the moment there is more than one world. The build moves
behind a choice: the title is up in the time it takes to open a window, and nothing is loaded until
somebody asks for something to be loaded.

That reorders startup rather than adding to it. `09-performance-budgets.md` §8 already says **cold
start to main menu ≤3 s** and **enter a world ≤5 s**; those two numbers only mean anything once
there is a menu between them.

**Every scripted run walks straight past it.** `--screenshot`, `--settle`, `--cam`, `--fly`,
`--chisel`, `--ticks`, `--stream-frames` and the crash tests open the world they were told to open
and never wait for a click. This is not a convenience: every measurement in this project is taken by
one of those flags, and a menu that a harness has to click through would end measurement here. The
same rule `19-auto-quality.md` states for the quality controller — *scripted runs never adapt* —
applied to the shell.

## 1. The title

**Two buttons: worlds, and settings.** That is the whole screen.

- **worlds** opens the worlds library — a library window, on the right.
- **settings** opens the settings window — a parameters window, on the left.

Everything else a title screen usually carries is absent on purpose. No news panel, no store, no
social feed, no "continue" that guesses which world you meant. The first constraint in
`14-ui-style.md` is *as little of everything as possible while staying legible*, and a title is the
easiest place in a game to break it.

- **Leaving** is the window's own close button and Escape. A third button saying "quit" would be a
  third of the interface spent on the one action every player already knows how to do.
- **In a world, Escape is one press and it opens both windows** (D476). It used to be two — the
  first gave the mouse back, the second opened the library — so the key everybody presses to reach
  the settings had to be pressed twice before anything appeared, and what appeared was the other
  half of the interface. Both halves of that were wrong for one reason. Giving the mouse back is not
  a state anybody asked to be in; it is what opening the menu *costs*, so it belongs to the same
  press. And the two families are one state, not two: what a player wants when they press Escape is
  *the menu*, and the interface cannot know which side of it they meant. So there are two states and
  no third — **menu** (both windows up, the pointer free, the game deaf) and **playing** (no
  windows, the mouse captured, every binding live) — and Escape swaps them. A press in the middle of
  the screen, the part a window may never cover, does the same thing, because an interface a child
  who cannot read can use is not allowed to require a key.
- **The logo is the one surface allowed a hue of its own** (`14-ui-style.md`, the five permitted
  colours), and it is animated, because it is a picture rather than furniture. It is drawn from a
  **seed** — chosen when it is pressed, and again on its own when nobody has touched anything for a
  while — and the seed decides which two of twenty-four animations run, which two of twenty patterns
  colour it, which of twelve palettes those read from, which of twelve **arrangements** its voxels
  fold into (a ring, a spiral, a bar chart of its own weight, a turning cube, a sphere, a cylinder, a
  four-dimensional rotation of the plane it lives on, a leaning card, a wave, a terrain, a vortex),
  and which of ten **sorting algorithms** re-orders its 160 slices — one per voxel row or column of
  the drawing, so nothing is quantised to anything coarser than the picture. A change between two
  seeds is one of twenty-four transitions rather than a cut, and the mark has **no bounding box**:
  what an arrangement throws wide of the drawing is drawn rather than clipped. D463–D473, and
  `src/ui/logo.hpp` for the half of it that cannot live in a shader.
- **It is diegetic** — answer O12 and D59: a place rather than a screen, with the flat fallback held
  in reserve for anything that would cost weeks. The title is the cheapest possible diegetic space:
  one room, two things in it, the ink rule doing the work it already does over the loading screen.

## 2. Two families of window, and why the side carries meaning

| Family | Where it opens | What it holds |
|---|---|---|
| **Settings and parameters** | the **left** edge | numbers and switches: game settings, a tool's parameters, a node's parameters, a material's |
| **Libraries** | the **right** edge | things you can pick: worlds, clips (characters among them), materials, mods (loose Lua among them) |

**Every window is docked, resizable, and re-dockable to any edge.** The sides above are where each
family *opens*; a player can drag either to any edge and it stays there.

Why a side at all, rather than "wherever it was last time": *changing a number* and *choosing a
thing* are different tasks, and a consistent side means the eye knows where to go without reading a
title bar. It is the same argument the icons are made of — an interface a child who cannot read can
use is one where position and shape carry the meaning.

Four rules fall out of docking, and each rules out a failure the floating-window version has:

- **Nothing floats.** A game that makes the player manage overlapping windows has handed them a
  desktop. Docked windows cannot be lost behind each other, cannot be dragged off-screen, and never
  need a "reset layout".
- **A window never covers what it is about.** The world stays in the middle. A parameters window
  editing a tool's reach is worth nothing if it is over the thing being reached at.
- **Two windows on one edge share it**, splitting along that edge with a draggable divider, rather
  than stacking on top of each other.
- **A window remembers its size per edge**, because the useful width of a library docked left is not
  its useful height docked top.

Sizes are stored per player alongside the settings; a missing or damaged layout costs the defaults,
never a crash (the same rule `19-auto-quality.md` states for `settings.txt`).

## 3. Every value is a slider, and typing goes past the end of it

**Every numeric value in the interface is a slider.** Double-click one and it becomes a field: type
any number and press enter.

**Typed values are not capped** (D444). The slider's range is the useful range, not the legal one —
it is where the handle can go, not where the value can. Type ten times the maximum and the value is
ten times the maximum, with the handle sitting at the end of its travel saying so.

Two things this is not:

- **It is not "no limits anywhere".** A value that would break the game — a render scale of zero, a
  simulation rate that divides by nothing — is refused, and the refusal says in one line what it
  would have done. A refusal that does not explain itself is indistinguishable from a bug.
- **It is not for values that are not numbers.** A choice between three things is three icons; a
  switch is a switch; a world's name is a name. Sliders for those would be the rule applied where it
  has nothing to say.

Feedback follows `14-ui-style.md`: the handle moves *with* the value rather than jumping to it,
there is a sound on commit and none on hover, and a value typed and rejected keeps what it had.

## 4. What a library is

**A library is a file manager over the real folder that kind of thing lives in.** Not a metaphor for
one — the folders are folders on disk and the files are files, under
`%LOCALAPPDATA%\WorldShaper\`:

```
worlds\      .wsworld     one file per world (answer K1) -- ONE file, D493
clips\       .wsclip      creations, procedural or not (answers H2, H4) -- and characters
materials\   .wsmat       pattern generators
mods\        .wsmod       Lua and native packages (answers K5, O17) -- and loose .wslua
cache\                    what this machine built from a world. Not a shelf, not the player's
crashes\                  reports. Not a shelf either
settings.txt              all of them, in one file, written whenever they change (D496)
```

**There is no `scripts\` and no `trash\`.** Loose Lua is a mod that is not finished (D492), and a
delete goes to the system's own recycle bin (D491) — every player already has one, knows where it is
and how to empty it, and it outlives the game not running. A delete that cannot be recycled is
refused rather than done.

**A world is one file** (D493). It was three: the `.wsworld`, the world already built from it, and
the timings of that build. A library is a file manager over a real folder, so everything in that
folder is on the screen — and two of those three are not the player's work, are worthless on any
other machine, and were nineteen megabytes beside a five-kilobyte file. They live under `cache\`
now, keyed by a hash of the world's full path, so two worlds of one name in two folders are two
caches rather than one they take turns evicting.

**What the game ships with stays with the game** (D494), in `clips\` beside the executable, and is
never copied anywhere. The shelf lists it alongside the player's own, marked as built in: it can be
opened and duplicated, not renamed or deleted, and a duplicate lands on the player's shelf — which
is what makes *duplicate* the way to edit something the game shipped. An `include` that is not
beside its own file falls back to there, **beside always winning**, so a world assembled out of
twenty-two pieces is one file that cannot be broken by deleting a folder. That bluntness is earned:
the facility's parts were copied onto the player's shelf, looked like an ordinary folder, and were
deleted three times, each time leaving a world that opened as an empty sky.

**There is no characters shelf** (D479). There was one, holding `.wsclip` files — the same format on
a different shelf — and what made it a different shelf was the idea that a character is a different
*kind* of thing. It is not. A character is a clip you can **wear** as well as one you can **stamp
into the world**, and which of those you do with it is a decision made when you use it rather than
a fact about the file. A shelf per use is a decision the player has to make on the way *in*, before
they know: save a figure to `clips\` and you could not wear it, save it to `characters\` and you
could not place it, and neither of those was a rule anybody wrote down — it was the consequence of
two folders holding one format. Two shelves for one thing also cost twice over: a duplicate landed
in one of them, a search found half of what was there, and every operation in the library had two
places to look. Anything already on the old shelf is moved to `clips\` once, on the first run that
finds it, because a shelf that stops existing must not take a player's files off the screen with it.

**A world is not one file, and the library shows all of it.** A large one is a `.wsworld` that says
`include "its own folder/piece.clip"` twenty times, plus that folder, plus the world already built
from the two (`.world`, `.load`). That is what a file manager over a real folder means, and it has
one edge that had to be handled rather than documented away (D478): a player can delete the pieces
and keep the world, and what they get is a world that opens as an empty sky.

- **Deleting a folder a world is built out of is refused**, and the refusal names the world — unless
  that world is in the same selection, because deleting a world together with its own parts is
  exactly right.
- **Deleting a world takes its parts folder and its built copy with it**, into the trash, under the
  same name, so putting it back is a rename of three things that still agree with each other. Left
  behind they are a shelf that fills with megabytes nothing can open.
- **A piece that is missing refuses the build in one line.** It used to be a warning: the document
  was parsed with a hole in it, produced forty cascading complaints about names declared in the file
  that was gone, and built to nothing. The one line that said what had actually happened was the
  first of the forty, which is the same as not saying it.

A player can drag a clip in from Explorer and it appears; a player can back up their work by copying
a folder. Nothing in the game may keep state about a file that the file does not carry, because the
file can move without the game watching.

**What a library does**, and all of it works on a multiple selection:

| | |
|---|---|
| **new folder**, and folders inside folders, to any depth | the library is how a player organises, and nobody's shelf looks like anybody else's |
| **rename, duplicate, delete, move** | drag to move; a delete goes to `trash\` rather than away, because a delete a player cannot undo is not what they pressed |
| **bulk select** by dragging a box over the icons, plus shift for a range and ctrl for adding one | exactly what a file explorer does, because that is what a player already knows |
| **the world you are standing in is bold** (D499) | the listing looks the same from inside a world as from the title, so this was the one question it could not answer |
| **sort** by name, by date, by author, by size | |
| **open** — the double-click, which means "use this": a world loads, a clip goes to the cursor, a material goes in hand |
| **a right-click** opens the same operations at the pointer, on the selection under it (D482) |

**A right-click is the only way to reach an operation, and there is no toolbar** (D482, D488). The
toolbar was seven drawings and six of them are what the menu now puts under the pointer; two ways to
do one thing is two places for it to be wrong, and the row they took is a row of the listing back.
*Up* is the exception and stays, because it is not an operation on a selection — it is *where you
are*, it sits beside the breadcrumb that says so, and a folder you are inside is not an entry in its
own listing to right-click. The cost is real and worth naming: this trades a control that can be
seen for one that has to be shown, which is a step away from the second constraint in
`14-ui-style.md`. What holds it: the breadcrumb row answers a right-click as well as the empty space
below the listing does, so the folder's own menu has a target even when a full folder has left no
empty space at all. Every operation is on the toolbar as
well, and the toolbar is the one a child who cannot read can find; this is the one somebody who has
used a computer will try first, and it is one gesture instead of aim-at-the-row-then-aim-at-the-bar.
It carries the same items in the same order, so the two can never disagree about what a library can
do. On something already selected it keeps the selection, so *delete* over four files is one
gesture; on anything else it selects that one first, because a menu about a thing you did not point
at acts on the wrong file. Each item **says how many things it is about** — *delete 4* and *delete*
are different decisions and only one of them needs thinking about — and an item that cannot act on
this selection is drawn faint rather than left out, because a menu whose length changes with the
selection is a menu whose items are never twice in the same place. Right-clicking the empty space
below the listing is the menu about the **folder** instead: new folder, new one of these, sort.

**Opening a world does not need the title** (D483). The library is the same window in a world as it
is on the title and it lists the same worlds, so opening one from inside another is something a
player will obviously try, and what used to happen was nothing — going out to the title to come back
in was not a step anybody wanted, it was a step the code needed because the only thing that could
open a world was the loop the title runs in. It now ends the way leaving does, with the world torn
down and every pool with it, and hands the next one out instead of handing out nothing. **Entering a
world closes the interface**, both windows: it is a departure rather than a setting, the windows
that were up were about choosing where to go, and once you are going they are over the thing you
went to look at. That is the same *playing* state Escape's toggle is made of (§1).

**Every file carries who made it** (D447). The author's chosen username is written into the file when
it is created and travels with every copy of it. A clip you downloaded says who made it, for ever,
in your own library and in anyone's you pass it to. The game does not let that field be edited from
inside itself — not because it cannot be forged from outside, but because a name that the interface
lets you overwrite is a name that means nothing.

## 5. The three tabs

Every library window has the same three tabs. What changes between libraries is *what kind of thing*
they are about, never the shape.

### 5a. library — yours

§4, above. The one on the left of the three and the one that opens.

### 5b. community — what is out there, right now

**A search over the files and folders of every player who is online at this moment** (D448). Files
are tagged with who made them and the tag is shown.

- **Newest first** is the default order, because it is the only one that is true without counting
  anything.
- **Trending** is by downloads, over **a day, a week, a month, and all time**.
- **A file is here while its author is reachable.** If they delete their copy, it leaves the browser
  — and the copy in your library stays, still saying who made it, no longer offered to anyone else
  (D449). That is what "auto-published" means and it is worth being exact about: publishing is not a
  transfer to somewhere, it is *being reachable while you are online*.

**How it works with no server, which is the hard part.** It runs on the same ladder the game's
multiplayer runs on — IPv6 direct, LAN, STUN hole punching, peer relay, and Steam's relay once the
game ships there (answers M1, M2; `06-multiplayer.md` §7). A peer answers three questions about
itself: what it has, what one of them looks like, and give me that one. The index is **gossiped, not
hosted** — you see what your peers have and, one hop further, what theirs advertise.

Three honest limits, all of them consequences of having no infrastructure rather than of this
design:

- **"Every online player" is bounded by who you can reach.** With no rendezvous of any kind, a
  fresh install knows nobody. The bootstrap is the connectivity ladder plus the people you have
  played with; a **DHT rendezvous** (answer M1's option b, which is explicitly *not* infrastructure
  of ours) is what widens that to strangers, and Steam's matchmaking widens it further once the game
  is there. This is the piece that decides whether the browser is a village or a city, and it is
  named as its own sub-step in §8 rather than assumed.
- **A download count with no server is a claim, not a fact.** Trending is computed from what peers
  report about themselves and is therefore approximate and gameable. It is presented as what it is —
  an ordering, not a number on a scoreboard — and *newest* stays the default for that reason.
- **Being online is being browsable, and there is no switch** (D495). Everything in your library is
  offered while you are online, full stop. D450 kept a switch for the player who had thought about
  it; what it actually was is a switch that ships on, that nobody turns off, and that every peer has
  to be asked about — whose only real effect is a browser that is sometimes mysteriously empty.
  *Online* and *browsable* being one state is what makes this work without a server at all: being
  reachable **is** the publishing, and a flag able to contradict that is a second source of truth
  about who is offering what.

### 5c. editor — the two views of one thing

**The editor tab asks for a file first.** Not a dialog: it sends you to the library tab, with *new*
sitting where the cursor already is. There is no editing without something to edit, and an editor
that opens on an untitled nothing has to invent a place to put it.

Then two sub-tabs, which are **two views of the same document**:

| | |
|---|---|
| **visual** | nodes with parameters, sliders and operations, wired together |
| **script** | the same graph as text, in the same language a clip file is written in |

**Editing either updates the other, live** (D452). This is not an import and an export. `20-clip-forge.md`
§8 already states the rule this rests on: *the node array is the real representation; text and wires
are two views of it, and either can be saved as either.* Both views read and write that array.
Neither is the master.

Four things that has to be true for it to work, and each is a decision rather than an implementation
detail:

- **The script is parsed on every keystroke, and a script that does not parse is not an error.** The
  visual view holds the last graph that parsed and greys itself; it does not empty, and nothing pops
  up. A parser that interrupts you halfway through typing a word is a parser you fight.
- **A round trip does not reformat what you wrote.** Comments, blank lines, the order you declared
  things in and your own spacing are part of the document, not decoration around it. A visual edit
  rewrites the lines it touched and leaves the rest byte-identical — otherwise nobody will trust the
  text view twice.
- **The script view is the more powerful one, deliberately.** It has expressions, loops, its own
  functions, and does not have to arrange anything on a canvas. The visual view is not a toy
  underneath it: everything the language can say has a node, including the awkward ones — a
  sub-graph is a node, a loop is a node with a body, a condition is a node with two outputs.
  Anything the visual view cannot yet show is drawn as an opaque **text node** carrying the source
  it stands for, so the two views can never disagree about what the document *is*, only about how
  much of it is drawn as wires. **A view that silently drops what it cannot draw would delete a
  player's work**, which is the one failure this whole design exists to make impossible.
- **One editor, one node set per library** (answer O19, D66). The world-generation graph, the logic
  graph, the material graph and the clip graph are the same editor with different nodes in the
  palette — the player learns it once.

**Every node parameter is a slider** by §3, with the same double-click-to-type, and the same lack of
a cap. A node's parameters are a parameters window: they open on the left while its node is
selected, which is why the two families exist.

## 5d. A parameters window folds, and every value can be put back

Two rules that arrive with the settings window and apply to every parameters window after it.

**Sections fold, and a section can hold sections** (D485). A panel that shows every control it has
at once is a panel a player reads rather than uses, and the first constraint in `14-ui-style.md` is
*as little of everything as possible while staying legible*. So settings are *you*, *interface*
(holding *colour*), *sound*, *picture* (holding *resolution* and *camera*) and *data*, and a fold is
how a control exists without being on the screen. **They open closed, every time the window opens**
and not only the first: what a settings panel opens as is its answer to *what can I change here*,
and five words with five triangles is that answer where four screens of rows is the question asked
again in more detail.

**The data section is where the files are** (D497). One button opens the folder everything lives in,
because a folder somebody has to be told the path of is a folder they cannot find. One button puts
every setting back and sends every world, clip and mod the player made to the recycle bin — in **two
presses**, the second being the decision: the button becomes a different button that says what it is
about to destroy and goes back on its own after a few seconds, so it cannot be pressed by accident
and cannot be pressed by habit either.

**Every value has a reset beside it, and a heading has one when anything under it is changed**
(D486). The gutter it sits in is reserved on every row whether or not the button is drawn, because a
column that moved when a value changed would jump as it was used. The button is drawn **only when
that value is off its default**, which makes its absence the other half of what it says: an empty
gutter down the side of a panel means *every one of these is as it shipped*, and a heading carrying
one means *something under here is not* — which is what makes a changed value findable while it is
folded out of sight. Defaults come from the one place they are written down, the in-class
initialisers of `Preferences` and `Knobs`, so there is no second copy of them to drift.

One value has no reset by design: **detail, while the quality controller owns it**. That row shows
what the machine decided this second, and a button offering to put it back is a button offering to
undo a measurement.

## 6. What the interface is made of

Restating from `14-ui-style.md` only what this document adds:

- **Symbols and animated icons carry the meaning; words confirm it.** An icon animates to say what
  it *does* — the duplicate icon splits, the delete icon opens — and never to say that the pointer
  is over it (`14-ui-style.md`: motion answers a press and nothing else). The drawings themselves
  are distance fields rather than a grid, the press curve overshoots, and the whole vocabulary is
  photographable with `--icon-sheet`; all three are `14-ui-style.md`, D474–D475.
- **The performance overlay is the shell's, not the developer HUD's** (D484). Everything drawn by
  ImGui is rendered onto the swapchain *after* the interface has been composited, so the overlay was
  drawn over the docked windows — which is right for the developer panel, because that is a tool
  being used, and backwards for a readout about the world, because a window is in front of the
  world. It is now a mark like any other: drawn before the windows, lettered in the game's own face,
  inverting against whatever is behind it, and under the glass of anything opened over it.
- **While the menu is up, the game gets no input at all** (D477). The windows are a mode, not an
  overlay: the pointer is free and the mouse is not captured, but every key the game binds went on
  reaching the world anyway, because only the *mouse* was ever asked about. Dragging a slider on the
  left of the screen also flew the camera with the other hand, the wheel over a value also changed
  the flight speed, a digit typed into a field also swapped the tool in your hand, and Z was undo
  while you were reading the settings. That is one bug and not eight, and the fix is one line: the
  game is handed an *empty* input for those frames rather than being asked to remember, binding by
  binding, who each key was for. The developer keys are deliberately outside it — they are not bound
  to anything in the world, and a panel that cannot be opened while looking at the menu is a panel
  that cannot be opened while looking at the menu.
- **Words are the three-by-five typeface** (D437) and a description is markdown (D439), which is
  what lets a clip carry a paragraph about itself without inventing a second notation.
- **The ink rule is unchanged**: every window is the world behind it, blurred, and its text is the
  per-pixel inverse of that. Docking does not change what a panel is made of.
- **A tooltip is the one place with a sentence in it**, on a pointer that rests for about a third of
  a second, and it is the deprecated project's control tooltips that D68 permits carrying over.

## 7. Where this sits in the code

The rule from `02-architecture-overview.md` holds without exception: **the interface mutates the
world only through ops.** A library that renames a file touches the file system; a library that
stamps a clip emits an op like every other edit, and multiplayer replicates it for free.

```
ws_ui      the shell: docking, windows, widgets, the ink rule, input hit-testing
           knows: ws_core, ws_game, the file system
           does NOT know: the GPU device, the network, the world's internals
ws_library the file manager: kinds, folders, the trash, author tags, the index a
           community search answers from
```

Two structural notes worth writing down before either is built:

- **A world is torn down on the way out, never shared.** `02-architecture-overview.md` §"Many worlds"
  already requires it: switching worlds rebuilds the pools rather than reusing them, so two worlds
  can never contaminate each other. The title screen is what makes that path exist at all — today
  there is exactly one world per process and it is built before the first frame.
- **The shell is a state machine with three states** — title, library, world — and the loading
  screen is what covers the transition between the last two. It already exists, it already reads the
  same `ui.glsl`, and it is the one piece of this that is finished.

## 8. What can be built when

| Piece | Stage | Blocked on |
|---|---|---|
| The title, the two buttons, and moving the world build behind them | **15** — **DONE** | nothing — this is the first slice |
| Docked windows: dock, resize, re-dock, split an edge, remember sizes | **15** — **DONE** | nothing |
| Sliders with type-in, uncapped | **15** — **DONE** | nothing |
| The worlds library, over real folders, with bulk select | **15** — **DONE** | nothing |
| The settings window, over what `19-auto-quality.md` already stores | **15** — **DONE** | nothing |
| Clip, material and mod libraries | **15** | the formats: `.wsclip` is Stage 15's own, `.wsmat` is Stage 6's, `.wsmod` is Stage 14's. There is no character library: see §4 |
| The editor tab, **script view only** | **15** | the clip language exists today (`20-clip-forge.md`); the live parse is new |
| The editor tab, **visual view**, and the live link between the two | **20** | the node editor is Stage 20's, and answer O19 says there is only one of them. Building a second one here to throw away would be the one thing the roadmap's ordering rationale exists to prevent |
| The community tab | **16** | it is a multiplayer feature and there is no transport before Stage 16 |
| DHT rendezvous, so the community reaches strangers | **16+** | its own decision — see §5b |

**Until each of the last three lands, the tab exists and says so in one line.** A tab that is missing
teaches a player it will never exist; a tab that says *this needs the multiplayer stage* is a
roadmap they can read without opening a document.

---

## 9. What is built, as of the first slice

Everything in §8's first six rows is in and running. `run.bat` opens on the title; the two buttons
are the whole screen; the worlds library is a file manager over `%LOCALAPPDATA%\WorldShaper\`;
double-clicking a world builds it and the world is torn down on the way back out.

**Both numbers this stage exists to separate, measured on the development machine:**

| | Measured | Budget |
|---|---|---|
| Cold start to the title | **543 ms** | 3 s (`09-performance-budgets.md` §8) |
| Enter the facility from the library | **0.84 s**, at the detail the clip asked for | 5 s |
| The interface, at 2560×1440, with two full-height windows docked | **0.26 ms** mean, 1.09 ms worst | 0.6 ms |

The first is what D441 is for: nothing above the device, the swapchain and one compute pipeline
exists when the title is up, so the three-second budget is spent on opening a window and not on
building a world. The second is the tiling in `src/ui/draw.hpp` paying for itself — the marks
dispatch covers only the tiles the host says have something in them, so the world in the middle of
the screen costs nothing at all, and with no window open the pass does not run.

Where it lives:

```
src/ui/glyphs.hpp    the typeface's metrics, generated from the font and checked against it
src/ui/ink.*         the ink rule on the host, which is what can be swept and tested
src/ui/draw.*        the list of marks, and the tiles each one reaches
src/ui/widgets.*     the controls: buttons, sliders with type-in, switches, tabs, fields, bands
src/ui/dock.*        edges, splitting, re-docking, sizes per edge, the layout file
src/ui/library.*     the file manager, the author tag, the trash
src/ui/shell.*       the state machine, the title, the settings window, the library window
src/ui/logo.*        the mark: which seed, when it changes, and the sorting that re-orders
                     its slices -- on the host, because a pixel cannot sort (D467)
src/ui/logo_marks.hpp  how much mark each row and column of the drawing holds, generated from it
src/ui/sound.*       the synthesis; src/platform/audio.* is the device it goes to
shaders/shell.comp   every mark, drawn; the icons are functions in it
shaders/logo.glsl    the mark's drawing, animations, patterns, palettes, folds and transitions
shaders/title_room.* the room the title is a picture of
shaders/shell_blur.* the glass, at a quarter resolution, three passes
src/gpu/shell_pass.* the surface, the buffers, and the two ways it is presented
```

**The five things that are not built, each for a stated reason and none of them silent:**

- **The `.wsworld` container.** A world is a clip script with a `.wsworld` extension today. The
  single-file container with append-only journaling and save-on-every-edit is Stage 15's own and is
  the next piece; the shelf, the author tag and the path everything is opened through are already
  what it will use, so it lands underneath the interface rather than through it.
- **The community tab** — Stage 16, and the tab says so.
- **The editor's visual view** — Stage 20, and the sub-tab says so and refuses with a line.
- **Clip, material and mod libraries** are on the shelf and list their own kind, but
  *open* on them goes to the editor rather than to a tool, because the tools are their own stages.
- **A selection is not dragged between folders yet.** *move* exists in the library and has no
  gesture on it; the rest of §4's operations do.

**And one thing that had to be added to make any of it checkable.** Every measurement in this
project is taken by a flag that walks *past* the title, so the one screen the game opens on was the
one screen no automated run would ever look at — the same failure `14-ui-style.md` names for the
tool previews, *a shape nobody can photograph is a shape nobody notices has stopped being drawn*.
So `--title-shot FILE` photographs the title after `--title-frames N`, and `--title-open
worlds|settings|both` puts a docked window up first, in the world as well as on the title. **A
scripted title steps its clock by a fixed sixtieth** (D473), so `--title-frames N` means N/60 seconds
rather than however long that took on this machine; `--logo-seed N` pins which combination the mark
draws and `--logo-change N` asks it for another one on a named frame, which is the only way the
transition between two of them is ever looked at by anything other than a person watching it. A
scripted screenshot taken with a window open comes from the shell's surface rather than from the
render target, or the interface over a world would be the one picture never captured.

**One trap that came out of the shelf being real files** (D462). Every file the game creates is
stamped with who made it, and a built world is cached under a key hashed from the source that
produced it — so stamping a copy changed the key, and every world on the shelf missed the cache
sitting next to it and rebuilt from cold: coarse first, then re-sampled region by region over
minutes. From inside a world that is blocks slowly resolving, which looks exactly like the
view-driven streaming being broken and is nothing of the kind. **Who made a file is not part of what
the file builds**, so the key excludes the author tag, and seeding copies the built world along with
the clip.

`--cycle N` is the other half of it: play N frames, **leave the world**, show the title again, and
exit. That is the path D441 and D458 exist for and the only one that cannot be reached without a
hand on the keyboard — and it found a crash on its first run (D461). Anything a world installs on
something that outlives it has to come off when the world does, and there were four: the developer
HUD's event hook, which crashed, and the captured mouse, the crash report's camera context and the
platform's text composition, which merely lie.
