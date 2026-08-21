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
`--chisel`, `--ticks` and the crash tests open the world they were told to open
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

**And beside-always-winning has a second face** (D607). The copying stopped at D494; the copies did
not go anywhere. A shelf that has been through that upgrade still holds a folder of fragments dated
whenever it was made, beside-wins makes that folder the building, and every fix to the shipped clip
afterwards goes into the game and never into the world. The facility on this machine was assembled
from an 8 August copy for five days, through two fixes to the doorway it kept showing barred — and
it read as a cache fault, reasonably, because **a frozen world and a stale cache are the same
picture from the player's chair**. The order is not the bug and is not changed: preferring the
shipped file would throw away the edits beside-wins exists to protect. What is fixed is the silence.
An include taken from beside the world while the game ships a *different* one now says so by name,
once, at load. Removing such a folder is safe by construction — every include still resolves, to the
shipped file — but the game does not remove it for you: telling a stale dump from a working copy is
a heuristic, and a heuristic that deletes a player's files should not be silent. **That is a line
the library still owes the player**, and until it exists the log is the only place it is said.

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

**The editor tab asks for a file first, and a selection is one of the ways of telling it** (D455,
D743). There is no editing without something to edit, and an editor that opens on an untitled
nothing has to invent a place to put it — but choosing something in the library and then opening the
editor *is* a player asking to edit that thing, and answering *open something first* asks the
question they have just finished answering. So: whatever is selected opens here. With nothing
selected it says so and the button goes to the library tab, with *new* in the menu a right-click
gives.

When the open document has unsaved changes and something else is chosen, nothing is thrown away and
nothing pops up: the tab says which file is waiting, and opens it the moment it is saved — and
**saving is ctrl-S** (D776), from either view, with no button beside it. A control a player's hands
already know does not need a second one taking up a header.

**The document already open is a FILE, not a path** (D772). Delete `untitled.wsworld` while it is
open, press *new*, choose it: same path. Answering *is this the one already open* by comparing paths
said yes, so nothing reloaded, and what was on the screen was the world that had been deleted —
which saving then wrote back over the live one. The open document remembers when it was last written
and how many bytes it was, and any disagreement with the disk means a different file.

**A world opens here too** (D744). *open* on the worlds shelf enters the world — that is what a
double-click means and it is not changed — so a world's row now carries both things you can do to
one: *enter this world* and *edit*. Before that, the one file kind the editor could never see was
the file the game itself writes when a player presses *new*.

**What came with the game opens and does not save** (D494, D749). The refusal says so in one line
and names *duplicate* as the way, because a built-in is not the player's to lose and a delete or an
edit that came back on the next install would look like it had failed.

Then two sub-tabs, which are **two views of the same document**:

| | |
|---|---|
| **visual** | nodes with parameters, sliders and operations, wired together |
| **script** | the same graph as text, in the same language a clip file is written in |

**Editing either updates the other, live** (D452). This is not an import and an export. Both views
read and write one thing and neither is the master.

**What that one thing is, exactly** (D745). `20-clip-forge.md` §8 says *the node array is the real
representation; text and wires are two views of it*, and that is true of what a clip MEANS. It is not
what an editor can edit. By the time a file has become a `forge::Field`, one `let` has become a dozen
nodes, every name has gone, the numbers have been folded together and the comments never existed — so
a visual edit made against it could not be written back without rewriting the whole file, which is
exactly what the round-trip rule below forbids. So the document the two views share is **the
statements the author wrote**: `src/game/clip_graph.*` reads them, the names they bound, and the
numbers as they spelled them, each with the line and column it is written at. Those lower to field
nodes; the field nodes do not lift back.

Four things that has to be true for it to work, and each is a decision rather than an implementation
detail:

- **The caret is one caret, and it pulses** (D758). Everywhere text is typed — a name being
  renamed, a number being typed into, the script — because three carets is three rhythms and the eye
  reads two of them as one of them being wrong. It never goes all the way out: a line that is
  sometimes not there is indistinguishable from a field that has stopped taking characters, which is
  the one question a caret exists to answer.
- **The script view is set proportionally and has a bar you can drag** (D759), down the side and
  along the bottom. A code span steps six cells a glyph and this typeface is three columns wide, so
  monospaced it fitted twenty characters of a sixty-character line; nothing in a clip needs columns
  to line up except the line numbers, which are set on their own.
- **The script is parsed as you type, and a script that does not parse is not an error.** The
  visual view holds the last graph that parsed and greys itself; it does not empty, and nothing pops
  up. A parser that interrupts you halfway through typing a word is a parser you fight.
  **On every keystroke was literal and had to stop being** (D753): a clip parses in a millisecond
  and a *world* parses the whole building — 22 ms to splice its twenty-two pieces and 54 to read
  them — which is five frames a letter. So the verdict is asked for when the text has been still for
  a moment, and the moment is three times what the last one cost, capped at half a second. A clip is
  therefore checked on every keystroke in everything but name; a world is checked a fifth of a
  second after you stop. The **graph** is not deferred: it reads the document alone, it is 1.3 ms on
  the largest fragment in the repository, and it is what the other view is drawn from.
- **A round trip does not reformat what you wrote.** Comments, blank lines, the order you declared
  things in and your own spacing are part of the document, not decoration around it. A visual edit
  rewrites the lines it touched and leaves the rest byte-identical — otherwise nobody will trust the
  text view twice. In practice it is narrower than that and deliberately so (D746): a slider
  rewrites **the bytes of one number**, checked against what the graph said was written there, and
  touches nothing else in the file. **The author's own spelling decides the step**, so `sides=6`
  moves by one and `round=0.04` by a hundredth and nothing had to be told which; a value typed into
  the row widens the spelling only where it has to.
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

#### The materials shelf is the tool's palette (D806)

**Pressing a row on the materials shelf is choosing what to build with.** That is what *open* means
on every other shelf — a world opens by being entered — and a shelf where the obvious gesture edits
the file instead is a shelf a player has to be told about. Editing one is on its row's menu, where
editing a world is.

It replaced **Q and E**, which stepped through whatever the open world happened to declare, in the
order it declared them: a palette a player cannot see, cannot name, cannot add to and cannot take
anything out of. A library is all four of those already.

The file is read by the same parser everything else is, into **this world's own type table**, which
is what mints the id the tool builds with. Interning is idempotent, so switching between two types
does not grow the table. The row is lit and bold while it is the one in use.

The game ships six of them — stone, oak, brass, glass, lamp, moss — because the shelf is the palette
and a shelf with nothing on it is a game a player cannot build in.

#### A material is a list of properties (D786, D787, D788)

A node's panel listed the numbers that are **written**. For a shape that is complete — a `box` has
six and there are no others. For a material it is a lie by omission: the document writes three of a
dozen and the rest take their usual value silently.

So a head can declare what it **offers**, and `material` offers eleven: `rgb`, `rough`, `metal`,
`emit`, `opacity`, `ior`, `translucent`, `absorb`, `lacquer`, `sheen`, `brush`. Every one was
already read by the clip parser and none of them were on a screen. A property the document writes is
the number the document wrote, edited byte for byte; one it does not is its usual value, and moving
its slider writes the key in.

Those properties are **sockets** now, and so is everything else a statement takes — see below.
There is no fold and nothing to open: a box wears its whole list at every size.

#### A node is its sockets, and a wire lands on one by name (D826-D831)

**This is the model, and it is the one the previous version of the game used.** *This entire thing
is uncomprehensible, rewrite the whole way in which nodes connect.* A box is:

- a **title**: the name it binds, with what KIND of statement it is written under it in that kind's
  own colour — `all` over `union`, `grain` over `param`;
- then **one row per socket**, down the left, each with a dot on the edge, its name, and either the
  name of what is wired into it or the value it holds. `--` means nothing is in it and the statement
  writes nothing, so it takes its usual value;
- and **one output**, a single dot beside the title on the right. A node makes one thing.

A socket is a place in the TEXT as much as a place on the screen, which is what keeps the two views
one document (D745). There are four kinds of place:

| where | reads | writing a wire into it |
|---|---|---|
| the statement's **bare name** | `solid all`, `paint stone` | replaces the name — a solid names one shape |
| a **positional number** | `box -1 0 -1  1 2 1` | becomes a `key=` |
| a **`key=`** | `where=grain`, `round=0.06` | written, or overwritten |
| a **`{ }` child** | `union { a b }` | added inside the braces |

The rows come in that order — bare name, positions, keys the document writes, keys the KIND takes
and this one does not. A `box` reads `x0 y0 z0 x1 y1 z1 round`; a coat reads `voxel type`, `where`,
`above`; a voxel type reads every one of its eleven properties whether the file writes them or not.
**The unwritten ones are the point**: an empty socket is where the next wire goes, and a socket that
only appears once something is in it is a socket nobody can use.

A drop is refused unless the kinds match, and the refusal names both sides — except where the two
mean something together, which is D821's five joins. **Pressing a full socket empties it**, which is
what a press on a socket meant in the version this matches.

A socket names what it holds **even when that thing has no box on the screen**: the picture is an
answer and one level under it (D796), so a union's parts are usually a level too deep to draw, and a
row reading `--` about a part that exists is a lie. It is dimmer when there is nothing to follow.

A box is therefore **two cells for the title and half a cell per socket**, and two cells wide,
always — a row is a name, a value and a dot, and one cell is not enough for any two of those, and
the title carries two lines. The layout works those sizes out BEFORE it places anything and stacks
a column by them (D832); a column spills into a second sub-column at a budget of cells rather than
of boxes, because a box is no longer a fixed height.

A row's words are as big as the row will hold, and where a name and its value cannot both fit, the
**value keeps its room**: a row whose value is cut off says nothing at all, and one whose name is
cut off still says most of it.

#### A document is made of documents (D783, D784)

The graph's right-click menu opens on **a clip** and **a material**, above the words of the
language, because they are the two biggest things a document can be made of. Each offers *a new one*
and then everything on that shelf — beside the document, on the player's own shelf, and (for clips)
what the game ships.

**A part off a shelf is copied beside the document** and included by name. An include resolves
beside the file doing the including and then in the game's own clips and *nowhere else*, so a world
that pointed at the player's own shelf would open on their machine and on nobody else's. Copying is
what makes a document something you can send — and it is what the shipped facility already is: a
world file and a folder of parts beside it.

**A new part is named before it exists**, in the place the menu was standing. The name is a file
name and not a sentence; a space becomes an underscore and anything a quoted `include` could not
carry is dropped. Neither kind is made empty, and both carry the author tag.

A **material** is a document too: a `.wsmat` holding a `material` statement with every property it
takes written out, so the file itself is the list. It is spliced in like any other part, which is
why one editor and one language cover both.

#### One box, one cell — and therefore no overlaps (D780, D781, D782)

**Every box on the graph sits on a whole cell, and a cell holds one.** A drag snaps as it moves
rather than on release, and lands on the nearest cell nothing is standing on; the layout settles a
file the same way when it reads one, authored `#@` positions first so a hand-written place keeps its
cell.

That one rule is what makes the wires possible. A box fills the top-left `kNodeWide` × `kNodeTall`
of its cell, so:

- the **channel** down the right of every column, and
- the **lane** along the bottom of every row

have no box in them at any layout, ever. A wire is five segments — out into the source's channel,
along it to a lane, across the lane, into the target's channel, and in — so it **cannot cross a
box**, by construction rather than by luck. A wire whose straight run crosses nothing takes the
straight run; and every wire sharing a channel or a lane gets its own slot in it, so parallel wires
read as a bundle.

**And the document itself has a box**, at the right, with every answer wired into it. Outside a
node, every box on the canvas is a statement nothing else uses — which is right, and looks exactly
like a list until you can see what they are answers *to*. It is not a node: it is not in the file,
nothing in it can be changed, and it cannot be dragged, joined or taken out.

`Shell::boxes_overlapping()` is the gate and reads nought always.

#### What a text view owes, and now pays (D777, D778, D779)

The script view could be typed into a character at a time and nothing else. Four things fixed that,
and they are one mechanism: **an anchor and a caret, and what lies between them is chosen.** No third
piece of state, so there is nothing to keep in step.

- **Putting the two somewhere** is a drag, a double-click on a word, a triple-click on a line, shift
  with any movement key, or ctrl-A. A word is letters, digits and the underscore, because
  `snake_case_names` are one word to whoever wrote them.
- **Ctrl-C, ctrl-X and ctrl-V go through the system's clipboard** (`platform/window.hpp`), not one
  of our own: a copy a player cannot paste into anything else is not a copy. A paste splits on
  newlines and drops the carriage returns a Windows editor sends with them, which would otherwise be
  a byte the clip carries for ever.
- **Anything that writes replaces what is chosen**, which is what every text field everywhere does —
  and a selection across the author line refuses, because a cut or a paste over the top is D774's
  edit by a longer route.
- **The sideways bar takes a drag rather than a jump** (D778), through the same `Ui::drag_handle` a
  dock divider uses; and its rectangle is worked out before the caret's hit test, because the bar
  lies inside the page and steering it would otherwise drag a selection across the file behind it.
- **A document saved is the document that was opened** (D779). `std::getline` cannot tell `a\nb\n`
  from `a\nb`, so every save was taking the last byte off a file that ended with a newline — which
  all of this repository's clips do. The editor asks the file directly and puts it back. One byte,
  and the whole promise of the round-trip rule above.

**Every node parameter is a slider** by §3, with the same double-click-to-type, and the same lack of
a cap. A node's parameters are a parameters window: they open on the left while its node is
selected, which is why the two families exist. A value that is **not** a number is not a slider —
`axis=y` is a word and `where=grain` is a name — and where it names something the document bound,
the row carries a press that goes and looks at it.

#### A document shows its answers, not all of itself (D769)

A clip is fifty boxes and a world's manifest is a hundred and thirty, and a docked panel is a
quarter of a screen. No amount of laying out makes those agree; what makes them agree is not drawing
all of it.

**A box is on screen when nothing uses it — the document's own answers — or when something that uses
it is on screen and OPEN.** So `clips/sampler.clip` opens as eight boxes: its metre, its bounds, the
one parameter nothing reads, the one material nothing paints with, four coats and the solid. Opening
a coat shows the material and the pattern it reads; opening the solid shows what it is a union of.
It is §5d's own rule about a settings panel, one panel along — *a panel that shows every control it
has at once is a panel a player reads rather than uses.*

Which boxes are open is **view state and is not written into the file**. A `#@` position is authored
and travels with the document (D756); a fold is where somebody happens to be looking, which is the
class of thing a scroll offset is.

**A box says what it offers** (D770), and all three drawings are ones the interface already uses:

| | |
|---|---|
| ▸ / ▾ | it is made of something, and this opens it — the same two drawings a settings section folds with |
| ▶ | it is a **door**: an `include`, which leads into another file. Drawn at every size, because which boxes are doors is what decides where to look, and pressable |
| the three sliders | it has numbers to change. At the detail size only: a door is rare and worth a mark always, a number is on nearly every box and a mark on nearly every box is a texture rather than a fact |

Going through a door remembers where you came from, and the header grows a **↑** — only when there
is somewhere to go, because a control that is always there and does nothing most of the time is
furniture (D486). A door you can only walk one way through is a trapdoor.

#### One colour scheme for both views (D755)

`14-ui-style.md` grants two of its five permitted colours to the same three rotations of the
player's own ink — node-graph wires, and the parts of a command line. Nothing forced them to mean
the same thing, and making them mean the same thing is what turns two schemes into one:

| | in the script | on a wire |
|---|---|---|
| **shape** | `box`, `union`, `translate`, `solid` — the words that make or move matter | a wire carrying a shape |
| **value** | `fbm`, `sine`, `checker`, and every number | a wire carrying an amount |
| **material** | `material`, `paint`, `weather` | a wire carrying a material |

**There is no legend over the graph** (D768). There was one — three swatches and three words — and a
colour nobody can look up is a colour that means nothing, so it was right; it was also a row of the
panel spent on a sentence read once, which is the first constraint in `14-ui-style.md`. The three are
learned from the script view, where the word and its colour are the same object, and from this table.
**What takes no hue is as much of the decision**: the braces and the equals signs, because they are grammar rather than a
kind of thing; a comment, because it is not the document's meaning; and **a name**, because it is
the one thing on the line the author chose and the ordinary inverted ink is the strongest mark this
interface has.

#### Getting about, and changing it

- **Zoom and pan.** The wheel zooms about the pointer; a drag with the MIDDLE button moves the
  picture (D762 — the left one is what a box-select needs, and a canvas cannot answer one gesture
  with two things).
  A document **opens whole wherever whole is legible at all** (D771, reversing D760): at a third of
  full size a name is still set at `small_text`, which is the size the loading screen letters itself
  at, and what is lost is the second line rather than the word. Centred when it fits, at the left
  when it does not, because the left is what everything else is made of. With the fold above in
  front of it, most documents now fit at full size.
- **Drag a box to move it, and where it ends up is written into the document** (D756) as a `#@ x y`
  comment on its own line — inert to the parser and carried by every copy, so a clip you send
  somebody opens laid out the way you left it. §4's rule is that nothing in the game may keep state
  about a file that the file does not carry, and a layout is state about a file. It is taken out of
  the built world's cache key, for the reason D462 gives about the author tag.
- **A box drawn over the canvas chooses several** (D762), which is Explorer's gesture and the
  library's for the same reason §4 gives. Ctrl adds one or takes it out; a right-click on something
  already chosen keeps the choice, so *take out 4* is one gesture; and a drag carries everything
  chosen at the offsets they already had. **Duplicate keeps the wiring among the copies** (D763): a
  copied union is made of the copied shape, and anything referenced from outside keeps pointing at
  the original.
- **The palette is the whole vocabulary** (D764) — eight groups, `20-clip-forge.md` §2's own, and a
  test walks every head in every one of them.
- **Right-clicking an EMPTY document opens the palette too** (D761). That is the one gesture that
  has to work when there is nothing on screen, and it was the one that did not: the way to empty a
  document is to take out the single `include` a world is made of, which is the first thing a player
  does to a world.
- **Drag out of a box's right-hand tab and onto another to join them**; press a left-hand tab to cut
  that wire; right-click for the palette, and for *take out*. Each is a line of the file being
  rewritten (D757), and **each refusal says what it is** — a ring cannot be built, a `box` is not
  made of anything, a shape written inline has no name to join with, and a thing three others are
  made of is not taken out from under them.
- **Double-click a node and the script view opens on the line it is written at**; the statement the
  caret is in is the box that is lit over here. Two views of one document means being able to get
  from either to the other at the place you were looking at rather than at the top.
- **Double-click an `include` and that file opens in the editor**, resolved the way the game
  resolves one — beside the file that says it, then the clips the game ships with (D494). A world is
  a manifest, so a visual view of one that could not be walked through would be twenty boxes and no
  way to reach anything they stand for.
- **The layout comes out of the document.** A column is how far a node is from a leaf; the rows
  within it are settled by barycentre passes rather than by the order the lines were written in, and
  a column more than fourteen tall spills into sub-columns so the picture is roughly square. Nothing
  is remembered anywhere except what a drag wrote into the file.
- **A node with no numbers of its own lists what it is MADE of**, each with a press that goes there
  and a press that cuts the wire. A union *is* its children, and a panel that answered *nothing to
  change here* to the commonest node in a clip would be a panel a player stops opening.

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
| The editor tab, **script view** | **15** — **DONE** | the clip language exists today (`20-clip-forge.md`); the live parse is new |
| The editor tab, **visual view**, and the live link between the two | **15** — **DONE** (D744–D752) | It was Stage 20's, and D456's reasoning is about an EDITOR rather than a VIEW: Stage 20 builds the palette, the wiring gesture, the sub-graphs and a node set per library, and none of those is here. This draws the document that is already open in the other tab, adds no node type and no second language, and the only edit it can make is to move a number the author already wrote. **Wiring by hand is still Stage 20's**, and the script view is where a wire is changed until then |
| The community tab | **16** | it is a multiplayer feature and there is no transport before Stage 16 |
| DHT rendezvous, so the community reaches strangers | **16+** | its own decision — see §5b |

**Until each of the last two lands, the tab exists and says so in one line.** A tab that is missing
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
| The editor's **visual** view over a 1,602-line, 702-node fragment, with a node's parameters beside it | **0.217 ms** mean, 0.222 worst | 0.6 ms |
| The editor's **script** view, coloured, over the same | **0.287 ms** mean, 0.292 worst (one run in five spikes to 0.585) | 0.6 ms |

The script view being the expensive one is the figure worth explaining, and it is the colouring: a
line was one mark and is now a mark per run of one kind. A 1,602-line fragment costs what a
fifty-line sampler does either way, because only what is on screen is drawn. The whole editor is
about a tenth of a millisecond of median frame — 3.16 ms against 3.06 with nothing open.

The first is what D441 is for: nothing above the device, the swapchain and one compute pipeline
exists when the title is up, so the three-second budget is spent on opening a window and not on
building a world. The second is the tiling in `src/ui/draw.hpp` paying for itself — the marks
dispatch covers only the tiles the host says have something in them, so the world in the middle of
the screen costs nothing at all, and with no window open the pass does not run.

Where it lives:

```
src/game/clip_graph.*  the document as a graph: the statements, the names between them, and where
                     every number is written. ws_game rather than ws_forge for the reason
                     markup.hpp is there -- it is a reader of a text format, and ws_ui links
                     ws_game and does not link ws_forge
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
- **Clip, material and mod libraries** are on the shelf and list their own kind, but
  *open* on them goes to the editor rather than to a tool, because the tools are their own stages.
- **A selection is not dragged between folders yet.** *move* exists in the library and has no
  gesture on it; the rest of §4's operations do.

**And one thing that had to be added to make any of it checkable.** Every measurement in this
project is taken by a flag that walks *past* the title, so the one screen the game opens on was the
one screen no automated run would ever look at — the same failure `14-ui-style.md` names for the
tool previews, *a shape nobody can photograph is a shape nobody notices has stopped being drawn*.
So `--title-shot FILE` photographs the title after `--title-frames N`, and `--title-open
worlds|settings|both` puts a docked window up first, in the world as well as on the title.
**`--open-editor FILE` opens the editor on a document**, `--editor-view script|visual` chooses which
of the two views is showing, and `--editor-node NAME` chooses a node in it — which is the only way a
run with no hand on the mouse can put a node's parameters on the left, and that panel is half of what
the visual view is for (D751). It is `--open-editor` and not `--edit`, because `--edit` has been the
scripted chisel edit since Stage 5 and two flags of one name is `--clip` against `--clip-file` again
(D733, D750). **A
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
