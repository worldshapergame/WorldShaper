# 12 — Plain English

*How WorldShaper works, with no code words. Updated every stage. If anything here stops making sense, that's my fault, not yours — say so and I'll rewrite it.*

---

## The world is made of tiny cubes

Every single thing in the game is built from cubes about 3 centimetres across. Thirty-two of them stacked up make one metre. You are two metres tall, so you are sixty-four cubes tall.

There is nothing else. No invisible shapes, no painted-on textures pretending to be brick walls. If a wall looks like brick, it's because thousands of real cubes are sitting there in a brick pattern, and you can chip out any one of them.

## Why "sandstone" isn't really a thing

When you pick "sandstone" and start building, the game isn't placing sandstone blocks. It's placing thousands of individual stone cubes, each given a slightly different sandy colour, in a pattern that looks like sandstone.

The game doesn't know what sandstone is. It only knows "these cubes are stone, and they're these colours." That's why you can carve into a sandstone wall and the inside looks right — because there is no inside and outside, it's stone all the way through.

Materials are recipes for arranging cubes, not objects.

## How the game holds billions of cubes without exploding

Two tricks.

**First: identical things share one copy.** Imagine a wall of a million cubes, all the same shade of grey. The game doesn't store a million descriptions of "grey stone." It stores *one*, and the million cubes all point at it. If you paint one of them red, that single cube gets its own description and nobody else is affected.

This is why you can give every cube its own colour, its own list of labels, its own properties — and it costs nothing until you actually make one different from its neighbours.

**Second: empty space is free.** The game stores the world as a nest of boxes, and a box containing nothing at all takes up no space. Solid rock underground is stored as "this whole box is rock" instead of listing every cube in it. Only the interesting bits — surfaces, things you've built — cost anything.

## How you can see forever

Your screen has about a million dots on it. There's no point in the game working out ten billion cubes when only a million dots can show anything.

So the game stores the world at every scale at once: individual cubes, then groups of eight, then groups of those, all the way up to "this whole mountain is roughly this colour." When it draws a frame, each dot on your screen asks: "what's in front of me?" and the game zooms in only as far as that particular dot needs.

A mountain ten kilometres away only needs a handful of lookups. The rock at your feet gets the full detail. Neither costs more than the other.

**The important part:** this isn't the usual trick of swapping between a few pre-made "detail levels" and hoping you don't notice the switch. There is no switch. The game blends smoothly and continuously between detail levels, so there's nothing to catch your eye — no popping, no shimmer, no moment where distant things suddenly get better as you walk toward them.

And the promise that comes out of it: **anything big enough to take up even one dot on your screen is drawn in full detail.** Not "full detail up to 200 metres." Full detail, always, at any distance. Below one dot there's literally nothing left to show.

## How the lighting works, and why it's fast

Normally, a game that traces real light rays does it once for every dot on your screen — a million calculations a frame, and the result is noisy and slow.

WorldShaper does it differently, and this was your idea: **it works out the lighting on the surfaces of the cubes themselves, not on the dots of your screen.**

Picture a wall. On screen it might cover ten thousand dots — but it's one flat cube face. So the game works out "how bright is this face" once, and all ten thousand dots just read that answer.

Three things fall out of this:

1. **It's dramatically cheaper.** One calculation instead of ten thousand.
2. **The answers pile up.** The face keeps its brightness from last frame and refines it. After half a second it has effectively taken hundreds of light samples, so the picture is clean instead of grainy.
3. **Playing at a higher resolution barely costs more lighting.** The wall is still one face whether you're at 720p or 4K.

Light bouncing around corners comes for free from this. When the game traces a ray from a face and it lands on another face, it just reads *that* face's stored brightness — which already includes everything that bounced onto it. So one bounce of work gives you endless bounces of result.

**The one exception:** mirrors, glass and prisms look different depending on where your head is, so those can't be stored on a surface — there's no single answer to store. Those get calculated properly per dot, but only for the small part of the screen that actually contains glass or mirrors. That's the "hybrid" you agreed to.

## Rainbows and light patterns in water

Glass bends light. Different colours bend by slightly different amounts, which is why a prism makes a rainbow. Water focuses light into those wobbling bright patterns on the bottom of a pool.

The game does both for real. For the bright pool patterns, it fires light particles from the sun, lets them bounce and bend through the water, and **paints where they land directly onto the cube faces** — the same place all the other lighting lives. So those patterns cost nothing to display once they exist, and they show up correctly in reflections too.

They take about a fifth of a second to appear properly, which is why they look soft and shimmery, which is also how they look in reality.

## Nothing is ever lost

Pour a bucket of water into a hole. Freeze it. Melt it. Splash it around. Walk away for an hour and come back.

Every single drop is still there. The game never quietly deletes water to save effort, and never quietly makes extra.

The way it manages that: water never gets *copied* from one place to another, it gets *moved*. Every cube that wants to move somewhere puts in a request; each destination picks exactly one winner; the winner's water moves and the loser's stays put. Two things can never end up in the same place, and nothing can move out to nowhere.

On top of that, the game keeps a running tally of how much of everything exists. If the total ever changes without a good reason, that's treated as a serious bug and the automated tests refuse to let it through.

The same applies to smoke. Smoke never vanishes — it just spreads out until it's too thin to see. (Behind the scenes, once it gets thin enough the game stops tracking it cube by cube and just remembers "this area has this much smoke in it," which keeps it cheap without cheating.)

## Things that fall over

Cut a building in half and the loose half becomes a real object with real weight — worked out from what it's made of, so a stone half is heavy and a wooden half isn't. It falls, hits things, tumbles, and eventually settles.

Once it's been still for a moment, the game quietly turns it back into ordinary world cubes. That's important: without it, blowing up a city would leave thousands of objects wobbling forever and the game would crawl. This way, rubble becomes terrain and stops costing anything.

Undermine a wall and it collapses on its own, because every cube keeps track of how well supported it is, passed up from the ground.

## Explosions

Not a magic sphere that deletes everything within X metres. A pressure wave that actually travels: it rushes through open doorways, gets stopped by thick walls, blows out the windows down a corridor, shoves objects and splashes water.

Build a bunker properly and it protects you, for real reasons rather than scripted ones.

## Characters that walk without anyone animating them

You sculpt a creature out of cubes. Anything — a person, a dog, a bird, a blob with six legs.

The game then looks at the shape, finds its skeleton (roughly the way you'd find the "spine" of a shape by shrinking it inward), works out where the joints should go and which limbs are legs, and generates a walk cycle that suits that body — leg count, leg length, weight and all.

You don't animate anything. You sculpt it and it walks.

When it gets hurt or knocked over, it doesn't flip into "ragdoll mode." Its muscles just get weaker. It staggers, tries to catch itself, reaches out toward the ground, braces — and only goes fully limp when there's nothing left. Being knocked down is a gradual thing, not a switch.

## Playing with friends, with no server and no setup

Here's the thing that makes this possible: **the world is never sent over the internet.**

The landscape comes from a number — a seed. Everyone's computer builds the identical landscape from that same number. And the physics — every drop of water, every falling rock — is worked out identically on everyone's machine, because it's all done with whole-number arithmetic that produces the exact same answer on every computer in the world.

So the only thing that actually crosses the internet is **what people did**: "Alex carved a box from here to here." About thirty bytes. Everything else, both computers work out for themselves and land on the same answer.

That's why 32 people can share a world with billions of cubes on a home internet connection.

**Getting connected** without a server: you make a world, the game gives you a code, you send that code to a friend however you like, they paste it. Behind the scenes their computer and yours punch through your home routers by both reaching out at the same moment. For the small number of people whose internet setup blocks that, someone else already in the game passes their messages along.

**Nobody is in charge.** The world is divided into areas and whoever's nearest looks after each one. If you leave, other people take over the areas you were looking after — and you keep the whole world on your own computer, so you can keep playing it by yourself later. There's no "host" whose departure ends the game.

## Mods

Mods are written in a small, friendly language called Lua, and they can touch everything: new materials, new reactions, new tools, new rules, entire game modes.

The safety catch is that mods don't change the world directly — they *ask* for changes, and the game applies them the same way it applies your mouse clicks. Two useful results: a mod can never break the shared-world maths that multiplayer depends on, and a badly written mod can never freeze your game, because it gets a time limit and gets paused if it goes over.

## What runs on what

The minimum machine is a **Steam Deck**, at 1280×800, holding a steady 30 frames per second. Your own PC (the RTX 5060 Ti) is the main development target and should manage 1440p at 60–90.

On weaker machines the game doesn't cut your view distance or make things blocky. Instead, lights take slightly longer to settle after a change — a fraction of a second you're unlikely to notice — and the picture may be rendered a little smaller and scaled up when things get busy. **Detail and view distance are never what gets sacrificed**, because you said those are the things that would make the project a failure.

## What exists right now — Stage 0

Double-click `run.bat` in the project folder. It builds the game if needed and starts it.

You get a window with a moving gradient and a faint grid. That's a placeholder — the grid squares are one metre across, so it's showing you the scale everything else will be built at.

What's actually behind it, and why the boring stuff came first:

- **It builds with one click.** No setup, no configuring anything. `build.bat` builds, `run.bat` plays, `test.bat` checks everything still works.
- **It talks to your graphics card properly.** It found your RTX 5060 Ti, checked it has everything the renderer will need, and refuses to start with a clear message rather than a crash if a machine doesn't.
- **It measures itself.** Press **F1** for the developer panel: how long each part of the frame took on the GPU, how much memory traffic it used, and whether it's over the budget written down for it. Press **F2** for the small corner overlay (the one that ships with the game). Numbers in red mean something is over budget, which is treated as a bug.
- **Graphics code reloads while it's running.** Edit a shader file, save it, and the picture changes without restarting. Once the renderer exists, this is the difference between tuning it in seconds and tuning it in minutes, thousands of times over.
- **It can run with no window at all** (`--ticks 100000`). That's how the automated tests will later count every drop of water in a world and prove none went missing.
- **41 automated tests, 413,407 checks, all passing.** Mostly on the maths that everything else stands on — including the whole-number arithmetic that makes multiplayer possible, tested to prove it gives identical answers regardless of the order things happen in.

Controls: **F1** developer panel, **F2** overlay, **F5** force-reload shaders, **F11** toggle vsync, **Esc** quit.

Nothing here is a game yet. It is the floor everything else stands on, and it is measured, tested and reloadable — which is exactly what you agreed to when you said the foundations could take as long as they need.

## Stage 1 — the world now exists (you just can't see it yet)

There is now a real world underneath. You can't look at it — that's Stage 3 — but it works, and a machine can prove it does.

**Cubes know what they are.** Every cube points at a description: its colour, what it's made of, what it does, whether it burns. Identical cubes share one description automatically, so a million-cube wall of the same grey costs one description in total. Paint one cube a different colour and *that one cube* gets its own — nobody else is affected. This is what makes "every cube can be different" affordable instead of impossible.

**Storage picks its own format.** A region of solid rock is stored as the words "this whole box is rock" — eight bytes for 512 cubes. A region with three materials in it stores a short list and two bits per cube. A region a player has hand-painted with 400 different colours stores them outright. It switches between these on its own as you build, and switches back when you carve things away.

**Digging through open air costs nothing.** Holding the carve tool while flying doesn't quietly allocate a slab of memory per step — empty space stays genuinely empty.

**Every change goes through one door.** Placing, carving, later every drop of falling water: all of it is a small record saying what happened and when. Nothing writes cubes any other way. That sounds like bureaucracy and it's the reason multiplayer will work: friends will send each other those little records — about thirty bytes each — rather than the world.

**Nothing is created or destroyed by accident.** There's a running tally of how much of everything exists. After a million random edits the tally is compared against counting the entire world cube by cube, and they match exactly. If they ever didn't, the build would fail rather than shipping a world that quietly leaks water.

**Saving is honest.** Save a world, load it, save it again — the two files are byte-for-byte identical. Without that check, every save-and-load cycle would drift a little and nobody would notice until a world wouldn't open.

You can run all of this yourself:

```bash
test.bat
```

It builds, runs 128 automated tests (17.6 million individual checks), then throws a million random edits at a real world and verifies every rule above. On your machine it manages about 8 million cube-writes a second and stores the world at about 0.44 bytes per cube — right on the budget the whole memory plan was written against.

## Stage 2 — the world can now get to the graphics card

A graphics card can't reach into the computer's main memory and rummage around. Everything it draws has to be copied into its own memory first. And its memory is small — a Steam Deck has about six gigabytes for everything — while the world is infinite.

So the rule is: **only what you can see is on the graphics card.** Memory is decided by your screen, not by how big the world is. That's the trick that makes an endless world fit on a handheld.

How it works: as you look around, the renderer says "I want this area." If it's already loaded, nothing happens. If it isn't, it gets packed and sent. When space runs out, whatever hasn't been looked at for longest is thrown away — but never anything you're looking at right now.

**It's allowed to fall behind, on purpose.** Spin around fast and hundreds of areas suddenly become visible at once. Sending them all in one frame would stutter — exactly when you're moving the camera, which is the worst possible moment. So there's a strict work limit per frame: it sends what fits, and the rest arrives over the next few frames. You may briefly see something at lower detail. You will not see a hitch.

There's also a scripted test world now — rooms, towers, tunnels, an archway, a lattice, a stepped pit. Deliberately awkward shapes, because those are what break renderers and physics: thin metal bars, overhangs with nothing under them, enclosed rooms, and surfaces you see from the inside. It's built the same way every time, so a speed measurement taken today can be compared with one taken in two years.

And there's a check that runs every frame in testing: decode what the graphics card holds and compare it against the real world, area by area. They matched on all 24,273 comparisons. Without that check, every later stage would be debugging a mirage.

**Three real performance bugs got caught here**, and this is what the measuring is for:

- Repacking every cube of every area on upload: 22 milliseconds on one frame, against a budget of 0.8. Fixed by storing cubes on the graphics card in exactly the layout they already have in memory, so sending them is a straight copy.
- Checking whether an area had changed by re-reading every cube in it: 116 milliseconds a frame. Areas now keep a counter that ticks up when they change, so the check is instant.
- No work limit per frame, so one fast camera turn tried to send everything at once.

Final numbers on your machine: **0.059 ms per frame on average**, against the 0.8 ms it's allowed. You can run it yourself:

```bash
test.bat
```

## Stages 3 and 4 — you can see it now

Run `run.bat` and fly around with WASD. Right-click grabs the mouse, scroll changes speed.

**How a frame gets drawn.** For every dot on your screen, the game fires a ray into the world and asks what it hits. The clever part is *how carefully* it looks. A dot far away covers a lot of world, so checking individual 3 cm cubes out there would be work you could never see the result of. So the ray checks big chunks when it's far and small ones when it's close — smoothly, with no switch-over point anywhere.

That's why there's no view distance setting. A mountain 40 km away costs about the same as a rock at your feet covering the same dots on screen. Looking at the whole test world from 900 m away is *fourteen times cheaper* than standing in the middle of it, because from there almost nothing needs fine detail.

**How the world gets loaded.** The graphics card can only hold what's on screen. The old rule was "load everything within 30 m of the player", which sounds sensible and is wrong: something 300 m away can fill half your screen, and something 10 m away can be behind a wall.

So the renderer now says what it wants. As rays travel, any time one reaches a place the world has something and the graphics card doesn't, it writes that down. The list comes back to the main program, which loads those places. Then it goes quiet — once everything visible is loaded, there's nothing left to ask for.

That took five attempts to get right, and each wrong version looked reasonable:

- The "is there anything here" table described *what was loaded* rather than *what exists*. So anywhere not yet loaded looked like empty air, got skipped, and was therefore never requested. Nothing past the starting area ever loaded.
- Rays were cut off at the edge of what was loaded — so they could never reach anything new to report. Deadlock. They now get a margin to explore into.
- The list was assembled and then wiped before being sent. Correct on one side, all zeroes on the other.
- Rays reported the *first* thing they couldn't find, which was almost always empty sky. 22,600 useless reports a frame.
- Then they reported the *last* thing, which overshot: once the far side of something loaded, the near side was never asked for.

The fix for the last two was to give the shader a table of what the world actually has, per place. Then "the first thing I'm missing" is a real answer, and loading fills in from near to far — the order you'd notice.

**One bug worth mentioning** because of where it would have shown up: a block of settings passed to the graphics card had grown to 148 bytes. Every graphics card must support at least 128, and AMD gives exactly 128 — which is what a Steam Deck has. It worked on the development machine and would have failed on the target. Moved somewhere without that limit.

## Stage 5 — the chisel

**Building and digging.** Hold the left button to dig, the right button to build. Where you press sets one corner, where you let go sets the other, and the box between them is what happens. Both corners sit at a distance out in front of you — hold **G** and scroll to change that distance, and wind it all the way down to zero and it stops being a distance and just grabs whatever you're pointing at. That one tool therefore does both careful work on a surface and building in mid-air, without switching modes.

**Middle click drops a pin.** The shape has to reach every pin you've dropped, so it stretches out to touch them. That's how you make a cut line up with something instead of squinting at it. **R** clears the pins, **C** abandons the drag, **Q** and **E** change material, **Z** and **X** are undo and redo.

**Undo never runs out.** You asked for unlimited, and the way it's stored is what makes that affordable. The obvious approach is to photograph the voxels before changing them, which costs four bytes each — dig a five-metre cube and you've spent 16 MB, and you'd pay that again on every edit until memory ran out. Instead the game writes down a *description*: "this box was rock". Digging a tunnel through solid rock costs one line to undo however long the tunnel is. Painting every voxel of a wall a different colour costs a line per voxel — which is fair, because that's how much you actually destroyed.

That also means undo isn't a special case anywhere. It's the same kind of instruction as any other edit, so it saves, replays and will travel over the network exactly like the rest. There's a test that performs two hundred overlapping edits, undoes all two hundred, and checks the world is identical to before — and then does it forwards again.

**Seeing what you're about to do.** The outline is drawn straight into the picture rather than being real geometry, since it describes something that hasn't happened yet. Digging draws the outline as the exact opposite of whatever's behind it, so it shows up against sky, stone or snow without the game inventing a colour. Building draws it in the colour of the material you're about to place. Where the outline is buried behind something, digging switches to the colour of the stuff that's about to disappear and building switches to the opposite of its material — so you can always see the whole shape, and the two never look alike.

Drawing it *through* solid rock is deliberate. Digging happens inside rock. An outline that politely hid behind the surface would be invisible in precisely the situation the tool exists for.

**One limit worth knowing.** A single edit is capped at about a 6-metre cube. An edit happens all at once and can't be interrupted, and the measurements say that size takes about a twentieth of a second — a visible stutter and nothing worse. The cap started four times larger, which would have been a multi-second freeze. Bigger edits need the work spread over several frames, which multiplayer will require anyway.

**What it cost.** A 52-voxel cube — 140,608 voxels — applies in 0.28 ms in open air and 1.39 ms through the most detailed part of the test scene, both including working out how to undo it. A frame is 16.6 ms, so neither drops one. Getting there meant three separate fixes: a brick the box swallows whole is now overwritten in one go rather than voxel by voxel, a brick it only clips works out its encoding once for the whole patch instead of once per voxel, and undo reads a brick in a single pass. That took the worst case from 3.46 ms to 1.39 ms.

## Stage 5 — the clipboard

**Picking tools.** Numbers 1 to 9 are tool slots. Tap one to switch to it. Hold one and scroll to cycle through everything on that slot — so a single key can carry a whole family of tools instead of the belt growing a row of icons. Everything starts on slot 1 for now.

The wheel only changes tools while a number is held, and that is on purpose. The wheel is the busiest control in the game: the chisel wants it (with G) for how far out it is working, the clipboard wants it to slide a copy around, the camera wants it for speed. Making a bare scroll mean "change tool" would take it away from the tool you are using at that moment.

**Copying.** Select a box exactly the way the chisel does, and instead of changing anything you get a ghost of it — the actual voxels, see-through. Scroll to slide the ghost in whatever direction you are facing; hold shift and each click moves it a whole clip length, so pieces meet edge to edge. "." and "," add and remove copies, spread evenly between where the clip came from and where the ghost is. Arrow keys turn it. Left click puts it down, as many times as you like.

**P** picks what a paste does to what is already there: replace everything (the clip's empty parts clear what they cover), only its solid parts (its empty parts leave things alone), or only into empty space (nothing that exists is disturbed — for trim and moulding).

**O** turns on grid snap: copies round to whole clip lengths so a repeated piece tiles exactly, and the turn step changes from 7.5° to 11.25°. Both divide a quarter turn evenly — twelve steps and eight steps — so a right angle is always exactly reachable.

**Turning a clip does not lose voxels, and that took a specific method.** The obvious way to rotate is: for every destination voxel, rotate backwards, round off, and copy whatever is there. That reads some source voxels twice and skips others, so a wall of a thousand bricks comes out as 987 or 1,013. In a game whose central promise is that matter is conserved, a tool that quietly manufactures stone is not acceptable.

So a rotation is done as three *shears* instead — slide each row sideways, then each column up or down, then each row sideways again. Sliding a row is just moving it, so nothing is lost and nothing is duplicated, and three of them compose into a rotation. The voxel count is preserved by construction rather than by luck, and there is a test that turns a clip through every angle on all three axes and demands the count come out identical every time.

Two details matter: whole quarter turns are done separately by swapping axes (the shear maths misbehaves near a half turn), and every rotation is computed from the *original* clip rather than from the last rotated one, so turning something forty times does not chew it up.

**The ghost shows the real voxels**, not a box. A box tells you nothing about whether the arch lines up with the wall. It is drawn by the renderer stepping through a copy of the clip, shading each voxel exactly as it shades the world, and blending it half-transparent. When path tracing arrives it will be lit like everything else, and only that last blend stays.

It is drawn *after* the world rather than as part of it, deliberately: nothing should collide with a ghost, nothing should load because of one, and when you put it down it becomes ordinary voxels the renderer finds the ordinary way.

**Two things you reported, fixed.** The tool readout was off the bottom of your screen: it was being positioned in *pixels* while the interface works in the window's own units, and on a display with scaling those are different numbers. It now uses the interface's own size, and the game reports the mismatch at startup if there is one. And the ghost preview initially did not draw at all — the debug view that found it (F3 to mode 4) showed the renderer was doing everything right and my test clip was grey stone in front of grey ground.

## Stage 5 — the second pass

**The window was bigger than your screen, and that was the whole problem.** When the game asks for a 1600×900 window, that number is in *screen coordinates*, not pixels — and on a display with scaling those are different. Your desktop had less room than the game asked for, so the window opened with its edges hanging off the screen, taking the overlay and the tool strip with them. The window now shrinks to fit whatever room the desktop actually has, keeping its shape. Found on the development machine too, which turned out to be 1366×768.

**Right click** now drops whatever the clipboard is holding, and abandons a selection you're part way through. **Middle click** sends the ghost to whatever you're looking at, resting against the face you aimed at — much faster than scrolling it across a room.

**Scrolling speeds up the longer you keep going the same way**, and drops back to one voxel a click the moment you reverse, turn to face a different direction, or stop. The first click of any run is always exactly one voxel, deliberately: acceleration you can't escape makes lining something up impossible, and a big flat step makes crossing a room impossible.

**Copies now share the transform.** Turn the ghost a quarter turn with four copies and you get 22.5°, 45°, 67.5°, 90° — the shape ramps along the row and arrives at the full amount on the last one. Same for resizing. That turns a spiral staircase or a tapering spire into one gesture. Each copy is its own separately computed clip, so when there's no transform at all the game notices they're identical and computes one.

**Resizing.** "/" changes what "," and "." do — copies, roll, or resize — and the arrow keys always turn.

You asked for resizing not to lose or add voxels the way rotation doesn't, and I have to be straight with you: that one isn't possible. Rotation can promise it because turning something is just moving each voxel somewhere else. Making a thousand-brick wall twice as big means eight thousand bricks, and there's nowhere for those to come from except creating them.

What *is* possible, and what it does, is preserve the **shape** exactly. Sizes go in whole-number ratios only — 2×, 3×, a half, a third. Growing replaces each voxel with a solid block of itself, so nothing is missing and nothing appears in the wrong place. Shrinking replaces each block with whatever it held most of, so you lose detail but never end up with a wall full of holes. Fractional sizes like 1.5× are deliberately not offered, because they have to round, and rounding is exactly what puts stripes of doubled and missing voxels through a wall.

**The overlapping-copies bug** was real. The renderer had one budget of work shared between all the ghosts, and the first one it reached spent the lot walking through its own empty space — so every ghost behind it drew nothing. Copies vanished exactly when they lined up with each other, which is when a row of them is most worth looking at. Each copy now gets its own share.

**And one about the build itself.** The build tool told me a file was up to date when it wasn't, and left an old program behind — so a measurement I took was against code that no longer existed. The game now prints the moment it was compiled every time it starts, so that can't happen quietly again, and `build.bat clean` throws everything away and starts over.

## Stage 5 — the third pass

**Ctrl no longer sinks you.** Down is **C** now, because Ctrl is half of every shortcut anyone already knows and reaching for Ctrl+Z shouldn't drop you through the floor. Cancel is Backspace.

Chisel on slot **1**, clipboard on slot **2**. Switching tools drops whatever the clipboard was holding. With nothing selected, the wheel goes back to flight speed — which is what you want while flying somewhere to make a selection.

**Roll is gone**, since the arrows already turn the clip. "/" now just switches "," and "." between *copies* and *size*.

**Resizing is any size now, per axis.** 4.5×, 9.843×, a third — whatever, in small steps, and an axis can be squashed all the way down to one voxel thick. In size mode the **up and down arrows stretch along whichever direction you're facing**: look at the long side of a wall and hold up to make it longer.

I have to restate the promise, because it changed. At whole-number sizes it's still exact — each voxel becomes a solid block of itself. At an awkward ratio it can't be, and what it guarantees instead is that **the result is never torn**. Every voxel in the new shape asks "what part of the old shape lands on me", so a solid block stays solid at 0.37× or at 9.843×. The way that goes wrong — which this deliberately avoids — is pushing old voxels forward into the new grid and rounding, which leaves stripes of holes.

**O now does something you can see.** Both 11.25° and 7.5° were arbitrary angles, so neither looked "snapped" and the setting appeared to do nothing. Snapped now means **quarter turns**, which is the only rotation a voxel grid holds without any resampling at all — the clip stays square to the world and its sides swap exactly.

**Scroll acceleration actually works now.** You were right about why: I was treating "still scrolling" as "the wheel reported something this frame", and a wheel doesn't work like that — it sends separate notches with idle frames between them. Every gap wiped the speed. It now keeps the run alive through a quarter second of quiet, so a burst of scrolling builds up properly and stopping still puts it back to one voxel a click.

**The overlapping-copies fix, and the mask.** Clips now carry a coarse map of themselves — one flag per 8×8×8 block saying whether there's anything in it. A selection is mostly air, and without that map a ray entering a ghost has to walk every empty voxel before reaching anything worth drawing; on a large clip it ran out of allowance first and drew nothing at all. With it, empty blocks are jumped over whole. A 120-voxel cube of mostly air now ghosts in 0.09 ms and shows its contents.

It costs about 15% in the opposite case — lots of small dense copies stacked on each other — because the extra check happens whether it saves anything or not. That trade is worth it: the case it fixes was broken, the case it costs was already fast.

## Stage 5 — "can an object have its own grid?"

You asked this directly, so here is the direct answer: **not as world voxels, and not because I built it wrong.**

A voxel world has exactly one grid. That is not a limitation I chose in some corner of the code — it is what makes a voxel world a voxel world, and everything is built on it: the bricks, the tree that finds them, the mask that skips empty ones, the ray marcher, the streaming, the instruction log, the replay that keeps two players in sync. A voxel exists at a whole-number position the way a pixel exists at a whole-number position on a screen. Something sitting between the squares isn't a voxel that's off-grid; it's a thing that has to be redrawn onto the squares to become voxels at all.

**But the thing you actually want does exist in the plan, and I did build toward it.** Decision D56, made back in the questions round, says: *a clip carries a flag for whether stamping it produces world voxels or a free-standing physics object.* A free-standing object keeps its own grid, at its own angle, at its own offset, and is drawn with its own transform — a boulder, a door, a ship. It is never merged into the world grid, so it never has to be square to it. That arrives with rigid bodies in Stage 12, and nothing in what I've written blocks it.

So: two different operations, and the game will have both. *Stamp* turns a clip into world voxels, on the world grid. *Place as an object* keeps it as itself. Right now only the first exists.

Meanwhile **O has gone back to being about position**, since you're right that it was never about rotation: on, the clip moves in whole clip lengths so copies tile and it carries its own spacing; off, it moves a voxel at a time. Turning is always 7.5° a press — twelve presses is a quarter turn exactly, so a right angle needs no special mode.

## Stage 5 — four bugs you found

**Thin slopes vanished when shrunk.** Correct, and the cause is arithmetic. Each voxel of the smaller version looked at the block of the original it covers and took whichever material there was most of. Shrink a one-voxel-thick diagonal by half and every block it passes through is seven-eighths air — so air won, and the slope disappeared. The same sum shaved the corner off a right angle (a corner is one voxel out of eight) and thinned a one-voxel wall to nothing.

**Air no longer gets a vote.** If any matter falls in the region, the answer is matter, and only then does it ask which kind. Slopes, corners and thin walls survive. The price is that a shape can come out slightly chunkier rather than losing pieces — which is the right way round to be wrong: a slope that survives a bit thick is still your slope; a slope that vanished isn't.

**Growing past a point reverted to the original size, and lagged.** Both the same bug. It built the oversized clip, timed how long that took, discovered it didn't fit, threw it away, and fell back to the *unscaled* original. So you saw a lag and then an original-sized copy in the middle of a row of resized ones. The limit is now worked out before anything is built, so the ghost just stops growing. A second bug alongside it: the limit was applied one axis at a time, so it spent the whole allowance on the first axis — "twice as big" came out as thirty times along one side.

**Rotation collapsed past a point.** Two separate causes, both real. The maths that turns a big angle into "some quarter turns plus a small remainder" did the wrap in the wrong order, so anything past 315° was handed to the shears whole — a near-half-turn, which is exactly where that method blows up. And the accumulated angle was wrapped at 360°, which snapped every copy back to the start at once. Neither wraps now: a row can wind past a full turn and keep going, which is how you get a spiral of more than one revolution.

**And the stretch direction was wrong**, which turned out to be the same root cause as the speed problem: resizing was applied *before* rotating, so it stretched the clip's own axes, and after a quarter turn "the way I'm looking" pointed somewhere else. Turning first and resizing second fixes the direction and, as a bonus, stops three shear passes running over the enlarged copy — growing three times was costing twenty-seven times the work for the same result.

## Stage 5 — copies that carry on past the ghost

**O is 11.25° again** — my mistake, corrected. Free turning is 7.5°, snapped is 11.25°, and both divide a quarter turn evenly (twelve steps and eight), so a right angle is exactly reachable either way. O still also makes the clip move in whole clip lengths so copies tile.

**The copy count now goes negative, and it means something different when it does.**

Positive is what it was: the copies fill the gap *between* the original and the ghost, evenly, with the last one landing exactly on the ghost. You aim the ghost, you aim the row.

Negative flips it round: the ghost becomes the *first* step, and the copies carry on *past* it, taking that same step again and again. Wind the count down past one and it goes straight into the negatives — zero is skipped, since no copies at all is what dropping the clip is for.

And the step is the whole step, not just the distance. If the ghost is simply moved, the row runs straight. If the ghost is also **turned**, then every step is turned by however much the clip has turned by the time it's taken — so the row bends round instead of running off in a straight line. Nothing is being swung around a pivot; each copy is just one more step from where the last one ended, and each step points slightly further round than the one before.

Turn the ghost a quarter turn and put four copies past it and they close a square, coming back to where they started. Turn it a little and you get a long shallow arc. That's the "incrementally bent" you asked for.

## The three bugs behind "chunks flicker" and "dotted lines"

You reported two things. They turned out to be three separate faults, and all three come down to the same kind of mistake: something the code could only have known by being *told*, and nobody told it.

**One: the memory budget was counting the wrong thing.**

The game keeps two separate pools of graphics memory. One holds each block's *contents*; the other holds a fixed-size *slot* per block, whatever's in it. The budget was worked out by assuming an average block costs about a kilobyte of contents — which is true for a detailed, mixed-up world.

A huge flat build is the opposite. Nearly every block is entirely one material, and a block like that stores almost nothing — eight bytes — but still needs a whole slot. So the contents pool sat at 8 MB of its 1 GB while the slots ran out completely.

The numbers landed exactly: 651,465 blocks across 128 chunks came to 1,048,576 slots, which was the cap to the byte. Once the cap is hit, the game throws away a chunk to make room, and the chunk it throws away is the wrong kind, so it does it again, and again, forever. That's the flickering — chunks endlessly evicting each other.

**Two: after a big edit, most of the changed chunks were never told they'd changed.**

The graphics card asks for chunks it *can't find*. That's the whole system, and it works beautifully for exploring: look somewhere new, the chunks you're missing get reported, they arrive.

It's blind to one case. If a chunk is already loaded but the world underneath it has changed, the graphics card *can* find it — so it never asks. Only the edit knows. The edit did say so, but a request only lives for one frame, and a frame can serve four chunks. A big build touches hundreds. The other few hundred were quietly dropped and never mentioned again, so they went on drawing what used to be there until something unrelated knocked them out.

Now the edit's notice is remembered until it's actually acted on. That's the patch you saw sitting there that vanished when you got close.

**Three: the dotted lines on the wood.**

Far away, a single pixel covers several voxels, so the game doesn't check every voxel — it checks them in little 2×2×2 groups. A group counts as solid if *any* voxel in it is.

Which means a group sitting half-in, half-out of your floor sticks up a bit above the real floor. A ray coming in at a shallow angle clips the **edge** of that bump instead of landing on the top. The position is off by one voxel — three centimetres, less than the pixel can show, genuinely invisible. But the *lighting* flips from "floor, facing the sun" to "wall, in shadow", and that's not invisible at all. One dark pixel.

The reason it looked like scattered dots rather than a band at one distance: neighbouring pixels are deliberately given slightly different detail levels, to stop the world visibly changing quality in rings around you. That scatters the faulty ones across the whole surface.

The fix: the detail level still decides the **colour** — a distant pixel gets a blended average, which is the entire point and stops the far-off world sparkling. It no longer decides the **shape**. Shape is always checked one voxel at a time.

I was worried that would cost speed. It doesn't — the data's already been fetched, so it's a few more steps through memory the game is holding anyway. Before: 1,722 wrong pixels, 1.929 ms a frame. After: 11 wrong pixels, 1.886 ms. Slightly faster, if anything, and that difference is just noise.

## Why the building was rebuilt from nothing every single time you started the game

The building isn't stored anywhere as a building. It's a recipe, and starting the game means cooking it: about two hundred million questions asked of a mathematical description, answered into a hundred and twenty-five million cubes. That takes a couple of minutes, so the answer is meant to be kept in a file beside the recipe, and every launch after the first is meant to read it back in a second.

It was never once kept. Not because the saving was broken — because of *when* it was told to save.

The building doesn't come out sharp in one go. A rough version appears immediately so there's something to look at, and then it's re-done properly one twelve-metre box at a time, nearest and most visible first, while you're standing there. The file was written when the *last* box finished. And the last box never finishes: a box behind a wall is deliberately skipped, because sharpening a room nobody can see is time taken away from the wall in front of you. So from any one spot the game gets to fourteen boxes of eighteen, runs out of anything worth doing, and stops — four short, for ever, and the file was never written. Two minutes, every launch, for a result that was already sitting in memory.

The fix is to write it when it *stops* rather than when it *finishes*, and to write down which boxes are sharp so far. Then a later run reads that back, sees which four are still rough, and if you happen to be standing somewhere they're visible from, it does those and saves again. The building finishes itself across several sessions instead of never.

Measured on the spot the game starts you in: the first run still takes 133 seconds. Every run after it takes **6.6 seconds**. Walk somewhere else once and the remaining four boxes get done, after which every launch loads the whole finished building in about five seconds.

What this does **not** fix is the stutter during that first build. Each box, when it's ready, gets stamped into the world in one uninterrupted go, and the big ones take up to seventeen seconds with everything else frozen. That's the freezing you reported, it's the next thing on the list, and the answer is to stamp it in slices across many frames instead of all at once. This change just means you only pay it once.

## The renderer rewrite — what has actually changed

You asked for three things rewritten: the path tracer made faster and cleaner, the chunk system
taken out, and world streaming made to follow what's actually on your screen. Here is where that
stands, in plain terms.

### What a "chunk" was, and why it's going

The game used to find geometry by cutting the world into fixed eight-metre boxes. To draw anything
it kept a table of which boxes were on the graphics card — and then, because that alone is far too
coarse, three more structures stacked on top of it: grids for skipping empty space, and eight tiers
of cheap blurry copies for things far away. Four different filing systems glued end to end, and
nearly every rendering bug we've ever had lived in a seam between two of them.

They existed because a chunk is a fixed size, in a renderer whose entire idea is that **nothing has
a fixed size**.

That is all replaced by one thing: a single tree that holds the world at every scale at once, from
a whole kilometre down to a single cube. A ray asks it one question and walks down. It is faster on
every camera we measure, and it uses 4.8 megabytes where the old one used 57.7.

**Chunks themselves are staying.** They're still how the world is saved to disk and sent to other
players, and that's fine — that was never the problem. It's the *renderer's* use of them that's
going.

### What's done

- The new tree is what your game runs. The old one is still there behind a key (F6) so the two can
  be compared, and it goes when the last thing reading it does.
- Editing works properly again. Carving used to tell four different systems the world had changed
  and none of them was the one actually drawing — so chiselling did nothing you could see. And a
  single carved cube used to throw away everything within half a kilometre and rebuild it; now it
  throws away the cube.
- Memory now follows your screen. Halving the resolution roughly quarters what's held, which is the
  whole point of the design — with one honest limit: it can't do that for things closer than about
  a hundred metres, because a 25 cm brick is the smallest piece the tree currently has. Lifting that
  is the "infinite detail" work, much later.
- Twenty metres around you is kept loaded whether you're looking at it or not, so collision,
  physics and editing can touch what's behind you. That was written down as a promise and wasn't
  actually implemented; now it is.
- Loading is about four times quicker. Most of a load was building the old chunk system that
  nothing draws from any more.
- **The fast renderer has real sun shadows now**, which it never had — worked out per cube face
  rather than per pixel, which is the first piece of the lighting rewrite actually in your hands.
- **Shadows now exist on the first frame you can see a surface — including when you spin round.**
  Measured by jumping the camera 180° in a settled world and counting the pixels with no shadow
  information: it was **five whole frames of a completely unshadowed room**, then a sixth still
  mostly wrong. It is now **none, from the first frame**, and the shadow you get on that first frame
  reads the same as the one 120 frames later. Three things did it: two frames were being spent on
  pure waste inside the engine, and the last two were the graphics card having to ask the rest of
  the program for permission to remember a surface — so now it just remembers it itself, and tells
  the program afterwards. Costs nothing measurable.
- **Blocks you place now cast their shadow straight away.** They did not: the block itself looked
  right immediately, but the shadow it should throw onto everything below took **hundreds of
  frames** to arrive, and after five seconds was only about half there. The reason is that every
  surface remembers what it has seen — a patch of roof that has watched the sun two hundred times
  running does not change its mind because of one new answer. So now it does: if a surface that has
  *always* seen the sun is suddenly told it cannot, that is not a mistake, it is you having built
  something, and the surface throws away its confidence and re-measures. Shadow starts moving on the
  next frame and is basically right within half a second. One case is still slow and is written
  down: sunlight coming through a hole you have just carved, which is a different fault underneath.
- **Shadows no longer arrive a second late.** You reported that when you moved the camera indoors,
  things had no shadow behind them for a while and then filled in. Here is what was happening: a
  surface only gets a shadow worked out once a ray from your eye lands on it, and to keep that cheap
  only one pixel in sixty-four asks per frame. Anything that was hidden and then revealed had to
  wait its turn — about a second. Now a surface with no shadow of its own borrows the shadow of a
  bigger, blockier surface covering it, which 512 small ones share, so it exists almost immediately.
  You get a chunky shadow at once that sharpens into the real one over the next half second, instead
  of no shadow that appears. It costs nothing measurable and the final picture is identical.

- **Deleting something now deletes its shadow, and the deleted part stops coming back black.** This
  was three complaints from you and they turned out to be one thing. When you carved a hole, the
  game kept a little empty box where the cubes had been — the cubes were gone, the box was not. And
  everything the renderer asks about the world goes through one question: *is there a box here?*
  Not *is there anything in it*. So a wall you had just deleted still answered "yes, something is
  here", and the sun went on being blocked by nothing at all. Same reason a deleted region faded
  back in solid black when you pulled away: the game averaged the colour of cubes that no longer
  existed, which is black. And the same reason bricks flickered to plain cubes while you stood
  still — the game switching between two answers that disagreed.

  The box is now thrown away the moment its last cube goes, and an 8-metre region is thrown away
  the moment its last box goes. Measured on the building with everything above three metres
  deleted: **62,756 surfaces were being shadowed by things that no longer existed, and now none
  are.** The room went from about a tenth as bright as it should be to right. It also runs faster
  than before rather than slower — the deleted-but-remembered boxes were being asked about, and
  asked about again, thousands of times a frame: **11.4 milliseconds of work per frame down to
  0.3**. Nothing about an unedited world changed at all, which was checked across seven cameras
  and three screen sizes.

- **A test run that gets slow now stops and tells you, instead of hanging.** Not something you see
  in the game, but you felt it: four times during this hunt I handed you a build that ran at one
  frame a second, because the measurement that would have caught it was still running when I sent
  it. The automated runs used to finish after a fixed number of frames — which is no protection at
  all when the frames are what got slow. They now stop after three minutes whatever happens, save
  the picture, and print "this build was too slow to reach where it was asked to go", which is the
  answer.

- **The tool previews say what they are now.** The voxel you're pointing at used to be drawn as a
  little cube, which looked exactly like the cube that means "this is what I'm about to build" —
  two completely different statements in the same shape. It's now a hollow **ring** drawn on each
  face of that voxel, in the colour of the material you're holding. Hollow, so you can still see
  the surface you're lining the edit up against; on the faces rather than floating in front of
  them, so it's obvious which voxel and which way round. Constraint points (the X key) are an **X**
  on each face, same rules. And the preview boxes now tint their faces as well as their outline,
  so you can tell which way a box is turned instead of guessing from a wireframe — the material's
  colour where you can see it, the opposite where it's buried, and the other way round for
  carving so a delete never looks like a build.

- **Two keys that weren't working, and neither was the key's fault.** E and Q cycle materials, and
  they did nothing: the world was loading its list of materials from the saved file, that file was
  written before the list was part of it, so the list came back with **one** thing in it. Cycling a
  list of one goes nowhere. It now reads the list from the building's own description, which is
  where the materials are actually declared — 550 of them.

  And Ctrl+Z. Undo was putting the world back correctly every time; it just never told the
  *renderer*, so nothing on screen changed and it looked dead. Also found while fixing it: X was
  secretly bound to "redo" as well as to dropping a constraint point, so every point you dropped
  quietly put back an edit you had deliberately undone.

- **And the shadows come back with it, in about a fifth of a second.** You noticed the shape came
  back instantly after an undo but the *light* took forever — a terrace still sunlit under a roof
  you'd just restored. Here's why, and it's a nice one.

  Every surface remembers what it has seen of the sun, and it only throws that memory away when a
  new answer flatly contradicts an unbroken run of old ones. That's deliberate: one odd reading
  shouldn't make a surface forget everything it knows. But it means a surface caught **halfway
  through changing its mind** has no unbroken run to contradict. Delete the roof and the terrace
  starts creeping from dark towards bright; undo before it gets there and it's halfway, certain of
  nothing, so it just drifts slowly back. Four hundred frames later it was still wrong.

  The fix is that the game stops guessing. It *knows* you edited, and it knows exactly where, so it
  now tells those surfaces outright: forget what you saw, start again. Measured on the same
  terrace: what took more than four hundred frames and never finished now finishes in **twenty** —
  and after that the picture is closer to the never-edited one than two identical runs of the game
  are to each other.

- **The chisel acts where you are pointing now, and O moves both tools instead of one.** Placing
  used to land the block on the *empty* voxel in front of whatever you aimed at — the convention
  every block game teaches. That made sense when the marker was a vague cube; it doesn't now that
  there's a ring drawn around the exact voxel you're on, because the tool would then act on a
  different voxel from the one it was showing you. So it acts on the one you're pointing at, and
  O switches to the old behaviour when a block genuinely has to go in front of a surface.

  O also only ever moved *placing*. Carving ignored it, so the two modes disagreed about which
  voxel the crosshair meant. It moves both now.

- **There is no limit on constraint points any more, and you can hold X to lay down a line of
  them.** It was eight, then sixty-four, and now none — drop as many as you like and they all get
  drawn.

  Storage was never the problem; the cost was. Every marker has to be checked against every pixel
  on the screen, so a thousand of them naively means a thousand checks per pixel. Two tricks fix
  that: the game works out one box that contains all of them, so a pixel nowhere near any marker
  answers "no" once and stops; and it sorts them into local clumps of thirty-two, each with its own
  box, so a pixel that *is* near some of them only checks the clumps it's actually looking at.
  Three hundred markers crammed into one dense block — the hardest case for that trick — costs
  three hundredths of a millisecond.

  Holding X also means the same voxel gets marked over and over while you hold still, so a repeat
  landing where the last one did is ignored.

- **The previews cost nothing again.** An ordinary chisel box now measures the same as no preview
  at all, because the code asks "is this pixel anywhere near the box" *before* doing the work
  rather than after.

- **A wall that should be evenly shaded was coming out as a patchwork, and one bug was most of
  it.** Each cube face works out how much light reaches it by firing rays into the room and seeing
  how far they get — a ray that dies 5 cm away means a tight corner, one that crosses the room
  means open space. The trouble was rays fired almost *sideways* along a flat wall: they skim it,
  clip the corner of a neighbouring cube, and report "something is 3 cm away" when the something is
  the wall the ray started on. Whether a face got unlucky depended on its random ray directions, so
  a perfectly flat wall came out blotchy. A ray now has to actually clear the surface it left
  before anything it hits counts. Costs nothing, and the banding goes.

  What's left is graininess, and it's honest graininess: each face has only a couple of hundred
  measurements to average, and in a dark room where this is the *only* light the eye is looking at
  a heavily amplified picture. Smoothing that out between neighbouring faces is a planned stage of
  its own and hasn't been built yet. Two other suspects were checked and cleared on the way, so
  nobody re-checks them later.

### What's next, and what it's for

The path tracer — the pretty, slow, accurate lighting on F4 — is the big one and is **under way**.
It works out lighting **for every pixel on your screen**, which is why it costs more the bigger your
window is. The rewrite moves that onto the *surfaces themselves*: light is worked out once per cube
face, and pixels just look up the answer. A face doesn't care how many pixels are looking at it, so
the cost stops growing with resolution.

Sunlight has made that move already — that's the shadows above. Sky light, lamps and bounced light
are next, on the same footing, and after that the old per-pixel lighting comes out entirely. That's
the change that's supposed to make it fast enough to be the normal renderer rather than a slideshow.

**Ambient occlusion, planned next to the sky light and not after it.** Right now every surface in
the game gets the full sky, whatever is in front of it. A wall at the back of a corridor is lit as
brightly as one standing in a field — which is most of why the inside of the building looks like a
model of a room rather than a room, even now that it has real sun shadows. Ambient occlusion is the
missing half of that: the light that *cannot* reach a surface because something is in the way. It's
what makes corners settle into shadow, what makes a doorway read as a hole in a wall, and what makes
a carved cylinder look round instead of like a stack of blocks.

Three things about how it's planned, in plain terms:

- **It's measured, not faked.** Most games guess this from the picture on screen, which is why it
  smears when you turn and disappears at the edges of the screen. Here it's worked out on the
  surfaces themselves with real rays against real voxels, the same way the sun's shadow now is.
- **It goes finer than a voxel.** Each cube face doesn't store one darkness value — it stores how
  that darkness *changes* across the face: which way it gets darker, and how fast. So the shading
  keeps getting smoother as you walk up to a wall instead of turning into 3 cm squares.
- **It costs nothing once it settles.** Shadows from the sun have to be re-checked forever, because
  the sun moves. This doesn't: if you're not carving, the answer never changes, so it's worked out
  once and then stops using any time at all. Carving reopens it around what you carved.

**All three of those are now built, and the third one is what made it fast.** It used to take one
measurement per surface per frame and keep taking them forever — so a wall got quietly smoother for
a minute and a half after you loaded, and it never stopped costing anything. Now a surface takes
sixteen measurements a frame instead of one, arrives at its final answer in about two seconds, and
then goes completely silent. Because the silence pays for the hurry, **the game is faster than it
was, not slower**: on the inside-the-building view the lighting step went from 1.24 ms a frame to
0.79, and the whole frame from 3.35 to 2.94.

Three things you'd actually notice:

- **A surface is right the moment you see it.** A new face starts from the answer of the bigger,
  blurrier face covering the same spot, so it fades from *roughly right* to *exactly right* instead
  of from *wrong* to *right*.
- **Placing and removing voxels doesn't cost anything.** Carving used to tell every surface within
  sixteen metres to start over. Sixteen metres is right for *shadows* — a shadow can land that far
  away — but this term only sees about a metre, so now only surfaces within a metre start over.
  Measured on a small carve, the lighting step is *faster* than the old build, not slower.
- **The one time it costs more is spinning round on the spot**, when a whole screenful of new
  surfaces all start measuring at once. That's about a second at roughly twice the old cost, and
  then it drops below it for good. It could be spread out to remove even that, but three different
  ways of spreading it were tried and measured and all three made it *worse* overall — the expensive
  part is having a surface still working, so the cheapest thing is to let it finish.

### The building sharpened and the lighting never found out

Worth telling you plainly, because it explains something you may have half-noticed and put down to
taste.

The building doesn't arrive all at once. It's laid down blocky in a second and a half, and then
sharpened region by region in the background while you walk around in it. That part has always
worked. What was never wired up is **telling the renderer it happened.** So for the whole life of
that feature, two things quietly went wrong on every fresh load:

- the tree the renderer walks kept the blocky version of nearly half the bricks it had already
  looked at — it holds *copies*, and nothing was telling it the copies had gone out of date;
- the ambient shading, which now works itself out once and then stops, kept the answer it had
  measured against the blocky version. Permanently. Standing still didn't fix it and walking away
  and back didn't fix it, because there was nothing left running to fix it *with*.

Measured on the same building at the same spot — one run watching it sharpen, one run loading the
finished article — **more than half the screen was wrong, by an average of 19 shades out of 255.**
Now it's 2.4, which is about as close as two runs of the same build ever get to each other.

The fix is that sharpening a region now says so, in exactly the words carving a hole already said.
It costs something, and it costs it during loading where a paste already takes seconds; once the
world has settled it costs nothing at all, because nothing is changing.

**The part worth keeping is the alarm rather than the fix.** There was no way to ask "does the
renderer's copy of the world still match the world?", so nobody had asked, and the answer had been
*no* for months. There is now, it runs with every test screenshot, and it names the first brick that
disagrees. Anything else that changes the world without going through the undo system has the same
bug today — the difference is that it will now say so out loud instead of just looking slightly off.

### Lamps now actually light things

Until now, a torch or a lamp in this building **did nothing**. It drew as a pale block and lit
nothing around it. The room it stood in was lit by a flat number the renderer applied everywhere,
which is why the deep shade under the front portico — the one place the sun genuinely never reaches
— read as a black hole with a couple of white specks in it.

Lamps are now real lights, worked out the same way everything else in this renderer is: **once per
voxel face, not once per pixel on your screen.** A surface picks one fitting to ask about, aims a
ray at it, and remembers the answer. Three things follow from that, and they're the point:

- **The cost doesn't grow with how many lamps there are.** A surface never goes through a list of
  lights — it picks one, weighted by how much light that one would actually deliver to it. A hall
  with a thousand lamps in it costs exactly what a hall with one costs.
- **The cost doesn't grow with your resolution either**, for the same reason shadows and ambient
  don't: the lighting is on the surface, and the surface doesn't care how many pixels are looking
  at it.
- **It settles and then stops.** After about a second a surface has its answer and stops asking.
  Measured on the front of the building, the lighting step goes from **2.61 ms a frame to 3.08** —
  about 18% more for that step, and about 9% on the whole frame, which is well inside what that step
  is allowed. Carving near a lamp costs more for a second or so and then goes back to nothing.

What you'll see: real pools of light on the walls behind each flame, falling off with distance, with
the columns casting proper shadows from them across the portico floor — and the fittings themselves
now glow instead of reading as pale stone.

**And they react the moment you touch them.** This was the hard part, and it's worth explaining why.
A surface that has finished measuring stops looking at anything at all — that's what makes it free —
so it can never *notice* on its own that you just smashed the lamp lighting it. So the game tells
it, on the exact frame you do it: the list of lamps has a fingerprint, and when the fingerprint
changes every surface in the world is tapped on the shoulder for one frame. Each one keeps the
answer it had but stops trusting it, so the picture **moves immediately** rather than dissolving
into speckle and re-forming. Measured, by deleting the portico's lamps mid-run: **73% of the change
is on screen on the very next frame, 97% within a quarter of a second, and it's indistinguishable
from the settled answer by half a second.** Placing, deleting, dimming, or chipping a lamp smaller
all count.

**One thing this exposed that isn't fixed.** There's a test room that's sealed shut and lit by
thirty-six lamps and nothing else. The lighting in it is *correct* — the slow reference renderer
draws the same room the same way — but the fast renderer draws it **blown out white**, because it
has a fixed brightness setting chosen for standing outdoors in daylight and no automatic exposure
yet. That never showed while every indoor room was lit by a fraction of a sky constant. It shows
now. Fixing it changes the brightness of every screenshot ever taken of this project at once, so it
wants to be its own job with its own before-and-after rather than being smuggled in with this one.

## You said the lights should be faster when you're moving. Here's what I found first

You asked for the lighting to be much faster and for a higher frame rate **while you're walking
around**. Before changing anything I went looking for a number, and there wasn't one — which turned
out to be the whole story.

Every speed figure in this project so far is taken with the camera **standing still**. That's on
purpose: the world sharpens itself in the background, so a measurement taken while it's still
sharpening is a measurement of a different world each time, and years of confusion came out of that.
The fix was to wait until everything settles and then measure. It works, and it has a blind spot the
size of the thing you're complaining about — because "settled" is exactly the state where every
surface has finished working out its own lighting and has gone quiet.

So I built the missing instrument: it walks the camera along a fixed route and reports the same
breakdown. Standing still and walking, same camera, same building, same settings:

| | standing still | walking |
|---|---|---|
| working out what you can see | 1.1 ms | 3.3 ms |
| **working out the lighting** | **1.1 ms** | **11.8 ms** |
| putting the picture together | 0.9 ms | 2.8 ms |
| **whole frame** | **3.3 ms** | **18.6 ms** |

The lighting step is **ten times more expensive when you move**, and it's nearly two thirds of the
frame. That is the thing you were feeling, and it now has a number and a repeatable test, which it
did not have this morning.

**Why it happens, in one line.** A surface works out its own light once and then stops for ever —
that's the design and it's why standing still is cheap. Walk, and you keep revealing surfaces that
have never done it yet, so at any moment about **280,000 of them are mid-calculation**, firing
roughly six million light probes a frame between them.

**And the biggest single waste is embarrassing and cheap to fix.** Each of those probes is a short
ray — most of them stop within a metre. But every single one starts by walking the whole world
index from the top, a half-kilometre box down to a single brick, before it takes its first step.
Twenty-five probes leave the same spot on the same surface in the same frame, and all twenty-five do
that walk from scratch. They can share it: same answer, same picture, pixel for pixel.

**I stopped there rather than pushing the change in unmeasured.** That's this project's own rule and
it has saved it repeatedly — a fix that looks obvious and isn't measured is how three separate
"improvements" got written up here and later withdrawn. The instrument is built, the cause is named,
the fix is written down along with four more behind it, and the next session starts by making them
and proving each one on the walking test rather than the standing-still one.

## And here's what happened when I made it — including the bit where you were right twice

**The short version.** Flying around, the lighting step is now a bit over **twice** as fast. Flying
around *while carving and placing blocks the whole time* — which is what you actually do — it is
nearly **six times** faster, and the whole frame went from eighteen frames a second to sixty-seven.
The picture is unchanged: I photographed the same three views before and after and the difference is
smaller than the difference between two runs of the *same* build.

| | before | after |
|---|---|---|
| **flying** — the lighting step | 10.4 ms | **4.9 ms** |
| **flying** — whole frame | 17.0 ms | **12.3 ms** |
| **flying and chiselling** — the lighting step | 48.6 ms | **8.3 ms** |
| **flying and chiselling** — whole frame | 55.5 ms (18 fps) | **15.0 ms (67 fps)** |

**The change that did nearly all of it.** The game remembers a surface's lighting for about ten
seconds after anything last looked at it, so that when you turn back it's still there. That part is
right and I didn't touch it. What was wrong is that it was still *working on* all of them — 763,000
surfaces being lit, for a picture made of about 218,000. It now works only on the ones a pixel
actually looked at, this frame. Everything else keeps the answer it already had and costs nothing.
That is the rule you asked for at the start of the whole rewrite — *if you can't see it, it doesn't
get processed* — arriving in the one place that was still ignoring it.

**And the same rule for editing.** When you carve, everything within sixteen metres has to be told
its shadows may have changed — including things behind you, or a wall you deleted would keep its
shadow when you turn round. That part still happens everywhere. What waits is the *re-measuring*: a
surface nobody is looking at is marked as out of date and re-measures when you next look at it. That
one change took the worst single frame of a carve from **75 ms to 20 ms**.

**You were right twice and both times it changed the result.**

You said *"the true worst scenario is flying while continuously editing"* — and there was no way to
test that at all. I built one, and it was five times worse than flying alone. Every number above for
that case exists because you asked for it.

Then you said *"I didn't see the chisel doing anything"*. You were looking at the screen and I was
looking at a counter that said 1.4 million blocks changed. The counter was right and meaningless:
the test was carving *empty sky three metres in front of the camera* and quietly creating ninety-four
new chunks of world a kilometre away. Two separate mistakes, both mine, and the only thing that
caught them was somebody looking at the picture. Fixed, it demolishes the front of the building as
it flies past, which is what it was supposed to do.

**One thing I want to be straight about: this isn't the hundredfold you asked for.** Six times on the
worst case, twice on the ordinary one. I went looking for the hundred and it isn't there in one
piece — I built and measured five separate ideas that each looked like it should be worth a lot, and
three of them measured as *nothing at all*, including the one the previous session had ranked first.
The cost that's left isn't one mistake; it's the light probes themselves, and there are the right
number of them. The two things that could still take a big bite are written down: making
neighbouring surfaces be worked on together so they share the same slice of the world index, and the
smoothing pass that would let each surface get away with a quarter of the probes it takes now.
Neither is guesswork now — both have a number attached.

## "Standing still, bricks flash into plain cubes" — found, and it was one line

You reported this twice, in two halves. The half that happens *when you edit* was fixed last time.
This is the other half: standing perfectly still, doing nothing, bricks briefly turn into flat
coloured cubes — sometimes the wrong colour entirely — and they even cast shadows while they do it.

**What was happening.** The game only keeps the bit of the world you're actually looking at, and it
decides that by listening to the rays it fires at the screen. Every ray says *"I stopped here"* when
it lands on something. Nothing said *"I went through here"*. So a brick a ray passes through on its
way to the wall behind it — the edge of a step, the side of a window reveal, anything you're seeing
at a shallow angle — was being read sixty times a second and, as far as the game was concerned,
never touched at all. Ten seconds later it was thrown away as unwanted, and the next frame something
noticed it was missing and built it again. Over and over, for ever.

While a brick is missing, the game draws the *next size up* over the space it left, which is a flat
cube in an averaged colour — and that colour is averaged over whatever happened to be nearby, which
is why it can come out as sky blue in the middle of a wall. That is exactly what you saw.

**How big it was.** On one camera standing still for forty seconds: **249,454 bricks thrown away,
229,000 of them things the camera was pointing straight at, and 37,000 asked for again within two
seconds of being binned.** And the number that named the cause: **249,414 of those 249,454 had never
once been reported as read by anything.** Not "read rarely" — never.

**The fix is one line**, and it says the obvious thing: a ray reports the bricks it goes through as
well as the one it stops on. What it bought:

- the flicker is gone: I photographed pairs of consecutive frames on a still camera four times, and
  where before they differed by up to fifty pixels, now they are **identical every time**;
- the game got *steadier* generally. Run the outdoor view twice and compare the two pictures: before,
  they disagreed on 12,484 pixels; now, on 674. That matters beyond this bug, because "run it twice
  and compare" is how I check every change I make;
- it costs nothing measurable — every timing moved less than the same build moves against itself.

**There was a second version of the same loop, and it is fixed too.** The *lighting* rays also ask
for pieces of world — the far side of a wall, the roof over a room — and those pieces are somewhere
no screen ray ever goes, so nothing ever spoke up for them and they were thrown away and asked for
again on the same ten-second cycle. That is where the "and they even cast shadows" part of your
report came from: a piece of world that is missing counts as *solid* to the lighting, so it throws
a shadow of something that isn't there.

The rule already said a lighting ray may point at the one thing that blocked it. What it could only
ever say was *"that thing is missing"*. It can now also say *"I am using that thing"* — same one
piece of world, nothing new asked for. Rebuilds went from **29,017 to 4,660**, and every timing I
took moved less than the same build moves against itself.

**Two things I built for this and then removed, because they measured badly:**

- reporting from every ray instead of once per piece of world. Standing still it looked like a
  success; flying and carving it produced **1.5 million messages down a channel that holds 131,072**
  and threw away 1.4 million of them — including the messages that ask for new world to be loaded.
  So the cure was starving the thing it was meant to help, and it only showed up because I measured
  the moving case as well as the still one;
- a cheaper version that only checked a piece of world once every sixteen frames. It kept most of
  the cost and lost most of the benefit, and it was chasing a slowdown that turned out not to exist
  — two measurements said it was there and four said it wasn't.

**One thing I guessed and was wrong about.** I thought this would explain why about 6% of surfaces
never quite finish working out their shading. It doesn't — I can't reproduce that 6% on this camera
at all now, in any configuration, so it stays on the list as unexplained rather than quietly getting
credit here.

## "The light goes square and flickers, and what I place looks corrupted for a moment"

You reported three things at once. Two of them turned out to be **the same bug**, and the third
turned out not to be a bug at all — which is worth saying plainly, because I went looking for the
wrong one first.

**How I found it.** The light on a surface is made of four separate things: the sun, the sky, the
darkening in creases and corners, and the lamps. Each has a debug view that shows it on its own. So
instead of arguing about the picture, I set the game carving one block a second — which is roughly
what building feels like — took two frames in a row, and asked which of the four *changed between
them*. The sun: not by a single pixel. The sky: 791 pixels. The creases: 6,253. **The lamps: 442,227
pixels of a million.** Four screenshots, and the argument was over.

**What was wrong.** When you change the world, the game has to tell every surface nearby "look
again". How far "nearby" reaches ought to depend on what you are asking about. A shadow from the sun
can land sixteen metres away, so the sun's answer has to be re-checked out to sixteen metres. The
darkening in a crease fades out after about a metre, and the game already knew that — it only
re-checks that one within two metres. **The lamps had no such rule, so they used the sun's sixteen
metres.** Place one block and every surface in the room threw away everything it knew about the
lamps and started counting again from scratch — a second's work — and if you were still building, the
next block landed before it finished. So the lamps were *permanently* half-measured. That is what
"squares" is: each little square is one voxel face working it out on its own. That is what
"flickering" is: it never finishes.

**The fix.** A lamp can only change what a surface sees if the thing you built stands *between* the
surface and the lamp. So that is now the question the game asks, once, on the frame you make the
edit: for each fitting in the room, does the block you just placed sit on the line between it and
this surface? If not, the surface keeps everything it already knew. It costs a few sums on one
frame, and it does not loop over lamps on any other frame, which is a rule this renderer is built on.

What it bought, measured against the same build with the fix switched off:

- **surfaces that had finished working out their lamp light: 0 out of 120,833, now 89,408 out of 121,026;**
- the picture between two consecutive frames while you build: **302,797 pixels changing, now 79,949**
  — and 70,649 is what the same camera does when you are not building at all, so it is essentially
  at the floor;
- **two frames after placing a block, the picture is already what it will look like**: it used to be
  wrong on 578,934 pixels of a million and is now wrong on 84,023. That is your "what I place looks
  corrupted for a while";
- and it got **faster**, which I did not expect: at 2560×1440 while building, the lighting pass went
  from 11.1 ms a frame to 7.6, and the whole frame from 18.8 to 15.3. Its worst frame — the one you
  feel as a hitch — went from 25.9 ms to 18.4.

Nothing is lost by asking the narrower question: I let both versions run until they had fully worked
themselves out after a two-million-voxel placement, and the two pictures are the same to within the
noise the game has against itself.

**Undo.** I could not reproduce voxels being left behind. I placed 2,146,689 voxels, pressed undo,
and the world came back **identical to the byte** — the game computes a fingerprint of the whole
world and it matched a run that never edited at all — with the renderer's own copy of the world
agreeing with it brick for brick, and the worst pixel in the picture differing by 35 out of 255 with
no block-shaped ghost anywhere in it. What *is* left after an undo of something that big is the same
lamp re-measuring as above, spread over the whole room, and it lasts about a second. So "chunks of
them remain, only visually" is light catching up, not matter left behind.

**One thing I should flag rather than quietly close**: if a single stroke of the tool produced more
than one entry in the history, pressing undo once would take back only the last of them, and that
would look exactly like "undo only deleted part of it". Nothing I ran does that — one stroke made one
entry every time — but it is the one reading of your report I have not been able to rule out. If it
happens again, tell me what you were doing when it did (one drag, or several clicks) and that will
settle it.

## "It still goes square and flickers after a while" — the second way in

You reported the same picture again, and you were right: the fix above was real and it closed one of
the two doors into that state. Here is the other one.

**The room is relit whenever the list of lamps changes.** A surface works out its lamp light over
about a second and then stops asking, so it cannot notice on its own that you smashed the lamp
lighting it — the game has to tell it, and the way it tells it is: *the list of lamps changed, every
surface look again*. That is the fingerprint I described above, and it is right.

**But the list was in the wrong order.** The lamps are sorted by how much light each one delivers
**to wherever you happen to be standing** — that is what lets the game keep the important ones when
a scene has more lamps than it can carry. Sorting is fine. Taking the *fingerprint over the sorted
order* is not: walk two paces and the same lamps come back in a different order, the fingerprint
changes, and the game concludes the lamps changed. Nothing changed. But every surface in the room
throws its lamp light away and starts counting again — which is exactly the state the fix above got
us out of, reached by walking instead of by building.

So: build while standing still and it was fine. Build while **moving**, which is what building
actually is, and every single block you placed relit the entire room.

**How I measured it, and the number that made it obvious.** The game already prints one line at
every screenshot saying how many surfaces have finished working out their lamps and stopped casting
rays. I ran nine tool strokes twice, on the same world, from one build — once from a still camera
and once flying:

| nine strokes | times the room was relit | surfaces that had finished |
|---|---|---|
| standing still | 1 | 469,861 of 507,251 |
| flying, before | **9** — one per stroke | **0 of 997,296** |
| flying, after the fix | **1** | **264,456 of 995,684** |

Zero out of a million is the whole diagnosis: nothing ever finishes, so every surface is still
guessing every frame — one square each, changing every frame. That line was being printed all along
and I had not looked at it.

**The fix.** The fingerprint is now taken over the lamps in a fixed order that has nothing to do with
where you are standing. Moving cannot change it; placing, deleting, dimming or chipping a lamp still
does, so nothing about "the room notices instantly" is given up.

## "It still happens, and I haven't placed or erased a single voxel"

That sentence is what closed it, because nothing edit-driven can survive it. Here is the actual
cause, and the one above was a real bug that was not this one.

**Every visible surface gets a slot in a table.** That is where its light is kept — how much sun it
sees, how much sky, how dark its creases are, what the lamps give it. The table holds about a
million slots, and a surface keeps its slot for ten seconds after the last time anything asked for
it. Ten seconds of walking about is not ten seconds of *looking* at things: it is everything you
walked past. Measured on your building, ten seconds of flying leaves **995,684 slots taken out of
1,048,576** — while what is actually on your screen is about half a million.

**So the table fills, and a full table has no polite failure.** A surface that cannot get a slot has
nowhere to keep its light, and there is no host stand-in for it either, because that needs a slot
too. What it falls back on is a coarse stand-in the graphics card claims for itself, covering an
eight-by-eight-by-eight block of voxels — and that one, by design, **starts again from one single
measurement every frame**. One measurement per block per frame is a blocky picture that is
completely different next frame. That is the squares, and that is the flickering, and it needs no
edit at all — just enough time.

**How I proved it rather than argued it.** There is a switch that shrinks the table on purpose, so
the state you reach after minutes of play arrives in ninety seconds. The picture that came out is
your screenshot. And the flicker is a number: two frames in a row, camera not moving, nothing
edited — **231,409 pixels of 1,390,170 different**, against **56,284** with room in the table.

**What I changed.**

- The table now starts giving up its oldest history *before* it is full, instead of waiting until a
  surface is turned away. Spinning on the spot with a deliberately small table: **75,421 surfaces
  turned away, now none** — and it keeps the same number of surfaces, so nothing is lost.
- The rule that decides when a surface is "cold" was measuring the wrong thing. The game only asks
  about one pixel in sixty-four each frame, and it takes 64 frames — 256 at 4K — to come back round
  to any particular one. The old emergency rule was giving surfaces up after **18** frames, which is
  before it had even asked. It cannot go below that round trip now.
- And the table says how full it is, and counts every surface it turned away. It used to report only
  the number of surfaces in it, which cannot show you a table that is nearly full — the one state
  that produces this picture.

**One thing I tried first and took back out**, because it made the game slower rather than faster:
starting to give history up at *half* full. It removes every refusal too, and it costs the lighting
pass **1.9 ms a frame against 8.1** — because a surface too far away to cover a whole pixel is
rarely asked about, so a short window throws away distant things you are looking straight at and
they have to work themselves out again. Starting at seven-eighths full does the same good and
measures as costing nothing.

**Then I wired up the signal I said was the real answer, and I was wrong about what it would buy.**
The card knows exactly which surfaces your pixels are looking at — it writes that down every frame —
and the part that decides what to throw away was reading a much sparser signal instead. It reads the
real one now. What I predicted was that this would make a *small* table cheap, because I thought the
expensive part was throwing away distant surfaces by mistake. It is not: with the table squeezed hard
the lighting pass costs **7.108 ms with the old signal and 7.181 with the exact one** — the same.
What actually costs is that surfaces which genuinely leave your screen and come back have to work
themselves out again, and no amount of knowing which ones they are makes that free. I have kept the
change because it makes the rule honest and stops it getting worse at 4K, but it is not a speed-up
and I am not going to present it as one.

**And the thing I should have built three rounds ago.** Every number about this was only ever printed
when the game takes a scripted screenshot — so the exact state that produces your picture was
invisible in the one situation you keep reporting it from, and I have now diagnosed it three times
from my own reproductions instead of from your session. The game writes it into its log while you
play now: a warning the moment it starts turning surfaces away, and a line every ten seconds saying
how full it is either way.

**So: play until it looks wrong, then quit.** The log at
`%LOCALAPPDATA%\WorldShaper\worldshaper.log` will say whether the table filled, and if it did not, it
will say that too — which rules this out and points at the next thing instead of at another guess.

**You went looking and could not find it.** That is what closes this, and it is worth saying plainly
that it is the only test that counts here: everything above is a number explaining *why* it went, and
none of those numbers would have been worth anything if the picture had still been wrong. If it comes
back, the log now tells us in one line whether it is this again or something new.

## The other thing you said, and it is a real one: the hiccups

You said the detail system is not being used when you open a world, and that the game has massive
hiccups. Both are true, and they are the same thing.

A world is a *description* — the facility is a few kilobytes of instructions — and the game turns it
into real voxels the first time it opens it. That is done in stages: a rough version so you are
standing in it in a second and a half, then eighteen regions sharpened one at a time in the
background. The **sharpening** happens on a background thread, which is right. The moment where the
sharpened region is **put into the world** does not: it happens in one frame, with everything else
waiting. I measured single frames taking **1.4 s, 6.5 s, 7.0 s, 13.0 s and 14.1 s** on your machine.
That is your hiccup, and it is not the renderer — swapping the renderer changes nothing about it.

The finished version is saved so the next launch does not redo it. The catch is that the facility had
**never finished**: the last regions cost 7–26 seconds of sampling each, so a session was always
quitting part way through. It does carry on where it left off, and after enough launches it does
complete — a complete one loads in **119 ms with no hiccup at all**. But until then every launch pays,
and a brand new world always pays.

## The hiccups are fixed, and I had the wrong culprit for two sessions

Everything I said above about the hiccups was **true and about the wrong thing**. Putting a region
into the world was never slow. It was *waiting*.

Here is the whole of it. The game has a set of worker threads it hands work to. The background
sharpening was using them — correctly, that is what they are for. Putting a region in was *also*
using them, from the main thread. And the way that queue works, once a worker picks up a piece of
the sharpening it stays on it until the whole sharpening job is done. So the main thread asked for
help, got none, and then — because a thread that is waiting is told to make itself useful — **the
main thread ended up doing the background sharpening itself**. That is your frozen frame. The game
was not putting voxels in the world for fourteen seconds; it was doing somebody else's job.

**What gave it away was not a profiler.** I printed how long each region took to go in, next to how
much was in it. The same region went in **twice** — once in 146 ms and once in 7,076 ms. Identical
work, forty-seven times apart. Nothing about the work can explain that, so it was not the work. Then
I printed it next to what was running at the same time, and every single row matched the sharpening
job beside it. The one region that had nothing running beside it took **75 ms**.

The fix is that the two now have separate sets of workers, which is a few lines. On your machine,
opening the facility from scratch:

| | before | after |
|---|---|---|
| worst single frozen frame | **7.3 seconds** | **0.09 seconds** |
| all twelve of them added up | **34.7 seconds** | **0.7 seconds** |
| frames actually drawn while it builds | 453 | **5,439** |

The building comes out **identical** — I checked that rather than assumed it, by comparing a
fingerprint of the finished world from both versions, and they match exactly. The sharpening itself
got about 1% slower, which is the price of the two of them sharing a machine for a tenth of a second.

**And the thing I said was next is not next any more.** *Slice the paste across frames* was on the
list because a fourteen-second freeze had to be broken up. What is left is 31 to 92 milliseconds —
two to six frames — which is not something you have ever complained about, and cutting it up is a
genuinely risky change for no reward you would notice.

## The very large chisel stroke, which was the same mistake again

Deleting 36 million voxels at once froze for about a second. Same shape as the paste, one level
down, and I found it the same way — by splitting the frame up and printing the parts.

When you change part of the world, the renderer keeps a tree describing it and has to be told what
changed. It was being told **brick by brick**: for that delete, **1,573,269 of them**. It then had
to sort out which ones it actually held — and the answer was that it needed to fix up **13,325**
things and rebuild **no** bricks at all. Working that out took **718 milliseconds**. The useful part
of it took **4**.

The tree knows what it holds; the thing announcing the edit does not. So it now hands over **the
box** and lets the tree walk down through what it actually has, skipping anything the edit cannot
have touched. **718 milliseconds becomes 7**, and that frame is no longer the slowest frame in the
run. A one-voxel chisel is exactly as it was.

Before changing it I built a check that did not exist, because this is the sort of change that can
look right and be wrong in a way nothing notices for months. The tree stores, for every node, a note
saying *which of my eight children have anything in them* — and that note decides where light and
sight are even allowed to look. If it is wrong one way the renderer asks for something that is not
there, every frame, for ever; wrong the other way and real geometry becomes invisible and nothing
ever asks for it. None of the existing checks could see it. Now there is one, it runs at every
screenshot, and it says *the node pool agrees with the world, mask for mask*.

**What is left of that frozen second is the undo capture: 194 milliseconds.** Deleting 36 million
voxels writes down 538,169 instructions for how to put them all back, and it does that immediately,
for an edit you may never undo. That is the next thing in that frame.

## What the game will open on, and where everything will live

This is written down and not built yet — it is the next stage. Here is the whole of it in plain
terms, so you can tell me now if any of it is wrong rather than after I have built it.

**The game will open on a title with two buttons: worlds, and settings.** Nothing else. At the
moment the game builds the whole facility before it will even show you a window, which made sense
when the only thing to look at was the renderer and stops making sense the moment you have more than
one world. Nothing will be loaded until you ask for something to be loaded.

**Windows come in two kinds, and which side they are on tells you which kind.** Anything with
numbers in it — settings, a tool's controls, a node's controls — opens on the left. Anything you
pick a thing out of — worlds, your creations, materials, mods — opens on the right. All of them are
stuck to an edge rather than floating, all of them can be resized, and you can drag any of them to
any edge you prefer. The reason for the two sides is that you should be able to know where to look
without reading anything, which is the same reason the buttons are pictures.

**Every number is a slider, and you can double-click it and type instead.** What you type is not
capped — the slider's range is where the handle can go, not where the value can. If a number would
genuinely break the game it gets refused, and it says in one line what it would have done, because a
refusal with no explanation is indistinguishable from a bug.

**A library is just a folder on your computer, shown nicely.** Your worlds, your clips, your
materials and your mods each live in a real folder, and the library is a file manager over it — make
folders inside folders, rename, duplicate, drag things around, select a bunch of them by dragging a
box the way you would in Windows. Delete puts things in a trash folder rather than destroying them.
You can drag a clip in from Explorer and it appears; you can back your work up by copying a folder.

**Everything you make is stamped with your name, and that stays on it for ever** — in your library,
and in the library of anyone who takes a copy.

**Every library window has three tabs.**

- **Yours** — the folder above.
- **The community** — what everyone who is online *at that moment* has in theirs. Newest first
  normally, or sorted by what is being downloaded most today, this week, this month, or ever. It
  works the same way the multiplayer does: no server, no hosting, no port forwarding. So it can only
  show you people your machine can actually reach, and I would rather say that plainly now than have
  you find it later. If somebody deletes their copy of something you downloaded, yours keeps working
  and keeps saying who made it — it just stops being offered to anyone new.
- **The editor** — where you make one of these things rather than pick one. It asks you to choose or
  make a file first, because there is no editing without something to edit.

**The editor has two halves, and they are the same document seen twice.** One is boxes and wires
with sliders on them; the other is the same thing written as text. Change either and the other
changes as you type. Neither is the "real" one — the real one is underneath and both are views of
it. The text half will always be able to say more, because it does not have to draw anything; the
picture half will get as close to it as I can make it, and anything it genuinely cannot draw appears
as a block of text sitting in the graph rather than quietly disappearing. **Something disappearing
because the editor could not draw it would be the worst bug this thing could have**, so it is
designed to be impossible rather than unlikely.

**What lands when.** The title, the windows, the sliders and the libraries are the next stage. The
community tab needs the multiplayer stage, because there is nothing to search over before it. The
picture half of the editor waits for the stage that builds the node editor — there is only ever
going to be one node editor in this game, used for world generation, logic, materials and clips
alike, so you learn it once; building a second one now to throw away later is exactly the mistake
the whole plan is ordered to avoid. Until each of those lands, the tab is there and says in one line
what it is waiting for, because a tab that is simply missing teaches you it will never exist.

## How we'll work

- I write all the code. You never open a code file.
- Each stage ends with something you double-click and play.
- Because there's no second programmer checking my work, the game tests itself constantly — thousands of automated checks that run every time anything changes, including one that literally counts every drop of water in the world to prove none went missing.
- Your job: play the builds, make the design calls, tell me what feels wrong. That genuinely is the harder half.
- This is a big project. It'll be playable and fun long before it's finished — that's what the nineteen playable checkpoints are for.

## Chunks are half gone, and one whole renderer is gone

Two of the three things you asked for are structural rather than visible, so here is what actually
happened and what you would notice.

**The old renderer is deleted.** The game used to carry two: the one you play, and a slow
"reference" one on F4 that drew the same scene a different way. It existed to check the fast one was
telling the truth. The fast one has since learned to do sun shadows, ambient occlusion and lamps
properly, so the slow one was 3,000 lines nobody looked at. **F4 and F6 do nothing now** — those were
the two switches between renderers, and there is only one renderer.

What you would notice: **the very first launch after a graphics driver update gets much faster.**
That old renderer was one enormous shader, and your driver had to compile it once each time its
cache was cleared. I measured that at **8 seconds**. It is not compiled at all now. Ordinary
launches are unchanged — about half a second either way.

**Chunks are gone from the renderer.** A "chunk" was the 8-metre box the world used to be cut into
for drawing. The new system does not need them, and as of this session nothing that draws the world
mentions one. Gone: the traversal code, the summary tree, the eight "thumbnail" tiers that drew
distant scenery, the streaming system that decided which chunks to keep, its ten graphics-card
buffers, and the map-rebuilding routine below. About 6,500 lines.

Chunks still exist where they were always meant to — saving a world to disk, and sending it over a
network later. That part is untouched.

**Three things you can feel:**

- **Editing.** Every chisel stroke used to rebuild a chunk map over the *whole world*, however small
  the edit was: **3.9 milliseconds a stroke**, which was the single largest cost of editing. It is
  **nothing** now — the same measurement reads 0.00. What is left in an edit is the undo record,
  which is the next thing to look at.
- **Memory.** The game held **970 MB** on the graphics card for that system, including a quarter of
  a gigabyte for a table the deleted renderer used. It is **112 MB** now. That is memory back for
  the things you can actually see.
- **Loading.** A warm start is **505 milliseconds down to 340**.

What did **not** change is the frame rate standing still, and that is expected: the deleted work was
being done on the processor beside the frame rather than inside it.

**Nothing about the picture changed.** I check that with a fingerprint of the finished world and two
audits that compare what the renderer believes against what the world actually contains, at every
step. Identical every time.

**One thing I got wrong and caught.** I ran the standard measurement grid against the recorded
baseline and it showed the close-up views 40 to 70 per cent *slower*. That is not a slowdown. That
baseline was recorded before the game had shadows, ambient occlusion or lamps at all — so it was
comparing today's renderer against one doing far less work and calling the difference a regression.
The distance views, which those three barely touch, came out 40 to 50 per cent **faster**. I have
recorded a fresh baseline and marked the old one as not to be used.

**And two more, from this session, because both are the same lesson.** The first: I moved the
overlay's numbers onto the new system and, without noticing, made every frame count the whole tree
to fill them in — so a change that only *deleted* work measured slower. It cost 1.8 milliseconds a
frame and it took splitting the frame into three timed pieces to see which piece it was in. The
second: the measuring harness itself had quietly stopped reading which world each measurement was
taken against, so its "are these two runs comparable?" check was comparing nothing with nothing and
saying yes. Both look exactly like everything being fine, which is why the numbers get split up and
printed rather than trusted.

**What is next, and it is the half of your third ask that has not been built.** You said everything
is per voxel face — *"even reflections and those things"*. Sun, sky, ambient occlusion and lamps all
live on faces now. Reflections, glass and bounced light do not, and the stage that adds them cannot
be built first, for a reason worth knowing: a face only exists once you have looked at it. So a
mirror facing a wall behind you reflects nothing, a red wall out of shot bounces no red onto a white
one, and glass shows the room behind it unlit. The fix is one rule — a face is also created because
another face needs to *read* it, not only because you looked at it — with a budget so that the
things you cannot see can never starve the things you can. That comes first, and reflections and
glass come immediately after it.

Two smaller things are queued behind that and both have numbers on them: the exposure meter, which
is why the hall lit only by sconces comes out white, and the undo record above.

## That rule is now in, and on purpose you cannot see it yet

The rule above — *a face also exists because another face needs to read it* — is built. When the
lighting fires a ray out from a surface and that ray lands on something, the game now writes down the
one surface it landed on. Only that one: never anything the ray passed through on the way, which is a
line I have to hold, because "remember everything a light ray touches" is how a renderer ends up
loading the whole world behind every wall.

**Deliberately, nothing reads that list yet, and the picture is unchanged.** I checked it two ways:
standing still and flying, the picture differs from a build with the rule switched off by less than
two runs of the *same* build differ from each other. Those surfaces sit in the list costing no light
and no rays. The next change is the one that reads them — that is bounced light, and it is what
replaces the single number the game currently uses to stand in for *all* indirect light indoors.

I landed it on its own rather than together with the thing that uses it, and that turned out to be
worth it, because two faults came out that would have been invisible inside a change that also moved
every pixel:

- **The lighting got faster and that was the bug.** Each surface waits its turn for a sun ray, and
  the queue was "everything in the list" rather than "everything you can see" — so the moment the new
  off-screen surfaces joined the list, every surface you *were* looking at was updated less often.
  The lighting pass measured 17 per cent cheaper, and the price was hidden somewhere no stopwatch
  looks: each visible surface had 72 sun measurements instead of 84. A thing that got cheaper by
  doing less of its job looks exactly like a thing that got faster. It is fixed, and both now read 84.
- **The label nearly erased the light.** I first stored "this surface came from a light ray" as a
  flag inside the surface's own record — and that record is shared with the graphics card, which
  writes the light into it. Changing the flag would have sent our copy over the card's, wiping the
  accumulated light on nearly 30,000 surfaces per flight. Right picture, every self-check green, and
  only the cost moving. The label lives in a separate list now.

## Dark is now actually dark

You asked for two things and both were real: nothing should have a minimum brightness, and fog
should not let you see in the pitch black.

**The first.** Two numbers in the drawing code made a black surface impossible. One added half of the
sky's light back wherever a surface could not see the sky. The other added a small fixed amount of
light to **every surface in the world**, no conditions at all — a face sealed inside solid stone,
with no sun, no sky and no lamp, still got some. Both are deleted. What is left cannot invent light:
sun times how much of the sun the surface can see, plus sky times how much sky, plus lamps, plus the
measured bounce. All nought means black.

**The second, and it was the worse of the two.** Fog was a torch. The air was lit with full sunlight
and full skylight everywhere in the world, whether or not either could reach it — so a sealed room
with fog in it *glowed*, and the glow sits in front of the walls, so painting the walls black would
not have removed it. The air now gets told how much sun and sky actually reach that spot, and in a
sealed room that is nothing.

**Proof, not a promise.** There is now a test room — four metres of air inside two metres of stone,
no door, no window, no lamp — and a one-line tool that renders it and reports the brightest pixel:

```bash
.\tools\darkroom.ps1 -Fog
```

It reads **0 out of 255, every pixel**, clear and with fog. I added it because this is exactly the
kind of thing that quietly comes back: a small amount of fake light is invisible in a lit scene, and
those two numbers survived the entire rewrite without anything noticing.

**What it cost the building: nearly nothing.** The average pixel in the rotunda goes from 126.3 to
124.8 — because the light those numbers were faking is now light that is actually measured. What
changed is the bottom end: the darkest pixel indoors was stuck at 4.7 and is now 0.4.

## And then the thing that list was for: bounced light

The game has lit every interior with **one number**. Not a measurement — a constant, 0.5, standing in
for all the light that bounces off walls onto other walls, with a note beside it saying this stage
would replace it. It is replaced.

Each surface already fires one long ray into the room to ask "can I see the sky from here?". That ray
lands on something almost every time, and until now everything it hit was thrown away. It now brings
back **what that surface is giving off** — read straight from the surface's own record, which the
same pass filled in a moment earlier. So the light in a room is now light that came off the things in
the room.

**What you can see:**

- outdoors, the portico, the columns and the steps go from crushed black to legible. Sunlit stone
  throws far more light than the old constant pretended;
- indoors, the room has shape instead of a flat wash: the niches, the dome and the pilasters carry a
  gradient that was not there;
- it is **quieter**, not noisier — the speckle measure falls from 21.2 to 17.6 and the count of stray
  bright pixels from 81 to 9;
- and it bounces more than once for free, because what a surface gives off already includes what it
  gathered, so the light goes round the room.

**What it costs: about 5% of a frame.** That is the whole point of doing light on surfaces rather than
on pixels — the ray was already being fired, so the fourth kind of light in the game is nearly free.

**I predicted this would make rooms darker and I was wrong**, which is worth writing down: I expected
one bounce to be dimmer than a constant of 0.5. It is much brighter outdoors, because a sunlit wall
is far brighter than half of a sky.

**Three faults on the way, and the first is the one worth telling.** The number saying how big a
surface's light record is was written down in two files. The record grew and only one of the two was
updated — so the pass that draws read every surface's light out of a *different* surface's record.
The building came out black under dense black-and-white speckle, which looks exactly like "not enough
samples", and I spent two fixes chasing sample counts before reading the two files side by side.

**Still open: the fine grid you spotted.** I measured it and can tell you what it is not — not the
bounce (identical with it switched off), not the model, not the materials, not the lamps, not the
resolution once measured properly. What is left is that **every kind of light in this game is worked
out per surface from that surface's own random samples, and nothing smooths between neighbours** —
so two surfaces side by side disagree slightly, and a disagreement per surface on a grid of cubes is
a grid. That is exactly the job of the next stage in the plan, the denoiser. Your last clue — that it
fixes itself when you leave and come back — points at something narrower: a surface stops measuring
once it is confident, so surfaces that finished while the room was still filling with light keep the
darker answer they had. That one I can test.

## The lights while you fly were not slow — they were not being drawn

This is the one I would most want you to look at, because it is a bug that has been in every build
you have flown around in, and it was hiding as a *good* number.

The graphics card keeps its own copy of the lighting table. When you move, that copy changes fast —
surfaces appear, others are thrown away — and the game sends the changes across a few megabytes at a
time. If a frame ran out of room mid-send, it **threw away everything it had just sent and started
the whole list again next frame**. Moving fast, it never got to the end. The card's copy fell up to
**434,838 surfaces behind** and stayed there.

What that did is worse than being out of date: pixels could no longer find their own surface in the
card's copy, so the game gave up on them and drew each one with a **throwaway coarse guess, made
fresh every single frame** — 8,255 of them per frame. That is the blocky, flickering light you have
reported more than once. Same picture, third different cause.

It now sends what fits and keeps the rest for next frame. The card's copy is **0 surfaces behind**,
and it runs out of room on **1 frame in 400 instead of 253**.

**What you will see while flying:** balustrades, window reveals and cornices that used to break into
hard black and white blocks now stay lit stone. Nearly three quarters of the screen changes. Stray
bright pixels drop from 2,720 to 944.

**And it costs, honestly.** The lighting pass goes from 2.0 ms to about 7 ms while flying, which is
over its budget. None of that is new work — it is the work that was being skipped. The old number was
cheap because the lights were not being worked out at all. Making that fast again is the denoiser's
job, which is the next stage in the plan.

**And one measurement that is bigger than the change that found it.** While hunting the above I
finally got the game to report, while you are *moving*, how far the graphics card's copy of the
lighting table is behind ours. The answer is **up to 434,838 surfaces**: we throw a surface away, the
card is told in the next upload, the upload runs out of room, and — this is the fault — it then
throws away *all* its progress and starts the whole list again next frame. So the card goes on
lighting hundreds of thousands of surfaces we abandoned long ago, and that is what the lighting pass
is actually spending its time on while you fly. Fixing that is now the biggest single number on the
list for movement, and it has nothing to do with lighting at all.

## "Where I stood still looks better and brighter than everything else"

You are right, and it was the light bouncing off surfaces — the thing that went in two changes ago.

**What was happening.** Every surface works out how much light is bouncing onto it by firing rays
into the room and asking whatever they land on how bright it is. But *that* surface is doing the same
thing, and when a room first appears everything in it starts black and brightens over the next
half-minute as the light goes round and round. So the answer a surface gets depends on **when** it
asked.

The mistake was in how it kept the answer. It averaged **every ray it had ever fired, for ever**. So
a surface that had been asking for ten seconds carried ten seconds of the room being too dark in its
average, and a surface that had been asking for a minute carried a minute of it. Two things follow,
and they are exactly what you saw:

- the longer you stand looking at something, the brighter it correctly becomes — and everything you
  have not been looking at is still somewhere earlier on that climb;
- once a surface decides it has enough rays, it stops firing and **keeps whatever average it had, for
  the rest of that surface's life**. So the difference does not go away on its own.

I can show you the size of it without a screenshot. Standing perfectly still at one camera in a
settled world, doing nothing, the average brightness of the whole frame goes **131.3 → 131.8 → 132.6
→ 132.7** at 2.5, 5, 15 and 45 seconds. Nothing in the world changed. That climb *is* the bug: it is
your dwell time being painted into the picture.

**The fix.** A surface now remembers its **last 128 rays** instead of all of them. That is one line,
it uses the same memory, the same number of rays, and the same three numbers in the record — but it
means every surface is showing you the room *as it is now* rather than an average of how it got here.
And because a surface that stops has frozen whatever it holds, it now has to fire four times as many
rays before it is allowed to stop, so it stops on the settled answer instead of a half-lit one.

**What you will see.** Indoors, where all of this lives:

- the room is **3.6 of 255 brighter** — that is light that was always there and was being averaged
  away;
- it is also **18% less speckly**, which I did not expect and is the better half of the result: those
  extra rays also sharpen how much sky each surface can see;
- outdoors nothing changes at all, and that is the proof the diagnosis was right. Outdoors a ray
  escapes to the sky on its first try, so there was never a fill-up to average over.

**What it costs.** Nothing while you are moving — I measured the flight twice each way and the two
versions sit inside each other's spread, and it cannot cost anything there because no surface lives
long enough to reach the point where this changes. Nothing in the first ten seconds of standing
still. What it does cost is that when you stand still, the lighting goes completely quiet after about
forty seconds instead of ten, so between those two points the lighting pass costs 1.4 ms instead of
0.6 ms. Its budget is 4.4 ms, so there is plenty of room, but I would rather tell you than have you
find it.

**Half of what you said is still open, and it is a different thing.** "Better rendered" is also just
sample count: a surface you have only just turned towards has fired **46 shadow rays against 203**, so
its shadow really is coarser for about fifteen seconds before it catches up exactly. That is waiting,
not a wrong answer, and the stage that fixes waiting is the denoiser — the one that lets neighbouring
surfaces share what they have measured instead of each one finding out for itself. It is next in the
plan.

## The light of a room was the first thing thrown away when you left it

This one is the change I would most like you to go and look for, because it is about walking, not
about standing still.

**The setup, in one paragraph.** Every surface in the building has its own record of the light that
reaches it. There are a *lot* of those — about seven hundred thousand of them from one camera — so
the game also keeps a **coarse** record for every 8×8 patch: one of those stands in for 512 of the
fine ones. The coarse one is what a surface reads while its own record is still being worked out, and
it is what every new fine record starts from. It is claimed the first time the game notices a new
surface, and after that nothing ever asks for it again — because everything under it now has its own
answer.

**And that is the bug.** The game gives up any record "nobody has asked for in ten seconds". Nothing
ever asks for a coarse one. So the coarse records were the **first** thing thrown away, always, while
every fine record under them was still perfectly alive. Then you walk away, the fine ones go too, and
when you come back the room has nothing at all to rebuild itself from: no fine light, and no coarse
light either.

I did not have to argue this from theory. The game now prints how many coarse records it is holding,
and standing at the steps of the facility it read **zero of seven hundred and eleven thousand** —
having thrown away every single one of the 21,796 it had ever made.

**What it fell back on instead is the expensive thing.** When a surface has no light record at all,
the graphics card invents a temporary one for that frame — and a temporary one cannot remember
anything, so it fires a fresh ray every frame and throws the answer away. One ray per patch per frame
is exactly the blocky, flickering light you have reported more than once.

**The fix is one sentence: a coarse record is only given up when the table is actually short of
room**, never merely for being old. The whole coarse set is about 3% of the store, and it is what
everything else is built from, so it is the cheapest thing in there to keep.

**And the second half:** when a light ray lands on a surface whose record is missing, it used to
count that surface as **black** — as though a lit wall were emitting nothing. It now reads the coarse
record over it. That is a real answer measured at 25 cm instead of a guess at 3 cm, and it is a great
deal better than black.

**What you should see.**

- **Walk out of a room and back in.** Three frames after you look back, the number of those expensive
  temporary records goes from **3,137 to 99**, and the picture is measurably less speckly (34.4 → 19.6
  on the scale I have been quoting you, with the "fireflies" — single bright wrong pixels — going
  1,494 → 387). In plain terms: the room comes back lit instead of coming back blotchy.
- **Standing at the steps, the building is brighter and cleaner.** Average brightness **133.5 →
  140.0** and speckle **45.5 → 38.5**. That brightness is not invented: it is light that rays were
  finding and then reporting as black.
- **Indoors it is a smaller version of the same** (126.4 → 127.6, and four times fewer fireflies), and
  **outdoors nothing changes at all** — 0.2 of 255, which is nothing. That is the proof the diagnosis
  was right: outdoors a ray escapes to the sky and never needs anybody's record.

**What it costs.** Nothing I can measure. The 21-camera measurement grid moved **+0.17%** in total,
which is far inside the noise, and the speckle across the whole grid went **down 6.9%**. Flying, the
two versions sit inside each other's spread.

**And a mistake I made on the way, because it is the useful part.** My first version quietly made the
light converge four times more slowly, and every timing said it was free. The sun's ray budget is
shared out among the surfaces that want one, and I had just added 21,799 coarse records to that queue
— records that never want a ray at all. So every real surface got fewer. The clock said 1.55 ms
against 1.56 ms and the *convergence* said 107,582 surfaces finished against 475,632. This is the
third time that exact mistake has been made in this project, so the rule is now written down twice: a
number that got faster with nothing to show for it has usually just stopped doing some of its work.

**What is still missing, honestly.** The game can now count what its light rays land on, and about a
third of them still land on something with no record at all — surfaces off to the side of the screen,
or behind you. That is the next piece of work in this stage, and I now have a number for it instead of
an opinion.

## The light was being collected from surfaces that had never been lit

**This is the piece I said was next, and the shape of it turned out to be one word different from
what I expected — worth telling, because the correction is the useful part.**

Two changes ago the game started keeping a lighting record for surfaces you cannot see: when a light
ray lands on something off to the side of the screen, or behind you, the game now remembers that
surface. The idea is that a room is lit by the whole room, not by the part of it that happens to be in
front of your eyes.

**What I found when I looked at the counters is that it kept a quarter of a million of those records
and never put anything in a single one of them.** There is a rule in the lighting pass that says a
surface only gets rays spent on it if somebody is looking at it — that rule is what made the lights
2.1x faster while flying, and it is a good rule. But it asks the wrong question for these records.
Nobody is *looking* at them, by definition. Something is *reading* them: the light rays that asked for
them in the first place. So the game claimed them, refused to light them, and then read the emptiness
back out. **One light ray in eight was collecting light from a surface it had itself asked for and
which had never been lit.**

**What I was about to build instead.** My own notes said the next job was to widen the net slightly —
to start keeping surfaces just off the edge of the screen, so that turning around finds them already
lit. That would have added *more* of exactly these empty records and changed nothing you could see. I
found this out by reading one line of the game's own log before starting, which took about five
minutes and saved a session.

**The fix** is that a surface now counts as worth lighting if a light ray is reading it, not only if
your eye is. It gets its own separate ration of rays, so it can never slow down the surfaces you are
actually looking at — that mistake has now been made three times in this project and the ration is
what stops it happening a fourth.

**What it looks like.** Stand in the domed room and look around:

- the room is **127.5 → 150.2 of 255 brighter** — and that is the honest kind of brighter: it is
  light bouncing off surfaces that were being counted as pure black;
- the speckle goes **16.1 → 12.8** and the stray bright dots go from 9 to **none**;
- outside the building, nothing changes at all (161.75 against 161.83), which is exactly right —
  outdoors most light rays reach the sky and there was never anything missing;
- standing at the steps is in between: 139.8 → 143.1, with the stray dots 108 → 27.

**The more enclosed you are, the more it matters**, which is the whole point of this stage.

**The safety check still passes.** The sealed pitch-black room test is still pure black in every
pixel, with and without fog, so nothing here is inventing light — it is collecting light that was
already measured and then thrown away.

**What it costs, and this is the part you may want to overrule me on.** Standing still in the domed
room it costs about 1.2 ms while the new surfaces fill in, and about 0.2 ms once they have settled
(around forty seconds). **While flying it costs 1.2 ms and buys almost nothing** — one percent more
of the light rays find something, against eleven percent when you stand still. That is because when
you fly, most rays go to the sky anyway and no off-screen surface lives long enough to be measured
before you have moved on.

So: it is a big win where you stop to build and look, and close to a pure cost while you travel. It is
on by default and it is one switch away if you would rather have the frames. Tell me which you prefer
after you have played with it.

**One honest loose end.** I could not explain the shape of the cost. Lighting **35** of these surfaces
costs 0.85 ms; lighting another 2,335 on top costs only 0.94 ms more. So the price is almost all
"doing this at all" rather than "doing a lot of it", which means turning the ration down saves very
little and gives up most of the benefit. I tried three explanations and measured all three away. The
one that fits the numbers is that the whole pass has to wait for its slowest piece of work, and one
surface being lit for the first time is slow enough to set that floor on its own — but I have not
proved it, and I have written down exactly what would.

**And one thing this makes worse rather than better:** the game still has no automatic exposure. The
brightness dial is a fixed number, and now that interiors are genuinely brighter, that fixed number is
doing less of a good job. That is its own piece of work.

## The room's light was rationed to a quarter of the store while three quarters of it sat empty

**Short version: indoors is brighter and smoother again, and this time nothing new was built — a
limit that was set to the wrong thing was set to the right thing.**

The section above added surfaces you cannot see to the lighting, so light can bounce off them. There
is a table that holds all these surfaces, and to stop the invisible ones crowding out the ones you
are actually looking at, the invisible ones were rationed to **a quarter of the table**.

A quarter sounds sensible and it is a fraction of the wrong thing. What the invisible surfaces can
safely have is **whatever the visible ones are not using**, and that is a wildly different number
depending on where you stand. In the domed room you can only see about 111,000 surfaces out of a
million slots — so the ration was turning away light while **three quarters of the table sat empty**.
Over one settled run in that room it refused **222,587 requests** for no reason at all.

The ration is now "everything the visible surfaces are not using, minus a safety margin", and the
safety margin is the same one the table already keeps for itself. So the two rules are one rule, and
when you fly — where the visible set is nearly the whole table — the ration shrinks by itself without
anything having to notice you are moving.

**What it looks like.** Stand in the domed room and look around:

- the room goes **150.1 → 157.4 of 255** brighter, on top of the 127.5 → 150.2 from the section above;
- the speckle goes 12.83 → **12.17**, and there are no stray bright dots in either version;
- at the steps outside, a small improvement: 143.09 → 143.89, speckle 35.85 → 35.25;
- from far back outside the building, **nothing at all** (161.821 against 161.822) — which is right,
  because outdoors most light rays reach the sky and nothing was ever missing.

**It costs nothing.** The lighting pass reads 2.613 ms against 2.652 in the room and 3.529 against
3.560 at the steps — the same number twice. Flying, the two versions land inside each other's normal
run-to-run wobble. The sealed pitch-black room is still pure black in every pixel with and without
fog, so this is still light that was measured and thrown away rather than light invented.

### The half of this that was a real bug, and I found it by doing the change wrong first

Letting the invisible surfaces grow means the table runs closer to full. And when the table gets
tight it gives things up — and it was giving them up **in the wrong order**.

There are three kinds of thing in that table and they are not worth the same:

- a surface **you are looking at**. Losing one means a wall in front of you with no light of its own,
  which is the blocky flickering you reported months ago;
- a surface **only a light ray has ever asked about**. Losing one costs a single bounce sample, and
  there is usually a coarser stand-in above it that answers instead;
- a **coarse stand-in** — one summary for every 512 fine surfaces, which is what a whole room is
  rebuilt from when you walk back into it. It is **3% of the table** and it answers **31%** of the
  light rays that would otherwise find nothing.

All three were on one clock. So when I first made the ration bigger and measured it, the coarse
stand-ins were the thing that paid: they went from **21,795 down to 62**, and the picture at the steps
came out very slightly *worse* even though the number I was aiming at had halved. That is the useful
part of this whole session — a measurement improved and the picture did not, because the things that
paid for it were each worth more than the things that replaced them.

They now go in order: the invisible surfaces first, then old surfaces nobody has looked at for a
while, and the coarse stand-ins last of all.

### What I measured and deliberately did not do

With the ration fixed, the thing now holding the invisible surfaces back is **how often a light ray
is allowed to report what it hit** — once every 64 frames per surface. I tried 32 and 16:

- at **32**, the domed room gets another 5.6 of 255 brighter and the speckle drops further — but the
  stray bright dots go from **none to eighteen**;
- at **16**, brighter again, and the dots go to **eighty-one**.

A stray bright dot is the one rendering fault that an average cannot see and your eye catches
immediately, and going from zero of them to eighteen for a brightness gain is not a trade I wanted to
make on your behalf. **The proper answer is the smoothing pass (R5)** that this project has owed for
several sections now, which would let me take the brightness without the dots. The dial is one switch
if you want to try it — tell me and I will show you both.

**And one thing I could not explain, written down rather than glossed over.** While you *fly*, about
22,000 coarse stand-ins are still being thrown away over a flight, in both the old and the new
version — so something other than the ordinary give-up rule is spending them there. This change did
not cause it and did not fix it. It is the next thing I want to find in this pass.

## Each surface used to work out its own lighting alone. Now it asks its neighbours

**This is the smoothing pass I have been promising for several sections, and it is the fix for the
fine grid you reported on flat walls.**

Every kind of light in this game — sun, sky, bounced light, lamps — is worked out separately on each
voxel face by firing rays and averaging what they find. That is what makes it cheap and what makes it
scale. It also means **every face's answer is slightly wrong in its own private way**, and on a flat
wall, where all the faces should agree, that disagreement is the only thing there is to look at. Your
words for it were "subtle horizontal lines on everything", then "it seems to be a grid". I spent a
whole session eliminating eight specific causes and found no ninth, because there wasn't one — this
was the cause, and it needed a filter rather than a bug fix.

**Now each face blends its answer with the eight faces around it**, weighted by how well measured
each of them is.

**The piece I am pleased with is why it costs almost nothing.** A normal smoothing pass over a screen
spends most of its work deciding which neighbouring pixels are even on the same surface — is that the
same wall, or is it the floor behind it? Here that question cannot come up. A face is identified by
which voxel it is on, which direction it faces and how big it is; so a face one step sideways at the
same size facing the same way is *guaranteed* to be on the same flat plane, right next to it. If it
isn't, there is simply nothing there to find. The engine's own way of naming faces does the hard part
for free.

**What it looks like.** Stand at the steps, or in the domed room, and look at a large flat surface:

- face-to-face roughness — the number that measures exactly this kind of stepping — falls **4.35 →
  3.14 of 255** at the steps and 3.02 → 2.45 in the room;
- the general speckle falls **35.3 → 28.7** and **12.1 → 10.0**;
- the overall brightness moves by **0.02 to 0.10 of 255**, which is nothing — the picture gets
  smoother without getting lighter or darker, which is what a smoothing pass should do and is the
  main thing I checked;
- walk out of a room and back and it is better there too: roughness 3.22 → 2.69, speckle 13.2 → 10.8.
  A face that has just been revealed borrows its neighbours' well-measured answer instead of starting
  from its own two or three rays.

**What it costs.** Standing still, about a third of a millisecond on the lighting pass at the steps
and an eighth of one in the room. **While flying it is free** — the two versions land inside each
other's normal wobble. And 16.5 MB of graphics memory, because each face now stores four extra
numbers.

**Two things I deliberately did not smooth.** The **sun's shadows**, because a shadow edge is
supposed to be sharp and smearing it would be a real loss. And the **near-field shading** — the soft
darkening in creases and corners — because I photographed each kind of light on its own first and
that one is already the smoothest thing in the renderer. That photograph also told me what to do
next: **the lamps are by far the noisiest indoors** and they are not smoothed yet.

### Two ways this could have looked like a win and not been one, and how I checked

**First: the stray-bright-dot count went UP, and it was the ruler, not the picture.** That count is
defined as "a pixel more than four times as bright as its neighbours". Smooth the neighbours and the
bar drops, so the same dot starts counting without a single photon being added. I checked it against
fixed brightness levels instead, which cannot move for that reason: pixels over 250 of 255 went 902 →
892, pixels over 254 stayed at exactly 391, and **in the domed room the single brightest pixel in the
frame got dimmer, 235 → 233, while the dot count doubled**. Nothing got brighter anywhere.

**Second: a smoothing pass can always improve a smoothness score by destroying the picture.** So I
measured the sharp edges separately from the flat areas. The number of sharp edges falls 4–6% while
their average sharpness holds steady — and indoors it goes slightly *up*. What disappeared was noise
spikes that had been counting as edges, not the real ones. If I had been smearing the building, the
edges would have got weaker as well as fewer, and they did not.

### And the thing I said last time I would come back to

Last section I measured a switch that makes interiors brighter and said no to it because it took the
domed room from no stray dots to eighteen — and that the smoothing pass was the real answer. That has
now come true: with the smoothing in, the same switch makes the room brighter *and* smoother
(brightness 157.4 → 163.1, speckle 9.97 → 9.44, roughness 2.45 → 2.28).

I have still not turned it on, for one reason: at the steps it takes the lighting pass to 4.11 ms
against a 4.40 budget while standing still, and that pass is already at 7–8 ms while flying. It needs
its own measurement of the flying case before I change a default that affects every frame. That is a
short piece of work and it is the next thing I would do unless you want something else first.

## And then the lamps, which turned out to be the biggest one

**The domed room is now nearly half as rough as it was this morning, and most of that is the lamps.**

When I built the smoothing pass I photographed each kind of light on its own first, so that I picked
what to smooth from a measurement instead of a hunch. That photograph said something I would not have
guessed: indoors, **the lamps are by far the noisiest thing in the picture** — three or four times
noisier than the soft shading in creases and corners. So they were the next thing to smooth, and they
were worth more than the two I did first.

**Why lamp light is noisy at all**, since it is not the slow kind: when a face works out how much
lamplight reaches it, it does **not** look at every lamp in the building. It picks *one*, at random,
weighted towards the ones likely to matter, and corrects for having picked that one. That is the trick
that makes a hall with a thousand sconces cost exactly what a hall with one costs — and the price of
it is that each face's answer wobbles until it has taken a few hundred picks. The wobble is per face,
which is exactly the shape the neighbour blend removes.

**What it looks like.** Stand in the domed room:

| | before any smoothing | sky and bounce | ...and the lamps |
|---|---|---|---|
| roughness | 3.01 | 2.45 | **1.72** |
| speckle | 12.11 | 9.98 | **7.99** |
| brightness | 157.49 | 157.50 | 157.45 |

Roughness down **43%** and speckle down **35%** from where it started, with the brightness moving by
five hundredths of a shade. At the steps outside: roughness 4.35 → 2.97, speckle 35.20 → 27.53.
Outdoors it adds almost nothing, correctly — the portico is the only lamp-lit surface out there.

**The same two sanity checks as before, and they both speak louder this time.** The stray-dot count
went 18 → 45 — and the room's brightest pixel got *dimmer* (233 → 232) while the number of pixels
over 230 nearly halved, 2,764 → 1,463. The background got so much smoother that the "four times your
neighbours" bar dropped under a lot of perfectly ordinary pixels. And the sharp edges: their count
falls 12% while their average sharpness goes **up** 4.5%, which can only happen if what left the
count was noise and not the building.

**One honest loss.** A sconce's light stopping sharply on a flat floor is a real edge, and about
9.4 cm of it is now softened. Where the surface turns a corner nothing is blended at all, so the
architecture's own edges are untouched — this only affects a hard lamp-shadow line drawn across one
continuous flat face.

**What it costs.** Nothing while flying — the two versions land inside each other's normal wobble.
Standing at the steps the lighting pass goes from about 3.5 ms to 4.06, against a budget of 4.40, so
that camera now has about 8% of headroom left standing still. And 29.6 MB of graphics memory for the
whole smoothing pass, which is now the biggest single thing the renderer allocates. Both of those are
worth knowing before I add anything else to that pass, and I have written them down where the next
piece of work will trip over them.

## The brightness dial had nobody turning it

**The game now sets its own exposure, the way an eye or a camera does. Two test scenes that were
unusable are usable.**

There is one number that decides how bright the final picture is. It has been **3.2, fixed, forever**
— and it was supposed to be temporary. The old path tracer measured the frame and set it properly;
when I deleted the tracer (and then the buffer it measured into) that measurement went with it and
nobody noticed, because the facility happens to look about right at 3.2.

Two test scenes were not so lucky, and both were written specifically to test this:

- **the sealed hall lit by thirty-six sconces** — I have mentioned this one several times as "comes
  out blown white". It was: the average pixel was **248.9 of 255**, and **99.9% of the frame** was
  brighter than 200. It is now **150.6**, with nothing at all fully blown;
- **a room with one window in it** — the opposite failure. The sky through the window took all the
  range and the room went black: average pixel **35.8 of 255**. It is now **149.3**. That clip's own
  notes predicted exactly that would happen, years before anything could fix it.

The meter chose a multiplier of **0.187×** for the first and **33.2×** for the second. Same renderer,
same code, a factor of 177 between them — which is what having a meter means.

### The part I want you to check, because it is a taste decision

Measuring the facility "properly" — to photographic middle grey, the textbook answer — takes it from
where you have been looking at it (around 144–162) down to **98–100 on every camera**. That is
technically correct and it looks wrong: a light meter averages, and this building is mostly shadowed
stone, so the average is far below what your eye is actually looking at. Every photograph of a snowy
field has the same problem.

So there are two numbers, not one: the meter measures, and a separate **+1.3 stops of compensation**
says what to do about it. I picked 1.3 by measuring what each of the three cameras would need to stay
exactly where it is (1.40, 1.11 and 1.44) and taking the middle. The result:

| | before | after |
|---|---|---|
| domed room | 157.4 | 153.6 |
| the steps | 144.1 | 152.1 |
| outdoors | 161.9 | 155.5 |

So the building shifts by 2–6% — the steps come up, the other two come down slightly, and all three
now sit within two shades of each other, which is the meter levelling them. **If that reads as
"everything went slightly flat" to you, the fix is one number and I will change it.** The two
constants are separate precisely so that adjusting the look cannot quietly corrupt the measurement.

### The things I checked because they are what would go wrong

- **It does not pump.** An auto-exposure that hunts is far worse than a fixed one. Standing still,
  two consecutive frames chose 2.991× and 2.991× — identical to three decimal places.
- **It adapts at about the speed of an eye.** Cutting from outdoors into the room moves it from
  2.899× to 4.014× within five frames and settles over about half a second.
- **A pitch-black sealed room is still pure black**, with and without fog. Exposure multiplies, so
  nought stays nought however far the dial winds up — and in that room it winds up to **460×**, which
  means anything the renderer was inventing would now be 460 times more obvious. It is still nought.
- **It costs nothing measurable**: the final pass reads 0.820 / 0.728 / 0.555 ms against 0.819 /
  0.729 / 0.559 before.
- **The picture is printed as a number now**, at every screenshot: what multiplier it chose and how
  bright the frame was. A brightness dial nobody could read is what caused this in the first place.

`--no-auto-exposure` puts the fixed 3.2 back exactly, so every screenshot I have ever sent you is
still reachable.

### One correction to what I told you last time

I said the face light record was "the largest allocation in the renderer". It is not — there is a
950 MB buffer in there labelled **TEMPORARY PROBE**, left over from an experiment about memory
placement that finished several sessions ago, and it is allocated on every launch. I have flagged it
as its own piece of work rather than pulling it into this one, because removing it has to be measured
(the whole reason it exists is that memory placement was suspected of changing frame times).

## The speckles were mine, and you diagnosed them

**"3×3 voxel faces, where the central pixel is properly coloured" is not a symptom — it is a
description of the smoothing kernel I added two changes ago.** It told me exactly where to look, and
it was right.

**What I got wrong.** I argued that this smoothing needed no test for whether a neighbouring face
"belongs" — because two faces on the same flat plane, side by side, facing the same way, are
*guaranteed* to be the same surface. That part is true and it is still the reason this is cheap. What
does not follow, and what I wrote down as though it did, is that it needed **no test at all**. A flat
wall can carry a real edge across it: where a shadow stops, where an alcove stops shading it, where
the last of a lamp's light reaches. Blending across one of those is not smoothing, it is smearing.

So every lit face was lending light to the eight faces around it. The lit face itself still looked
right, because its own answer dominates its own average — which is exactly the correct centre you
saw. The ring around it is what moved. In a bright room you would never notice; in a dark one that
borrowed light is the only thing there is to see, and on newly revealed geometry every face is
disagreeing with its neighbours at once, which is why it appears when you turn and then goes.

**The fix**, and the balance in it is the whole point: a neighbour now counts in proportion to how
much it *agrees* with the face doing the smoothing — and how hard that test bites depends on how well
that face knows its own answer. A face that has just appeared and has almost no measurements of its
own still takes its neighbours whole, which is the thing that stops new surfaces looking coarse. A
face that has measured itself for hundreds of samples refuses a neighbour four times brighter,
because that is a neighbour on the other side of something.

**The measurement.** I count sharp edges separately from flat areas, which I built for exactly this
question two changes ago:

| domed room | no smoothing | smoothing as you saw it | with the agreement test |
|---|---|---|---|
| sharp edges surviving | 91,707 | **79,781** | **87,883** |
| roughness | 3.03 | 1.75 | 1.86 |
| speckle | 12.21 | 8.10 | 8.92 |
| stray bright dots | 0 | 9 | **0** |

**13% of the room's real lighting edges were being destroyed; 4.2% are now** — and I keep 90% of the
smoothing. It costs nothing while flying and nothing standing still.

**One thing I have to be straight about: I could not measure the thing you actually saw.** The
transient is not reproducible — three runs of the *same* build, same camera, same frame, gave mean
brightness 70.0, 82.3 and 84.9. Anything I "measured" there was noise, and I threw away three tables
of figures once I checked. So this change rests on the mechanism being provably wrong and provably
fixed on a settled picture, not on a photograph of the moment you described. **You are the test.** If
it is still there, tell me and `--denoise-edge 0` puts the old behaviour back for comparison, or
`--no-face-denoise` turns the whole thing off.

## And a ceiling on the exposure, so darkness can be dark

You asked for a floor so that at some point darkness is pitch black. A light meter has no opinion
about absolute brightness — it makes *every* scene average to the same grey — so without a limit
there is no such thing as a dark room. A sealed unlit corner of the sconce hall was winding the
meter to **429×** and reading as a lit room with nothing lighting it.

That is the same fault I removed two sessions ago when I deleted the two constants that added a
little light to every surface — except that an exposure with no ceiling puts the floor *above* the
whole picture instead of under each surface, so no amount of black paint reaches it.

The limit is **64×**, and it is measured rather than picked: the room-with-a-window scene genuinely
needs 33×, so the ceiling has to clear that, and anything past it is a room with less light in it
than one window — which should go dark.

| | before | after |
|---|---|---|
| the dark corner: what the meter chose | 436× | **64×** |
| ...average pixel | 35.6 | **10.2** |
| ...pixels brighter than 200 | 30,104 | **1,020** (the sconces themselves) |
| the window room: average pixel | 149.4 | **149.3** — untouched |

`--exposure-max N` moves it if 64 is not where you want the line: smaller lets more of the dark stay
dark, larger recovers more of it.

## Reflections: the first piece, which you cannot see yet

You said directional faces, not the halo, so that is what I started. It is the half of what you asked
for at the very beginning — *"everything is per voxel face based, even reflections and those
things"* — that the rewrite has not done at all.

**Where it stands today: nothing in the game reflects anything.** Every surface is treated as though
it were chalk. The bronze doors, the gilded rosettes on them, the copper dome, the glass in the
windows and the water are all drawn as flat coloured paint, and they always have been in this
renderer — the old path tracer had reflections and it is deleted.

**What landed is the thing that was in the way, and it is duller than it sounds.** A face in this
engine knows *where* it is and *which way it points*, and that is all it has ever known. The lighting
pass could not find out what the surface under it was **made of** — it has never had access to the
table that holds that. It did not matter while everything was chalk: to light chalk you need a
colour, and it had one. To draw a reflection you need two more numbers, how rough the surface is and
how metallic, and it could not reach them.

Now each face works that out once, the first time it does any work, and remembers it. It costs
nothing I can measure — I ran it against itself with the feature switched off, four runs alternating,
and the two are inside each other's spread on both the standing-still case and the flying one.

**You can look at it if you want**, with `--debug-mode 21`: red is metal, green is smooth, so the
front doors and the rosettes light up orange and the window glass goes bright green while the
limestone stays black. That view is the only way to check the fact actually reached the lighting
pass, which is why it exists before anything uses it.

**One thing worth knowing, because you will spot it eventually.** This works for surfaces near you.
Far away the renderer draws one face for a block of many voxels, and a block of many voxels has no
single roughness — so a distant dome will still look like chalk after the next step, and making it
not do that is a separate piece of work I have not started.

**And the answer to your question: yes, it is fully open-ended.** Roughness and metalness are plain
numbers on each voxel, anything from 0 to 255, set in the clip file per material — and the sampler
already nudges them slightly per voxel so that no two voxels of "the same" stone are identical. There
is no built-in list of materials anywhere in the engine, and nothing I am building looks up a
material by name or picks from a set. Everything reads those numbers as quantities: how wide a
reflection is, how tinted it is, whether the surface has one worth storing at all. I have written
that into the plan as a rule for the rest of this stage, because the easy mistake here is to write
"if the surface is shiny do this, otherwise do that", and the moment anybody does that your dial
stops being a dial. (What does *not* exist yet is a way to set those numbers from inside the game —
today they come from the clip file. Say the word if you want that and I will add it.)

**What is next, and this is the one you will see.** Each face measures what it reflects, in a small
set of directions, and the picture reads whichever direction your eye is looking down. Then the
doors, the dome, the gilding and the glass stop being paint: they show the portico and the sky, and
what they show changes as you walk past. If it goes wrong you will see the reflection *swim* or
crawl as you move instead of sitting still on the surface, or a metal surface going black — tell me
either of those and I will know where to look.

## The metals stop being chalk — and you were right, there is still no reflection

**The first thing you said when I showed you the pictures was "I don't see any reflection in any
picture", and that is correct.** I want to put what I actually delivered and what I did not in front
of you plainly, because the previous section promised you the portico in the bronze doors and that
is not what happened.

### What did land, and you can see it

Every surface in this game has been drawn as chalk with a colour on it: one brightness, the same
whichever way you look at it. That is what a wall does. It is not what metal does — metal sends
almost nothing back diffusely and almost everything back in a *lobe*, a cone pointing away from you
like a bounce off a snooker cushion, narrow if it is polished and wide if it is brushed.

That split is now in, and it is driven by the metalness number on each voxel rather than by any list
of materials. So:

- the great bronze door reads as **darker, deeper bronze with shading across the panels**, instead of
  a flat pale brown;
- the gilt rosettes read as **gold discs** instead of pale blobs;
- the bronze glazing bars in the windows, the lead, the copper — the same;
- limestone, plaster and stucco look exactly as they did, which is the point;
- a metal in **direct sun** now gets a proper highlight that slides across it as you move. That one
  costs nothing at all to store, because the face has already measured how much of the sun it can see.

It costs no frame time I can measure. I ran the feature against itself, on and off, alternating, at
three cameras standing still, flying, and flying while chiselling — every pair is inside the other's
run-to-run spread. 525 tests pass.

### What did not land, and why — this is the part worth your time

There is no reflected **image**. No portico in the door, no building in the water. I found three
reasons, and I want to separate them because only one of them was my choice:

1. **I stored too few directions.** Sixteen, which smears anything reflected across 20 degrees. You
   cannot see a building through that.
2. **The metals on this building are genuinely rough, and should be.** The bronze is specified at a
   roughness that spreads a reflection over about 10 degrees, and the copper and lead over 24 and 25.
   A brushed bronze door does not mirror a portico in real life either. For the copper and lead, my
   sixteen directions are already *finer* than the material needs — the blur is not what is missing.
3. **The two genuinely polished things in the building — the window glass and the water — were shut
   out.** I had a rule deciding which surfaces are worth storing a reflection for, and glass and
   water fell just under it.

So I ran the experiment: **sixteen times more directions**, glass and water let in, camera down at
the water's edge looking across the basin at a grazing angle, which is where a reflection is
strongest. **Still nothing.** And that failure is the useful thing I got out of today, because it
names the real limit:

> **A face fills its reflection map with the same rays it already casts to gather light — about five
> hundred of them.** Sixteen directions get thirty or so rays each, which is enough. Two hundred and
> fifty-six get two each, which is noise. More directions costs noise, one for one, unless the face
> also gets more rays.
>
> **And those rays are aimed mostly straight out from the surface, while a reflection is what you see
> at a glancing angle.** So the directions a visible reflection would come out of are exactly the
> ones with almost no rays in them. Water seen across a pool is the case where a reflection matters
> most and the case my sampling serves worst.

I reverted the experiment. What is on the branch is the sizing the ray budget actually supports.

### So what does a real reflection cost, and is it worth it

It is not two constants. It is: **give a face more directions when it fills more of your screen** —
which was already the next step in the plan — **and give those faces a second ray aimed along the
reflection**, which was not. That second ray is the new part and it is a real cost.

The good news is how few surfaces need it. Of the 416,000 faces your screen is reading at the steps,
**22,158 carry any metal at all** — one in nineteen. So it is one extra ray on one face in nineteen,
not on everything.

**What I would like from you before I spend a session on it**: is a reflection you can *recognise*
worth an extra ray on the shiny surfaces, or would you rather I put that session somewhere else? I
have been wrong about what you would care about before, and this is a big enough piece to be worth
asking rather than assuming.

**If you want to poke at what is there now**: `--debug-mode 23` draws the reflection on its own with
everything else stripped out, which is the only way to see it at all, and `--debug-mode 22` shows
which surfaces are storing one — green yes, red asked and there was no room, dark grey not worth it.

## The second ray — and now there is something in the metal

You said to go ahead with it, so I did.

**What was wrong.** A face filled its little map of what it reflects using the rays it was already
casting to gather light — and those aim mostly straight out from the surface, while a reflection is
what you see at a glancing angle. So the directions a reflection actually comes out of were the
emptiest ones a face had. That was the finding from last time, and this is the fix.

**What it does now.** A face that stores a reflection casts a ray of its own, aimed *into* the cone
it is trying to fill, one direction at a time, until all thirty-six are measured. Then it stops for
good.

**What you should see.** The great bronze doors read as deep metal with the panels and the gilt
bosses standing out, where before they were a flat warm wash. The window glass has a pale sky-
coloured sheen on it. Against the same build with the ray switched off, that is a change on about a
hundred thousand pixels of the door view.

**What it costs.** Standing still: nothing I can measure — 4.229 ms against 4.218 with the ray off.
Flying: 1.5 ms, on a pass that is over its budget while flying either way. 527 tests pass.

### Three things I had to get wrong first, and I want to record them because two are interesting

1. **I gave each face a big burst of rays and it was far too expensive while flying** — 11.9 ms
   against 7.8. The odd part is that I had a measurement from earlier in this project saying the
   opposite: for the *shadow-and-shading* rays, spreading a burst out made things worse, not better.
   It turns out that finding does not carry, for two reasons. Those rays stop after a metre; these
   ones travel until they hit something. And that population converges and empties, whereas flying
   keeps supplying new faces for ever, so there is no state to get out of — only a rate to pay. Cut
   to eight rays a visit it costs a quarter as much, and a reflection fades in over about nine
   seconds instead of two.
2. **Two faces were stealing the same storage slot from each other for ever.** Taking a slot wipes
   what is in it, so both kept starting over and neither ever finished — 417 slots changing hands
   every frame. A slot is only taken now if the new face needs it half again as much. This one was
   only findable because I had put a counter on it, and I had put a counter on it because a full
   store and a store fighting with itself look exactly the same from outside.
3. **Two-thirds of the faces that wanted storage could not get any.** I had let the glass and the
   water in — they are the only near-mirrors in the building and the whole point — and that
   quadrupled the demand. Smaller entries let me hold four times as many, so almost everything that
   asks now gets one. The cost is that a reflection is very slightly blurrier.

### What is still not right

Look at it with `--debug-mode 23`, which strips everything away except the reflection, and you will
see the metal is **mottled** — each little face has measured its own answer and they disagree
slightly. It is frozen rather than crawling, so it reads as grain on the surface, and in the normal
picture the tone curve hides most of it. But it is there.

The fix is one I have already built for three other things in this renderer: let each face compare
notes with its neighbours. It needs no extra rays at all, and it is what I would do next unless you
want something else.

The other half still missing is that a mirror filling your screen gets the same thirty-six
directions as one across the room. It should get hundreds. That is the piece I keep calling "bins
from coverage", and it is a bigger job.

## And the grain on the metal is gone

The last thing I showed you had the reflection working and the metal visibly **mottled** — each
little face had measured its own answer with a couple of dozen rays and they all disagreed slightly.
This is that, fixed, and it cost no rays at all.

**What it does.** Each face now compares its reflection with the eight faces around it before it
settles. It only compares with faces on the *same flat surface facing the same way* — which the
engine can tell instantly, because a face is identified by where it is and which way it points, so a
neighbour on a different plane simply isn't found. Nothing has to be worked out about edges or
corners; they take care of themselves.

**It happens exactly once, when a face finishes measuring.** That matters more than it sounds. A
smoothing pass that keeps running on its own output smears a bit further every time until a whole
wall is one colour — so it runs once, the face marks itself done, and nothing runs it again. If you
chisel near it, the face throws the whole thing away and measures and smooths again from nothing.

**What it's worth**, measured on the reflection with everything else stripped away
(`--debug-mode 23`): the graininess at the great door drops by a third. Standing still it costs
nothing I can measure — 4.292 ms against 4.229. Flying it costs half a millisecond, because a moving
camera keeps finishing new faces. 527 tests pass.

**What's left**, and it's the same one as last time: a mirror filling your screen gets the same
thirty-six directions as one across the room. It should get hundreds. That's the "directions from
how much of the screen it covers" piece, and it's the biggest remaining part of this stage.

## Directions from screen coverage: built, measured, and switched off

You said continue, so I built the piece I'd been calling "directions from how much of the screen it
covers" — the one that was supposed to make polished gold actually polished. It does not work, and I
want to be straight about that rather than ship it quietly turned down.

**What the plan said.** A surface that fills your screen should store hundreds of directions; one
across the room should store a dozen. I wrote that rule into the plan myself, two years of decisions
ago.

**Why it does not apply here, and this is the interesting bit.** A face in this game is one voxel —
about three centimetres. Stand two metres from the bronze door and one face covers about eleven
pixels, and those eleven pixels are looking at it from within **less than one degree** of each
other. Neighbouring faces are three centimetres apart, so they see the room from within a degree of
each other too. **How blurry a reflection looks is decided by how wide each stored direction is, and
nothing else** — not by how many pixels are looking, not by how many faces there are. The rule was
written for a renderer whose surfaces are big flat things; here the surface *is* the pixel, which is
the whole point of the rewrite, and the rule does not survive that change.

**And using it made things worse.** Coverage changes smoothly across a flat door, so the "gets more
directions" decision flipped from face to face along the middle of it — and two faces cut into
different numbers of directions cannot compare notes, so exactly the faces on that boundary got no
smoothing and stood out as speckle. That is a mistake I have made in this renderer before, in three
other places: **a hard yes/no decision taken per face, on something that varies smoothly, puts a new
edge on every face.**

**So I drove it off the material instead, and then it hit the real wall.** Storing four times as many
directions needs four times as many rays to fill them to the same quality. I measured it twice: at
the cheap ray rate the door speckles outright, and at four times the rays it **still** speckles,
because two dozen samples of a reflection is what it is however finely you slice the directions.
This is the third time this session that the same wall has come up: **finer directions cost rays,
one for one.**

**What I did with it.** Built, priced, and off by default — `--lobe-coverage` turns it on. The
machinery, the sizes, the counter and the debug colour are all there for whoever comes back with a
budget that can pay it. The default picture is unchanged: 4.342 ms against 4.369, 527 tests pass.

**What it would actually take** is not a constant I can tune: it is a way to spend roughly ten times
more rays on a few thousand surfaces. That is its own piece of work, and I would want you to decide
it is worth a session before I start it.

## Light comes through the windows now

Every window in the building was lit as though it were made of stone. A room with four windows in it
received **nothing** — no sun, no sky, nothing — and was drawn by whatever lamps happened to be in
it. Stand in one of the wing halls on the old build and it is pitch black.

The rays that carry light now pass through glass instead of stopping dead on it. Same room, same
camera: **the picture changes on 1,018,413 pixels out of 1,024,000**, which is the largest single
change anything in this stage has made. The reveals are bright, the light falls off across the floor,
and you can see the room.

**One deliberate asymmetry, and it is the whole design.** The ray from your eye still stops at the
glass — it has to, because the game works out a surface's light where your eye lands on it, and a
window your eye passed straight through would have no light of its own and no reflection. It is only
the *light* rays that go through. What a shadow ray wants to know is whether the sun reaches, and a
window does not stop the sun.

It costs half a millisecond on the light pass, because those rays now travel further before they
find something. The pass that draws what you see does not move at all, because it never asks.

**Still owed**: the glass does not tint or dim what passes by distance yet — a thick pane and a thin
one let the same amount through — and looking *at* a window still shows a pale panel rather than the
room behind it. That second one is the bigger piece and it is where this goes next.

## And now you can see through the window

Both of those are done, and the second one is the one you will notice.

**The tint first, because it was a smaller and more embarrassing bug.** Every glass and every liquid
in the game carries a colour it tints light with. Nothing read it. Green bottle glass and clear
window glass dimmed the sun by the same grey amount, and the colour sat in the material table being
ignored. It is read now — and it is applied **per metre travelled**, not per voxel crossed, which is
not the same thing at all. A voxel is a tenth of a metre here, so tinting once per voxel made every
pane ten times too dark; and worse, it meant the same window would tint differently depending on
which detail level the ray happened to cross it at. Distance is a real quantity, so it gets measured
in a real unit.

**Then the eye.** Until now the ray from your eye stopped dead on glass, which is why a window read
as a flat milky panel — the pane's own surface light and nothing behind it. It now does what the
light rays already did: it works out the pane, and then carries on behind it and works out whatever
it lands on next, and blends the two by how much the glass lets past.

Standing in front of one of the tall windows, before and after: it was one pale rectangle. It is now
**fifteen separate lights in a five-by-three grid, with the tan wooden glazing bars and the transom
across them**, daylight coming through the panes, and the second window at the right-hand edge
showing through as well. **707,823 pixels of 1,024,000 changed.**

**What it costs, measured, same world, three cameras:**

| what you are looking at | before | after | difference |
|---|---|---|---|
| straight at a window | 5.147 ms | 5.393 ms | **+0.246 ms, +4.8%** |
| outdoors over the roofs | 4.240 ms | 4.172 ms | none — inside the noise |
| enclosed room, no glass in shot | 6.603 ms | 6.603 ms | **none at all** |

The whole of it is charged to the pass that traces your eye, and only on the pixels that actually
contain glass; every other pixel takes the same path it always did. The enclosed room is the
interesting row — the picture there **did** change, on 33,949 pixels, because daylight now reaches
in through a window that is off the edge of the screen, and it changed for free.

**Still owed, and this is now the whole of what is left in this stage**: the glass does not *bend*
light. A window is a perfectly flat sheet you see straight through, a lens does nothing, water does
not distort what is under it, and there is no rainbow-splitting through a prism. Every material
already carries the number that says how much it bends light and no ray reads it yet. That is
refraction, it is the last piece of this stage, and it is a bigger job than everything above.

## "This contradicts the entire point" — and it does. Here's what was actually wrong

You said the rewritten renderer was meant to remove loading, and instead you get a short loading bar
counting voxels, then a blocky building, then detail arriving in chunks while you wait.

Every word of that is right, and it took one run to reproduce. The reason is a distinction nobody
had written down, so here it is.

### Drawing the world and making the world are two different programs

When you walk around the facility, two completely separate things are happening.

**Drawing it.** Your screen fires a ray per pixel into the building and works out what it hits and
how bright it is. This is the part that got rewritten. It genuinely does work the way you asked: a
thing far away resolves coarsely, a thing near you resolves finely, and the changeover is smooth
because it is one piece of arithmetic with no steps in it. That part is done and it is good.

**Making it.** The facility is not a file full of cubes. It is a *description* — a few thousand
lines saying "a portico here, six columns, this profile, this stone" — and something has to turn
that description into actual voxels before the drawing has anything to draw. That is the part that
was **never touched**.

And the way it makes them is this: sample the *whole building* at a quarter detail so you can get in
quickly, then go back over the building in **eighteen big boxes**, sampling each one whole at full
detail and dropping it in. That is your loading bar, your blocky first minute, and your chunks — in
that order, and there is nothing pixel-based about any of it. Neither of those two detail levels has
anything to do with your screen; both are picked before the first frame is drawn.

Measured on your world, from a cold start: **3.6 seconds** before you can move, then eighteen boxes
at between half a second and seven and a half seconds of work each. By the time the game had drawn
900 frames, **eight of the eighteen** were done.

### The awkward part: this was already known, twice, and nothing was scheduled

When I went to check whether the fix was in the plan or something I had just made up, I found the
answer had been written down **twice** and acted on neither time.

The mechanism has been in the plan since the day it was written: *the shape description can already
answer at any detail level you ask it for* — which is the whole trick. And two days ago, answering
an earlier version of your complaint, somebody wrote in the same document: *"half a second with a
sharp first frame means nothing is sampled up front at all."* Correct. Filed. Closed as "measured,
not a fault."

The reason it never happened is where it was filed. It sat inside the **experimental** section — the
one about walking right up to a wall and having the voxels keep subdividing under your nose — which
is the last of eight stages and is switched **off by default**. So somebody could follow the plan
perfectly, meet every target in it, and still leave you with the loading bar, because the one step
that removes it was hiding inside an optional experiment at the very end.

That is a fault in how the work was organised rather than in the code, and it is now fixed: it has
its own stage, in the order, with the ladder named as a thing that gets deleted.

### What is now in the plan, and what each step looks like when you play it

The new stage is "the world source, driven by pixels". Eight steps:

| | what changes | what you will see |
|---|---|---|
| 1 | measure what one small piece costs to make, and prove a piece made alone comes out identical to the same piece made as part of the whole building | **nothing** — it is a measurement, and three later decisions are guesses without it |
| 2 | the unit stops being one of eighteen big boxes and becomes a small piece the screen actually asked for | detail stops arriving in slabs |
| 3 | how finely a piece is made is decided by how big it is on your screen, instead of by one of two fixed numbers | the jump from blocky to sharp goes away |
| 4 | **nothing is made before the first frame at all** | no loading bar |
| 5 | light bouncing around a room may not cause the world to be made — only your eye may | nothing, until it is missing: a dark room would otherwise quietly build the whole building |
| 6 | a saved world becomes "the description, plus whatever you carved" instead of a dump of every voxel | your world files stop being 600 MB |
| 7 | the measuring tools keep working when the world depends on where you stood | nothing — it protects every number in these documents |
| 8 | chiselling something you have only ever seen from far away still cuts properly | you can carve at a distance without getting a blocky hole |

Step 1 first, because the rest are trades against a number nobody has. Then 2 and 3, which you can
look at and accept or reject on their own. Then 4, which is the headline — but it needs 2 and 3
underneath it, because "no loading bar" without them means spawning into an empty room that fills
with big blocky boxes, which is worse than what you have now. Step 6 is last because it is the only
one that could lose a building you made.

### And the step after that

Right now, even with all of the above, there is still a round trip: your eye asks for something, the
processor makes it, hands it to the graphics card, and your eye finds it a few frames later. The
step after is to teach the **graphics card itself** to read the shape description, so it can work
out what it needs on the spot with nobody in between. The notes on the shape system have said this
is where it goes since it was written — the descriptions are simple fixed-size records with no
pointers, which is the shape of thing a graphics card can walk without changing it.

That is what makes it not merely fast but instant, and it also unlocks something that has been stuck
for months: throwing away detail you are not looking at. At the moment the game keeps almost
everything, because throwing something away means paying to make it again. Once the card can make it
again in a fraction of a millisecond, keeping it stops being worth anything.

### Step 1 is now built, and it found something

Step 1 is the measuring tool, and it does two jobs: it times how long one small piece of the world
takes to make, at every size a piece can be; and it checks that a piece made **on its own** comes
out exactly the same as that same piece made as part of the whole building. The second job is the
one that can fail, and it did.

**At the coarse sizes — the ones used for things far away — the two did not match.** Seventeen
pieces out of thirty-two came out different depending on whether they had been made alone or as
part of the building. Neither answer was right. The maker has a rule for keeping things thinner
than one of its cells: a window bar half a cell wide would otherwise vanish rather than merely
look thin. But when it decided a whole region was empty and skipped it wholesale, it forgot to
allow for that rule — so it threw away regions that contained exactly those thin things. At the
detail the game builds at today the mistake is under three centimetres wide and almost never
happens; at the coarse sizes the new system will use, it is nearly a metre, and it happens all the
time. Fixed, and it costs about three per cent.

You will not see any of this in the game, and that is the honest answer: the building comes out
byte for byte identical at the detail it is made at today. What it buys is that step 2 and step 3
are now standing on a maker that gives the same answer twice.

The timings, which decide how the rest is built: **one small piece takes about 1.4 milliseconds,
and a piece with nothing in it still takes 0.2** — and almost all of that 0.2 turns out to be the
maker re-reading the building's 139 paint rules every single time it is asked anything at all. On
a test file with four rules the same empty piece takes 0.012. So the thing to fix before anything
else is that the maker should read the rule book once, not once per question. Nobody knew that
before this measurement; it is exactly what step 1 exists for.

### Step 2 is built: the world stops arriving in slabs

The old way cut the building into eighteen boxes of about twelve metres each, decided before the
first frame, and sharpened them one at a time. That is what you were seeing as it "loading in
chunks" -- and a finer fixed grid would have been the same complaint at a smaller size, so that is
not what replaced it.

Now the piece being made is a **cube of the same tree the renderer uses to look at the world**, and
it **splits when you get close to it**. The building starts as four-metre pieces; anything within
about eight metres of where you are standing is made a metre at a time instead. So what arrives is
small and near you, and the far side of the building still arrives in bigger pieces because you
cannot see the difference from there anyway. It is the same rule the game already used to decide
what to sharpen FIRST, now also deciding how big a bite to take.

**It builds the same world.** On a small test building, made the old way and the new way, the two
come out **byte for byte identical** -- same voxels, same materials, same everything. That is the
check that matters: changing how the work is cut up must not change what gets built. The one
exception is the pass that cleans up stray single voxels of the wrong colour, which now judges a
voxel at the edge of a piece against the empty space outside it rather than against its real
neighbours: about 46 voxels in 152,000 come out a shade different on the facility. Small, measured,
and written down rather than waved away.

I tried the obvious fix -- make each piece slightly larger than needed so the edges can see their
neighbours, then trim it -- and it **lost 240 voxels out of 1.4 million**, because the shape-maker
answers slightly differently when the box it is given is an odd size. So that fix is out until the
shape-maker is fixed, and the measuring tool now watches for it.

**You will not see the loading bar go yet.** That is step 4, and it needs step 3 first.

### Step 3 is built: how finely a piece is made follows how big it is on your screen

Until now every piece was made at full detail -- 32 voxels to the metre -- however far away it was.
A wall sixty metres off was carved at three-centimetre precision so it could be drawn as a smudge.

Now the detail follows the size on screen. A piece eight metres across is made at one voxel per
metre; a piece a quarter of a metre across is made at 32. And since a piece is always eight voxels
across whatever its size, that is one rule doing three jobs: how big the piece is, how finely it is
cut, and when it splits are all the same number. Settled, every part of the world is held at about
one voxel per pixel -- full detail within about sixteen metres of you, and progressively coarser
beyond, which is precisely as much as you can see.

**It builds the same world.** Forced to full detail, the small test building comes out **byte for
byte identical** to the old way again -- this time through **9,819 separate pieces made at six
different resolutions**, each one blown up and then replaced by finer children as the scale
descends. That is the check that the whole ladder of detail composes properly rather than merely
looking plausible.

On the facility, standing in the rotunda: 120 eight-metre pieces become about 31,000, of which
9,272 get made and **14,096 are left coarse because you cannot see them from where you are
standing**. The worst pause from a piece landing is 12 milliseconds, under a frame.

**What you would see**: the jump from blocky to sharp is smaller and it follows you. It used to be
one jump of 4x (eight voxels per metre to thirty-two) applied to a whole twelve-metre slab. It is
now two jumps of 2x each, applied to a piece the size of your reach, at the moment you get close
enough for it to matter. It is not perfectly smooth yet -- the sizes are powers of two, so there
will always be some step -- but the big visible one is gone.

**What I did not check**: walking from sixty metres up to the facade with a camera and photographing
every frame, which is the test that would say whether those two remaining steps are visible to an
eye. The world is proved; the walk is not.

### The brick blocks: half of them are gone, and I was wrong about what they were

You reported big cube-shaped lumps sitting on top of things while the world loads, with a photo of
an urn as a stack of coarse blocks in a niche whose walls were already sharp. I gave three different
answers to that and the first two were wrong. Then you said, in passing, *"the circle voxel aim
thing doesn't detect the lumps"* — and that one sentence was worth more than all three of mine,
because the aim circle is drawn by asking the world what is under your cursor. If it goes straight
through a lump, **the lump is not made of voxels**. Nothing you can chisel is there. It is the
renderer drawing a solid cube where it has been told there is something and has nothing to show.

It turns out there are two different lumps wearing the same coat.

**The kind that never goes away — fixed this week.** When the game sharpens a piece of the building,
it writes the finer version over the blocky one, and that means *rubbing out* the coarse blocks
that were too fat. The rubbing-out worked. What did not happen is that the now-empty box was left
lying around, still registered as "there is something here". So the renderer kept drawing a cube on
it, kept casting its shadow, and kept refusing to build anything better — for ever, because there
was nothing there to build. Standing in the entrance hall with the world completely finished, there
were **304 of these**. They are the grey brick-sized blotches all over the marble, the black bars
either side of the doorway, and the dirty shadow under the arch.

The fix is one line's worth of idea: when a write empties a box, throw the box away, which is what
every other part of the game that removes matter already does. Measured, standing still with the
world settled: **304 empty boxes to nought**, faces wrongly in shadow **12,517 to 113**, stray white
sparkles **108 to nought**, and a fifth of the screen changed. The building itself is bit-for-bit
the same building — same voxel count, same fingerprint — so nothing was given up for it. It is also
very slightly *faster*, and the work the processor does per frame dropped by two thirds, because the
game had been asking for those 304 boxes over and over again three hundred million times a run.

**And the reason it went unnoticed for so long is worth a sentence.** The game has three separate
self-checks that compare the renderer's copy of the world against the world. All three said
"agrees, perfectly" while those 304 lumps were on screen — because all three ask the *same* question
underneath, and that question was the one telling the lie. I had to write a check that asked the
world something none of them asked.

**The kind you see while it loads — still there.** I photographed the same moment of the load with
and without the fix and they are the same picture, so the big blocks during loading are something
else. I have now measured what, and it is not the ladder and not the first pass:

The renderer has room for a fixed number of bricks. During a load it is **completely full** — jammed
at its ceiling — throwing away two to three hundred requests every frame, and its own report said
`0 refused`, because the counter was only being incremented on one of the four ways it can run out.
Every refusal was being logged as a success. Once the world settles it needs **one eighth** of that
room. So something during loading is holding eight times the bricks the finished world needs, and
nothing gives any of them back while it happens.

That is the next thing I look at, and the question is not "make the room bigger" — that would hide
it. It is *what is holding a quarter of a million bricks during a load that thirty-three thousand
serve afterwards*.

### The world now sharpens around you nearly five times faster

You asked for this to be dramatically faster at no expense. It is **4.8 times faster** — the
facility went from **83.6 seconds** to settle down to **17.3** — and I want to be straight with you
that you asked for a hundred times and I can tell you exactly why that is not on the table by this
route. The short version is at the bottom.

**Nothing had ever measured where a load's time goes.** The log timed one batch of work, and a batch
is a few hundredths of a second. So I made it add up the whole load and say which half spent it.
That one line found three faults in its first run, and every one of them was the game **waiting**
rather than working.

1. **The sharpening was running on one core.** The game picks a batch of pieces and samples them.
   Each sample was told "spread yourself across the workers" — and a piece is eight voxels wide, so
   there is nothing to spread. I had already measured that a year's worth of ago and written it down.
   So sixteen pieces were done one after another, each on effectively one core, while the other seven
   sat idle. Giving each *worker a piece* instead of each *piece the workers*: **34 seconds to 7.7.**

2. **Seven million ray tests to choose sixteen pieces.** To decide what to sharpen next, the game
   walks its whole list of pieces twice and fires a test ray for any piece that beats the current
   leader, to see whether it is behind a wall. The catch: a piece that *fails* that test does not
   become the leader — so the bar never rises, and every one of the 6,042 pieces permanently hidden
   behind walls fired a ray on almost every wake, for the whole load. Ranking everything cheaply
   first and only ray-testing the handful that could win: **26 seconds to 1.7, and seven million rays
   to 150 thousand.**

3. **It was asleep four fifths of the time.** Sixteen pieces a go was the right number back when they
   were done one at a time. Once they are spread across the workers, sixteen takes four milliseconds
   of a twenty-two millisecond frame, and the game only starts a new batch once per frame — so it did
   1,730 batches over 1,716 frames and spent the rest waiting. **128 a go** instead.

**And it is the same building.** That is the part I checked hardest, because "faster" is easy if you
are allowed to make less. Forced to full detail on the test clip, the world comes out with the
**identical fingerprint and the identical 1,430,104 voxels** as the reference I recorded two days
ago. On the facility the finished world has **32 more pieces sharpened and 351 more voxels out of
125 million** — very slightly *more* complete, because the picker now reaches a couple of things it
used to keep re-testing. The settled frame rate is unchanged, and the frame rate *during* loading is
**better** (24.1 ms a frame to 17.7), because the main thread's share of a batch went from 14 ms to
under 2.

**One thing I measured and did not keep.** The sharpener deliberately gets half your machine so it
does not stutter the game while you play. Giving it everything makes sampling 30% faster and makes
the *pasting* eleven times slower — the background work simply starves the foreground work. Left
alone.

### Why it is not a hundred times, and what a hundred times would actually take

The three faults above were all waiting. What is left is real work, and it is one number: building
the facility asks the shape-description about **3.8 million voxels**, and each one is checked against
**139 painting rules**, at about eight millionths of a second each. That is 32 seconds of one core's
time. Five cores make it **6.4 seconds**, and that is now half of what is left. No amount of
scheduling gets under it.

Two more halvings are in sight and I have not built them: the sharpener still idles about 40% of the
time waiting for the next frame (worth roughly 12 seconds down to 8), and the blocky first pass costs
another 3.6 seconds, which is step 4.

To get to under a second you have to stop asking a 139-rule description about voxels **on the
processor** and ask it on the graphics card, which is step 12 of the plan and was always going to be
its own piece of work. This measurement is the first hard number for why that step exists — before
today nobody could have told you that the field evaluation is half the load.

**Steps 4 to 8 are not built.** The loading bar is still there — that is step 4.

### Then I looked at the second time you open the game, and it had been wrong for a long time

Everything above was measured on a **cold** start — the first time you ever open a world, when there
is nothing saved. Every launch after that reads a saved file instead, and that is the path you
actually spend your life on. Nobody had measured it. Three things were wrong with it, and the first
two had been wrong for weeks without anything on screen or in the log saying so.

**The saved building was frozen at sixteen voxels a metre.** The game saves a list of which pieces it
has sharpened, so the next launch can carry on instead of starting over. It only wrote down pieces
that had reached the *full authored* detail — and from the ordinary spawn point **no piece ever
reaches that**, because most of the building is behind a wall or across the courtyard. So the list
was written **empty**. And an empty list has a second meaning in that file: *"this world was never
built by sharpening at all — it is finished, leave it alone."* The loader read the second meaning,
did nothing, and printed **"the world is at the detail the clip asked for"** over a building that was
permanently half as detailed as its description. It was not slow. It was **finished and wrong**, on
every launch after the first, for as long as that file has existed.

That is the same trap that has now caught this project three times: *"nobody did anything"* and
*"there was nothing to do"* looking identical. It is worth me writing down that I have started
checking for it on purpose.

**The speckle cleaner was switched off too.** The building has materials that are *meant* to look
mottled — deliberate two-tone dither, weathering, verdigris. The cleaner that removes accidental
stray voxels has to be told which materials to leave alone, and that decision is taken once, up
front, while the whole building is being looked at. A saved load never took it — and the cleaner
reads "no list" as **"leave every speck everywhere alone"**. So it ran twenty-three thousand times a
load and did nothing at all. A saved world was genuinely a *different world* from a cold one.

Both are fixed, and the honest price is that **a saved load went from 3.8 seconds to 5.8**. I took
that trade deliberately, because the old 3.8 was fast the way skipping the work is fast. What you get
for the two seconds is the building at its real 32 voxels a metre — **62,752 finished pieces where
there were none** — and a world that comes back identical every single time you open it.

### The game was sweeping a finished world for ever, and writing half a gigabyte doing it

Then I went after those two seconds, and found something worse sitting underneath them.

When the world has nothing left to sharpen, the sharpener never noticed. On a saved launch it woke
**270 times**, walked a list of **17.9 million entries**, fired **99,600 test rays**, and delivered
**nothing at all**, every frame, for as long as you left the game running — about **2.2 milliseconds
of every single frame**, permanently, for no result. Everything left to do was behind a wall or
behind you.

**And the part I had not counted at all.** A sweep that achieves nothing still ticks a couple of
pieces off as "settled", and the save routine asks only *"has anything been ticked off since I last
wrote?"* — so it said yes, every time. One launch wrote the whole world file to your disk **28 times.
Half a gigabyte, on a run that sharpened nothing whatsoever.**

The fix is that the sharpener stands down when a sweep finds nothing, and wakes up again for the only
two things that can change the answer: **you moving**, and **the world being edited**. Saved launch,
measured: wakes **270 → 33**, work on the main thread **584 ms → 147**, test rays **99,600 → 11,832**,
saves to disk **28 (504 MB) → 1 (18 MB)**. The cold load produces a **byte-identical building** and
is cheaper anyway, because the sweeps it now skips were the ones finding nothing.

**Two versions of this were wrong before the third worked, and both are instructive.** Standing down
the moment a sweep comes up empty loses the tail — a piece refused for being hidden is only refused
*provisionally* and gets retried later, and a sleeping sharpener never retries anything, so the world
came out **32 pieces short** and its fingerprint moved. And "stand down once enough time has passed"
can never happen, because every sweep pushes the deadline out ahead of itself. It has to be measured
from the last sweep that actually *delivered* something.

I also want to record how I found it, because it cost most of a day. I twice argued from a counter
printed **by the new code** — which was like asking the suspect for an alibi. What settled it in one
run was rebuilding the *old* code on the same machine in the same minute and running the identical
command. **Reach for the honest comparison before the third theory, not after it.**

### The blocky first pass is gone — you can turn it on today, and I have not made it the default

This is step 4, the one you have been waiting for: **nothing is built up front**. The world assembles
itself around you from nothing, at the detail your screen asks for, instead of starting as a blocky
whole building that then gets refined. Run the game with `--no-coarse-paste` and you have it.

I asked it five questions before I would believe it:

- **Does the world grow when you walk?** Standing still it builds 19.8 million voxels; walking the
  same scene, **29.6 million**. It stops when there is nothing more to see from where you are, and
  your moving wakes it up again.
- **Is a world that depends on where you stood still reproducible?** Same camera twice, **the same
  fingerprint to the last digit**. A different camera gives a different world, which is the point.
- **Does the measuring harness cope?** Yes, and it needed no changes at all — it compares like with
  like by construction.
- **If you leave and come back, is the work kept?** Yes, and this is the one I am most pleased with.
  Spawn at A, quit, come back at B: it loads A's whole world in **52 milliseconds** and *adds* to it.
  Go back to A and it takes **9.1 seconds instead of 18.6**, because it re-makes almost nothing. The
  world accumulates across sessions instead of being thrown away.
- **Can you chisel into a place that was never built?** Yes. A twenty-metre cube cut through the
  middle of the building comes out with clean sharp faces, and the portico it exposes — six columns,
  pediment, entablature — is made at full detail on the spot.

**Now the part I owe you, which is why it is not the default yet.**

**It does not remove the loading bar.** I need to be blunt about that, because it is the thing you
asked for. The up-front pass does two jobs, and I have only been able to remove one of them. It
*builds* the blocky building — that is gone, and it saves about a second. It also **takes one look at
the entire building** to decide which materials are deliberately mottled and must not be cleaned up.
Nothing else in the game ever sees the whole building at once, so there is nowhere else that decision
can come from. I tried three ways round it. Adding the answer up piece by piece **destroys two of the
six deliberate dithers**, because a small box cannot tell its own edge from open air. Taking the
decision at full detail instead works, and costs **19 extra seconds**. Taking it from only the part
you have looked at protects **nothing**. So the wait stays at about 2.8 seconds until step 12 changes
where the building is made. That is an honest dead end, not a thing I have skipped.

**Two other things keep it opt-in.** A chisel *sixty metres away* into a surface you have never
walked up to is untested — near ones are fine. And switching it on by default makes the shipped world
depend on where you stand, which invalidates every performance figure recorded in this project; that
is a deliberate decision to take on purpose rather than by accident.

**One mistake in here was worth more than the feature.** When I first measured the world without the
blocky pass, it held a sixth as many voxels and I wrote it up as a bug — *"a sixth of a building"*.
It is not a bug. It is exactly what you asked for in the first place: *if you cannot see it, it does
not exist*. A world that stopped early and a world that finished everything it could see produce
**identical readings on every counter I had**. The only measurement that separates them is whether
the number moves when you walk — and I had not taken it.

## A whole stage cancelled, and a fifth of the building work found in the wrong place

Two things happened here and neither of them is a feature. One is a stage of the plan I was about to
start and did not, and one is a piece of the building work that was being spent on nothing.

**The stage I did not build.** The list you ranked put "make the shape maths cheaper" second, and its
whole case rested on one line the tools print: **a quarter of the building's shape description has no
bounding box round it**. A box round a piece is how the maths skips it — no box, no skipping, and the
reasoning was that a piece with no box also takes the box off everything containing it, so a quarter
becomes most of it. The plan said, in as many words, *count them before writing any code*. So I
counted them.

**They are not in the shape at all.** Of the 923 boxless pieces, **746 are colours and patterns** —
numbers, dials, grains, brick courses — that the shape maths never looks at. Of the 177 that are in
the shape, **146 are two kinds that refuse a box deliberately**, for a reason already written down:
giving them one would silently delete geometry. And the number that decides it: **none of the 923 is
anywhere the box would have been read**. There was nothing to bound. The stage is closed with no code
written, which is the outcome the "measure first" rule exists to produce — and it cost an afternoon
instead of a fortnight.

**The other half of that stage was real, and it was backwards.** The same tool reports that 19 of the
building's wide assemblies have a *search tree* built over them to speed the maths up. Nobody had
ever measured whether that tree helps. It does not. Building the whole facility with the trees takes
**67 seconds**; with them switched off, **54**. That is a fifth of the shape work spent on a
structure that never once let the maths skip anything — and the reason was written in the code before
the tree was built: the parts of a building are *layers*, not *regions*. Every wall, window and roof
box covers the whole site, so there is never anything to rule out. I checked the obvious counter-move
too — more trees rather than fewer — and it is worse again.

**It comes out the same building.** Every version produced an identical fingerprint, so this is the
same world made faster, not a different world. The trees are switched off by default and the
machinery is kept, in case somebody one day builds a street of separate houses, which is the case it
was written for.

**What you should expect to see, and what I cannot promise.** Loading should get shorter — my
estimate is **about a second off the 17 you have now** — because the shape maths is the largest thing
left in it. **I could not run the game to check.** This session ran on a machine with no graphics
card in it at all: I could run the shape maths, the tests and the tools, and not the game. So the
second is an estimate from the part I *could* measure, and the first thing to do on your machine is
open the loading report and see whether it is there. If it is not, say so and I will find out where
it went.

## A third of the building work was measuring boxes, and half of that was the same measurement twice

After the last piece I still did not know *where* the building work goes — only how much of it there
is. So I ran the shape maths under a profiler, which counts every single machine instruction and,
unlike a stopwatch, cannot be thrown off by the machine being busy.

**The answer was not the shapes.** It was the *skipping*. Before the maths asks a piece of the
building "how far away are you", it asks a much cheaper question first — "could you possibly be the
nearest thing?" — using a box drawn round that piece. Those box questions turned out to be **31% of
the entire build**, more than all the spheres, cylinders and mouldings put together.

**And it was asking each one twice.** The code works out the distance to each piece's box in order
to put the pieces in order — nearest first — then forgets the numbers it just worked out and asks
for them all over again when it decides which pieces to skip. Four pieces cost seven questions where
four would do. The same thing was happening in the other of the two search paths.

There was also a small piece of reasoning to add: once the pieces are sorted nearest-first and one
of them is far enough to skip, **every piece after it is too** — they are further away and the
answer we are comparing against only gets smaller. So it can stop looking rather than test each one.

**What it is worth.** The box questions drop by **37%**, and the whole build by **18%** — measured
two ways that agree: 14.7 billion instructions down to 12.1 billion, and the clock going from 65.5
seconds to 53.2 on the same test. **It comes out the identical building** — same fingerprint on five
different clips — which is the only way I would accept a change like this, because a skipping rule
that is slightly wrong does not crash or look broken, it just quietly moves a surface and puts the
moss in the wrong place.

**One thing this ruled out, which is worth as much as the speed.** I had assumed the skipping was
working badly. It is not: it already throws away **93% of the building** at every question — 175
pieces looked at out of 2,505. So there is no big win left in "skip more". The remaining cost is the
plain price of asking, which is what step 12 — moving this onto the graphics card — is for.

**And a correction to the last entry.** I had reported that the extra search trees cost a fifth and
save nothing. They were also paying for every box twice, so that comparison was unfair to them. With
both sides repaired they are still a third *worse* than not having them, so the conclusion stands —
but I would not have known that without going back and re-running it.

**Same caveat as before:** no graphics card here, so the building maths is measured and the loading
screen is not. The number to look at on your machine is the loading report.

## Where the building maths actually goes — and a good idea that was not

Having made the skipping cheaper, I wanted the map: of everything the maths does to place one voxel,
what is it doing? So I counted, on the facility.

**A third of it is one thing: joining parts together.** When a clip says "this building is these
hundred and eighty pieces", the code folds them up four at a time, each fold carrying everything so
far. That makes a ladder a hundred and eighty rungs long, and every single question about every
single voxel climbs the whole ladder. Actual shapes — the spheres, boxes and columns — are only a
fifth. Moving things about (turning, mirroring, repeating) is another fifth.

**So I built the obvious fix and it did not work.** Instead of a ladder, fold the pieces into a
balanced tree, four at a time, so it is five levels deep instead of a hundred and eighty. Each
branch then covers four neighbouring pieces rather than the whole site, which should let far more be
skipped. It does: 5.5% more skipping. **And it evaluated 2.7% more actual shapes, for a net gain of
under 2%** — so I threw it away.

The reason is a nice one. The ladder is accidentally clever: because each rung carries *everything
below it*, climbing down once gives you a very good "nearest thing so far", and a good answer is
what lets everything else be skipped. A tidy tree gives you a worse answer for longer. Neatness lost
to an accident.

**That is now three ways of organising the same pieces, all measured** — the ladder, the balanced
tree, and the search trees from last time — and the plain ladder beats both. The reason is the same
every time and it is written in the code: a building is made of *layers*, not *regions*. Every wall,
every window, every roof covers the whole site, so there is nothing to rule out. **I would not try a
fourth arrangement, and I would want a clip of separate buildings before anybody did.**

**None of that is in the game.** Only the change from the previous entry is; this one was built,
measured and taken back out. It is in the record because the next person to have the same good idea
should be able to find out in a minute that it was already tried.

**What the two changes come to together.** I ran what the game did at the start of this session
against what it does now, alternating between them so the machine could not favour one: **13.9
seconds down to 8.9** to build the facility, which is **1.56× faster**, and the identical building
out of both. Making the building is about 9 seconds of your 17-second load, so **if that ratio holds
it is around 3 seconds off** — but I want to be careful with that number, because I could not run
the loading screen here to check it. It is the first thing worth looking at when you next build.

## Why your friend's copy had no facility in it

He was right, and it was not the renderer. **The download did not contain the building.**

The game keeps its worlds, its shapes and the twenty-two pieces the facility is assembled from in a
`clips` folder next to the program. The script that builds the download put the program in the zip,
the shaders in the zip, the licence and the readme in the zip — and never the clips. So on his
machine the shelf had nothing on it, there was nothing to open, and when the game went looking for
the facility it found no such file.

**And then it told him the wrong thing.** The message he saw — "it did not build anything" — is what
the game says when a world is *described badly and comes out empty*. It said the same thing for a
file that was never there to read. Those are now two different messages, and the missing one tells
you which folder the game looked in.

**Why this never happened to you.** Building from source copies the clips next to the program
automatically, every time. So the layout you have always run is the one the packaging script was
supposed to reproduce, and did not — which is exactly the kind of fault that only ever shows up on
somebody else's computer.

**The check that should have caught it has been fixed too, and that matters more than the one line
that fixes the zip.** Before publishing, the script already unpacks the download somewhere else and
runs it, precisely so a broken download is caught before anybody gets it. But all it checked was
that a picture came out. **A picture of an empty sky is still a picture.** It now reads what the
game said while it ran, and refuses to publish unless the game reports a world with voxels actually
in it.

**And two things about building from source, since he hit that as well.**

- `run.bat` only built the game when the program was *missing*. Every time after the first it
  started whatever was already there — so pulling new code and running it gave you the old game,
  which looks exactly like a change that did not work. It now always builds, and it says so plainly
  if a copy of the game is already open and blocking it.
- `build.bat` told Windows where to find one of the two tools Visual Studio ships and not the other,
  so a machine with Visual Studio and nothing else got `'cmake' is not recognized` — a message that
  names nothing you can install. It now finds both, and if the Vulkan SDK is missing it says so by
  name with the page to get it from, instead of failing deep inside a wall of build-system text.

**What I could not do from here.** This machine has no Windows and no graphics card, so **I could
not run any of the three scripts I changed, and could not compile the game itself.** I compiled the
changed piece of it on its own, checked the scripts for the character-encoding trap that has bitten
this project before, and ran the 541 tests. **The next thing to do is on your machine: run
`build.bat`, then `tools\package.ps1`, and watch it print how many voxels the unpacked copy built.**
If it prints a number, the download is fixed. If it stops with a complaint, it has caught something
and the complaint will say what.

## There is a new download, and this time the building is in it

**https://github.com/worldshapergame/WorldShaper/releases/tag/v0.7.1** — `WorldShaper-v0.7.1-windows-x64.zip`.
Send your friend that one.

Getting there meant fixing the thing that has stopped every previous release: the automatic builder
on GitHub, which has never once worked, so every download so far has been hand-made on your machine.
**The reason it never worked was written down wrong in our own notes.** It was recorded as the build
machine crashing its C++ compiler. It was not: every piece of the game compiled perfectly, and the
thing that died was the *shader* compiler, which comes from a separate toolkit the builder installed
fresh each time by asking for "the newest one". So the tool doing the work was a different tool every
run, and one of them had a bug. It now asks for a specific version — the one the project already
targets everywhere else — and the whole thing ran green in **five minutes**.

**And it checks itself now.** Before publishing, it opens the zip it just made somewhere else
entirely and makes the copy of the facility *inside that zip* actually build. A download with no
building in it cannot get past that, which is precisely the fault your friend hit. It does this
without a graphics card, because building a clip is pure arithmetic — no screen needed.

So this release is checked in a way no previous one was, and it carries a signed statement from
GitHub tying the file to the exact commit and build that produced it.

**What I still have not done:** run the game. There is no graphics card on this machine, so I have
proved the download contains the facility and that the facility builds from it — not that the game
draws it. That last step is the one thing only you or your friend can do: unzip it, run
`WorldShaper.exe`, and the facility should be on the shelf.

## The thing blocking "no loading screen" turns out to be protecting four things that are not there

The last big step you asked for — the world building itself around you with nothing loaded up front
— has been sitting behind one question for four sessions. It is worth explaining, because the answer
is a good one.

**The question.** Before the game builds anything, it takes one look at the whole building to decide
which materials are *deliberately speckled* — the weathering coats, where scattered single voxels of
a different colour are the point. Everything else gets a cleaning pass that removes stray voxels.
Get that decision wrong and the cleaning pass wipes the weathering off the building. That single
look at the whole building is 2.8 seconds, and it is the last thing standing between you and no
loading screen at all.

**So: can that decision come from anywhere else?** There were three ideas. I tested all three today,
on the real building, at four different levels of detail.

- **Tune the thresholds until a full-detail look agrees with the current coarse one.** *Impossible* —
  and I can prove it rather than just failing to find a setting. Three of the six protected materials
  have **no stray voxels at all** at full detail, and the test is entirely about counting stray
  voxels. Nothing you can set will protect something that isn't there.
- **Work it out from the clip file instead of from a sample** — a dither is written as a rule keyed
  on noise, so read the rules. *No.* Only two of the six protected materials are written that way,
  and eight materials that *are* written that way aren't protected. The two things barely overlap.
- **Take the look at some coarser level that still agrees.** *No.* I measured four levels; no two
  agree. They share one or two materials out of six.

**And then the finding that actually matters.** Laying the four levels side by side: **only one of
the six is genuinely a dither.** Material 27 has a big, steady population of speckles at every level
of detail — that is a real deliberate stipple. The other five appear at one level and vanish at
others. One of them exists *only* at the coarse level the game currently takes its decision at.

Which means the current decision is not protecting five weathering coats. It is protecting the
**blurring** of coats that come out perfectly well once the building is made at the detail you
actually stand in. It has been guarding ghosts.

**What switching costs, measured.** Build the whole facility at full detail and run the cleaning pass
both ways: the current decision leaves 896 voxels repainted, the new one 1,050. **A difference of 154
voxels in a building of 3.8 million.** And part of that difference is a bug being fixed: two
materials are currently left alone purely because the coarse look never happened to see them.

**So the blocker is answered, and the 2.8 seconds can go.** The decision can come from the world as
it is built — machinery that already exists in the game and is already switched on.

**What I could not settle, and it needs your eyes, not a number.** Far-away parts of the building are
made at coarse detail, and *there* those five coats do have stray voxels. Under the new rule they get
cleaned. Whether the weathering looks different at a distance is a thing to look at, and I have no
screen. That is the one open question, and it is a small and specific one — everything close enough
to matter is the 154 voxels.

## Measuring the last stage before anybody builds it

With the card-free work on the loading time done, the only stage left in the plan that can be
touched from a machine with no graphics card is the last one: **moving the shape maths onto the
card**. That is the step that makes "no loading" total rather than merely fast — the card works out
the shape of the world where it needs it, instead of asking the processor and waiting.

I cannot build that here. What I could do is measure the three things that stage will run into on
its first day, all of which were assumptions until today.

- **"A shallow stack."** The plan assumes the shape maths can be walked with a small fixed-size
  working area, which is what a graphics card needs — it cannot grow one on demand. Nobody had put a
  number on it. It is **41**: that is the longest possible chain of nested shapes in the building,
  and over three hundred million measurements the deepest it ever actually went was **36**. So a
  fixed working area of 64 is comfortable. The assumption holds, and now it holds with a number
  against it.
- **How much has to be sent to the card.** The entire description of the building — every shape,
  every pattern, every paint rule — is **351 kilobytes**. Smaller than a photograph. There is no
  streaming problem to solve and nothing to be clever about.
- **What the card has to be able to do.** Thirty-one distinct operations, listed with counts, so
  whoever writes it knows the whole job in advance instead of discovering it one error at a time.

**And one finding that will save somebody a bad day.** Three of those thirty-one — the ones that
work out whether a surface is a sharp edge, how enclosed a spot is, and which way it faces — do not
work like the others. They ask the *same* shape question seven or fourteen times at slightly
different places, and build their answer from the spread. Written the obvious way, a card program
gets those three wrong — and the facility uses exactly one of each. The building would come out
correct everywhere except its **weathering**, which is precisely the kind of fault that gets blamed
on the weathering rules for a week.

**What is still unknown:** whether the card can use ordinary lower-precision arithmetic, or needs
the slower high-precision kind. That one genuinely cannot be answered without writing the second
copy of the maths and comparing them voxel by voxel, which is the next piece of work and the one the
plan already asks for.

## The game builds on a machine with no graphics card now, and it settled the loading-screen question

Two things happened today and the first one is why the second was possible.

**The game had been "impossible to build" on a machine with no graphics card, and it was three
small things.** Every session run from such a machine — several of them now — has started by
writing off two thirds of the work as untouchable. That turned out to be wrong: the other compiler
was complaining about three harmless details, one of which was a piece of code that had been dead
for days and that the usual compiler is simply unable to notice. Fixing them takes the whole
program from "will not build" to "builds and runs", which means the building can be made, measured
and checked anywhere. What still needs a graphics card is anything about the *picture* — speckle,
lighting, frame rates. Those are untouched by this and still wait for your machine.

**And with that, the loading screen question got a real answer instead of a plan.** The last note
said the remaining blocker was one line away: as the world sharpens piece by piece, let it keep a
running tally of the speckled paint it sees, and the tally becomes the judgement that decides which
weathering coats are deliberate — no whole-building measurement, no loading bar. I built the way to
test that and ran it on the facility.

- **The idea is sound, and exactly sound.** Done properly, the running tally is not *close* to the
  whole-building measurement — it is **identical**, every material, to the voxel, and it protects
  exactly the same six weathering coats the game protects today.
- **Doing it properly means asking for slightly more than each piece.** To judge the paint on the
  edge of a piece you have to be able to see one voxel past the edge, into the piece next door.
- **And that is what makes it not worth doing this way.** Asking for that one extra voxel all round
  makes each piece **twice** as expensive to work out — measured at 2.02 times, and 2.13 at full
  detail. The building's sharpening work would go from six seconds to twelve, to save the two and
  three quarter seconds of loading bar. Worse, the six seconds is time you spend *inside the game*
  waiting for what you are looking at to sharpen, and the two and three quarters is a bar before
  you arrive.
- **The free version was tried too, and it fails in the way that matters.** Judge only the middle of
  each piece and ignore its edges: costs nothing, and it wrongly cleans away **two of the six
  deliberate weathering coats**. That is the exact fault this whole judgement exists to prevent.

So the step is refused, and the reason is written down with numbers rather than left as an opinion.
There is a third way — take the extra voxel from the world that has *already been built* next door,
instead of working it out again — which costs a memory lookup instead of a calculation. That is what
the next attempt should be, and unlike these two it cannot be measured without a graphics card,
because it only happens while the game is running.

## The nineteen-second bill for the loading bar is down to three

There was one other way to answer the same question, and it has been sitting refused since June for
one reason: **price**. Instead of judging the speckled paint as the building sharpens, judge it once
at the end, over the finished building. That works — it gets the right answer — and it took
**nineteen seconds** of the machine standing still. Nobody was going to trade a loading bar for
that, so the whole choice stayed shut.

The nineteen seconds turned out to be two mistakes rather than a fact about the work.

- **The building was being walked twice.** Once to decide which weathering coats are deliberate, and
  once again to rub out the accidental specks. Those two walks look at exactly the same voxels and
  ask exactly the same question of each one — *is this voxel alone?* The reason it was done twice is
  real and has not changed: you cannot rub anything out until you have judged the *whole* building,
  or you would be deciding what a coat is from one corner of it. But **finding** the lone voxels does
  not need the judgement; only deciding what to do with them does. So now it walks once, writes down
  every lone voxel it finds, takes the judgement, and then goes back to that short list — a few
  hundred entries — instead of walking a hundred million voxels again.
- **And it was doing it all on one core** while the rest of the machine sat idle, in a program that
  already has a way to spread work across cores and uses it everywhere else.

Measured on the facility, without a graphics card, three times each way: **thirteen seconds to two.**
The same on the smaller version of the building: five and a quarter seconds to two thirds of one.
About three quarters of that is using the whole machine and about a quarter is not doing the work
twice — and this machine only has four cores, so on a real one the first part is worth more.

Two honest limits on that. The building I could measure here is a quarter the size of the one the
figure of nineteen seconds came from — sampling the full-detail version takes three gigabytes and
more than ten minutes on a shared four-core machine, and it was abandoned twice — so what carries
across is the *ratio*, five to eight times cheaper, rather than the exact seconds. And the machine is
busy enough that the same measurement run twice, hours apart, gave thirteen seconds one time and
fifteen the other; everything about it that is not a clock came out identical to the digit both
times, which is how I know it is the machine and not the change.

**This does not switch the loading bar off.** It removes the reason the choice could not be made —
the bill. There is a second objection still standing, and it is a real one: if the judgement is taken
from the building *as far as it has been sharpened*, it depends on where you happened to walk, and a
weathering coat that survives one route through the building and not another is not something I am
willing to ship. That one is yours to decide when it comes to it.

**And it turned up an old bug on the way,** of the sort that is worth more than the seconds. The
speck-rubbing pass has a note at the top of it, written when it was built, saying it deliberately
takes a photograph of the surface first and works from the photograph — so that rubbing out one
voxel cannot change the answer for the voxel next to it. Half of that is what the code does. The
other half reads the surface *live*, so rubbing one out **does** change its neighbour's answer, and
the result depends on the order things were scanned. On the whole building that is sixty-one voxels
out of fifteen million, so it is not something you would ever see — but it means a note in the code
has been telling everybody the opposite of the truth for two months. I have not changed it, because
that pass runs on every piece of every building as it sharpens and changing it would change every
building the game makes. It is now written down and there is a test that fails the moment anybody
changes it, which is the honest place to leave it.

## Glass now bends what is behind it

Windows and water in this game have been drawn as *transparent* for a while — you can see through
them, the light comes through them, coloured glass tints what passes. What they have never done is
the thing that actually makes glass look like glass: **move what is behind it**.

Look through a real window at an angle and the wall behind it sits slightly to one side of where it
would be without the glass. Look into a basin of water and the bottom is nearer than it really is.
Until now this engine drew both exactly as if the glass were not there — a hole in the wall with a
tint on it.

**Now the ray bends.** It bends going into the glass, carries on through it, and bends back coming
out the other side — two bends, not one, and that pair matters: a flat pane bends a ray one way and
then unbends it, so what you get is a sideways *shift* rather than a distortion. Through the
building's own 12 cm glazing at a slanted angle that shift is about four centimetres, which is a
voxel and a bit — enough that a straight edge behind a window visibly steps sideways as you move.
Water is the case where the two bends do not cancel, because the second one would be at the bottom
of the basin and the ray stops there instead: that is exactly why a pool always looks shallower than
it is, and now it does here.

Two smaller things came with it. Coloured glass and water now darken by **how far the ray actually
travelled through them** rather than by how many voxels it clipped, so looking into deep water is
darker than glancing across it. And looking up at a water surface from underneath past a certain
angle now shows a mirror, which is what really happens.

**What I could not do is look at it.** This machine has no graphics card, so the picture is owed:
the maths is tested against the textbook, but whether it looks right on your screen is your machine's
answer to give. If it looks wrong, `--no-refraction` puts it back exactly as it was.

## And a crash that was going to be somebody's bug report

While getting the game to run at all on a machine with no graphics card, it kept dying after eleven
frames. I recorded that as the software renderer's problem. **It was ours.** The game asks the
graphics driver for half a gigabyte of one kind of memory, and it never once asked the driver
whether that was allowed. Powerful desktop cards say yes to four gigabytes, so nobody ever found
out — but the Steam Deck is the machine this project treats as its floor, and any device with a
smaller limit was getting undefined behaviour with no warning and no error, of the kind that lands
as a crash in somebody's driver with nothing to point at.

It now asks, and takes what it is given. That is not a fix for anything you can see today; it is a
crash report you will never receive.

## Marble glows now, and the game worked out which bits of it are thin

Marble is not a grey stone with light bouncing off it. Hold a thin piece up against a window and it
lights up from inside; the thick parts of the same block stay stone. That difference is most of what
makes marble look like marble rather than like painted concrete — and this game has been drawing all
of it as painted concrete.

The odd part: **the building has always said its marble was marble.** There is a number in every
material for how far light gets inside it, the facility set it years ago, and no part of the
renderer ever read it.

It reads it now, and the way it does is the part I'd want you to know about. **Nothing anywhere says
which parts of the building are thin.** A moulding's lip is thin and the block behind it is not, and
no file records that — so the game finds out by firing a ray *into* the stone toward the sun and
counting how much stone it has to cross before the light is used up. Thin: the light gets through
and the surface glows. Thick: it doesn't, and the surface stays stone.

**And this time I could check it.** There is a test scene somebody built for exactly this — the same
stone as five panels, from one voxel thick to a hand's breadth, with the sun behind them, plus one
panel with the effect deliberately switched off as a control. I got the game running on this machine
without a graphics card (slowly — four frames a second) and photographed that scene with the feature
on and off. The five panels come out in order: the thinnest brightens most, each thicker one less,
and **the control panel does not move at all**. That last one is the result that matters — it means
the game is responding to the actual thickness of the stone rather than to the word "marble".

In the building itself this is the mouldings, the balusters, the statues' hands, the thin lip on
every cornice: with the sun behind them they now carry light instead of going flat dark.

What I still cannot tell you is what it costs in frame rate — that needs your machine.

## The glass got looked at, and it does what glass does

Last time I wrote that glass now bends what is behind it — and then had to admit nobody had ever
looked at it. The maths was tested; the picture was not. That is the wrong way round for a change to
something you look at, and it stayed that way because every scene with glass in it crashed on this
machine after eight frames, while the one marble scene ran fine.

**Eight frames looked like a limit on how big a scene this machine could draw. It is not — it is one
feature.** The thing that dies is the part of the renderer that gives shiny surfaces a *direction*
to reflect in: metal, polished stone, glass, water. Marble and rough stone never ask for it, which is
why the marble scene was the only one that ran. Turn that one feature off and every scene runs here,
all the way to a settled picture.

So I turned it off and took the glass picture. **A pane of glass moves what is behind it.** In the
test scene there is a wall of two colours with a pane over part of it: where the pane covers the
wall, the line where red meets blue and the wall's bottom edge both step sideways; where the pane
stops, they run straight again. The pane's own top edge cuts a step into the top of the wall behind
it. That is refraction, and it is the whole of what the change was supposed to do.

**Two things are still owed.** Glass with its reflections switched back on — that is what a real
window is, and it needs your machine. And the frame-rate cost of both this and the marble, which
also needs your machine.

## There is a new download: v0.8.0

Everything above is in it. The two you will notice are the glass and the marble — a pane now moves
what is behind it, and thin marble carries light through it where thick marble stays stone.

Same as last time: get `WorldShaper-v0.8.0-windows-x64.zip` from the releases page, unzip it
anywhere, run `WorldShaper.exe`. If you already have v0.7.1 open, it will tell you there is a newer
one and **F8** installs it.

Windows will still warn about it, because the file is not signed by a certificate authority — that
part has not changed and is explained on the release page. What is on the page beside the download is
a statement signed by GitHub tying that exact file to the exact code and the exact build that made
it, plus its checksum, so "is this the real thing" is a question you can answer rather than trust.

The build was made by the workflow rather than by hand, and before it published anything it unzipped
its own download somewhere else and made the facility build out of it — the check that exists
because v0.7.0 shipped without the building in it.

## The strip along the bottom now says what you are painting with

Q and E change the material, and until now nothing on screen told you which one you were on. You
pressed a key, something changed somewhere, and the only way to find out what it was was to cut with
it. The strip along the bottom of the screen — the one with the nine tool slots on it — now ends
with the material's name, in the words the world's own file uses, and how far through the list it
is: `Q/E alabaster 3/5`.

The `3/5` is there for a reason. Twice in this project a report of "changing material does not work"
turned out to be a list with only one thing in it, and a key that steps a list of one looks exactly
like a key that does nothing. If it ever says `1/1` again, that is what happened, and it says so
without anybody having to open a log.

Where a material was never given a name in the file, it shows its number instead — a blank there
would look like the game failing rather than like a material nobody named.

This is on the temporary strip, not the real interface, and it goes when that arrives.

**It is in v0.8.1**, which is the download on the releases page now — or press **F8** if the game is
already telling you there is a newer one.

## The whole building now draws on a machine with no graphics card

This one is about how the work gets checked rather than about the game, and it is the reason the
last few reports have had pictures in them at all.

The machine these sessions run on has no graphics card. Everything the game draws has to be done by
the processor instead, which is roughly a thousand times slower — and until yesterday it also
crashed after eight frames on any scene with glass, water or metal in it, which is every scene worth
looking at. Yesterday that turned out to be one feature: the part that gives shiny surfaces a
direction to reflect in. With it switched off, **the facility itself now draws** — all hundred and
twenty-five million voxels of it, the room with the two urns and the arch, in about twelve minutes
a picture.

So a session with no card can now photograph the actual building, at the actual camera the
performance numbers are written for, and compare two versions of it. What it cannot do is tell you
what any of it costs in frames per second, or show you anything about metals and reflections — that
still needs your machine.

**And the first thing that was measured that way came out empty**, which is worth saying because the
alternative is only ever reporting the things that worked. When a surface first comes into view the
renderer fires a single ray at the sun and believes the answer, which is a coin toss; every other
kind of light on a face starts from a sensible guess borrowed from the bigger surface it was part
of, and the sun was the one that did not. Giving it that guess is two dozen lines and it is in the
build, switched off. Six runs — three each way, same building, same camera, same frame — could not
tell the two apart: the difference between the two versions was smaller than the difference between
two runs of the *same* version. On your machine, at full resolution, it may well be visible. Here it
is not, and the honest thing is to leave it off until somebody can see it.

## Distant railings stop being staircases

Stand a long way from a railing and each post is thinner than a single dot on your screen. The game
had one answer to that and it was the wrong one: a dot either was the post or it wasn't. So a far-off
railing came out as a row of solid bars with hard jagged edges, and every one of those edges crawled
and shimmered as you moved — the same effect as an old photograph of a fence taken through a
window blind.

The reason it was the wrong answer is that the game already knew better. When it builds its
summaries of the world — the coarse blocks it draws distant things out of — it works out, for each
of the six directions you could look at a block from, **exactly what fraction of it is solid from
that side**. That number has been sitting in the file for two years with nothing reading it, because
the note explaining what it was for described a different number that used to be there.

Now a distant block that is only half solid is drawn as half solid: the ray carries on past it, finds
whatever is behind — another part of the building, or the sky — and the two are mixed in the right
proportion. A thin post covering a third of a dot contributes a third of its colour, and the dot
keeps two thirds of the sky.

**What you should see:** distant railings, window bars, cornices and crate edges look like thin
things rather than like blocky ones, and the edge where a far-off building meets the sky stops
crawling when you walk. Nothing within about twenty metres of you changes at all — up close each dot
is one cube and there was never anything to smooth.

I could check this one here, which the last few could not be. It needed a new test scene, because
every scene small enough to draw on a machine with no graphics card is four metres across and
everything in it is close up. With one built at the right distance, the only part of the picture that
changes between the old way and the new is the railing itself, down to the pixel — and, magnified,
the posts stop being blocks. `--no-edge-aa` puts it back exactly as it was if you dislike it.
## The stripes on a distant wall

Look at a wall from far enough away and the renderer stops drawing it voxel by voxel — it draws the
average of a bigger and bigger box, because at that distance a box is all a pixel can hold. That is
what makes the whole building affordable to draw at once.

The awkward part has always been the moment of changeover. A wall twenty-five metres away wants
boxes half a metre across and a wall twenty-six metres away wants them a metre across, and the wall
that runs between the two has to be both. What it did was alternate: a fixed four-by-four pattern of
pixels, some drawn at one size and some at the other, which is an old and honest trick — it keeps
the picture steady when you stand still, because a pixel always makes the same choice. But the two
sizes are different average colours, so you could see the pattern, and walking towards the wall made
it crawl across the surface.

Now the two sizes are mixed instead of alternated. A pixel that is three quarters of the way from
one size to the next draws three quarters of the way between the two colours, and both halves of the
pattern arrive at the same answer — so there is nothing left to see a pattern in. The choice of
which size of box to actually *hit* is untouched, and that is deliberate: that choice is also what
decides how much of the pixel the wall covers and which patch of surface gets its own lighting, and
those are not things to change while fixing a colour.

**Two honest limits.** The first is that this only does something where the two sizes really are
different colours, and on a building made mostly of one stone that is a narrower band than it
sounds. The second is that this machine cannot show it to you: with no graphics card, two runs of
the *same* build differ on four fifths of the picture, because the lighting is still settling when
the photograph is taken. What could be measured is the colour the renderer chose, before any light
touched it — nine per cent of the picture moved, by about a tenth of the way from black to white,
and every number that describes the *shape* of what was hit came back identical. On your machine,
at full resolution, the place to look is a large flat surface at middle distance while you walk
slowly towards it.

## The speckle on anything you have just turned towards

This is the other half of the sun's guess three sections up — the one that is in the build and
switched off. This half is in and switched on.

When a surface comes into view the renderer fires one ray at the sun to find out whether the sun can
reach it. One ray has only two possible answers — yes or no — so a wall that is really nine tenths in
shadow comes out as a scatter of black and white faces, each of them wrong, until enough rays have
been fired to average them out. That takes a few frames, and it is a few frames on **every** surface
you turn towards, walk up to, or reveal by cutting something away. Measured on the close camera of
the facility: **135,071 of its 589,870 surfaces, twenty-three per cent, had not yet fired the four
rays it takes to have an opinion**. That is what the speckle is made of. It is not the shadows being
wrong; it is the shadows not having been measured yet.

What has changed is that the renderer now knows the difference between an answer and a guess. Every
small surface sits under a bigger one — one coarse surface covers five hundred and twelve of them —
and that bigger one was measured long before its children were even found. So a surface with one ray
to its name is now drawn mostly as its parent, a surface with two is drawn half and half, and by the
fourth ray it is entirely its own. Nothing extra is traced: it is the same rays, read more honestly.

**The number four matters and it is not a dial.** Four is where the renderer already stops treating a
surface as new. Stopping there means a real shadow edge — the soft edge under a cornice, which takes
about four frames to resolve — is left exactly as it was, to the last decimal place. A longer blend
would have smoothed the speckle further and taken the edges of your shadows with it, which is the
trade this deliberately refuses.

**And it was nearly shipped doing a third less than it should**, which is worth saying because the
thing that caught it was not a picture. Measured on the small test scene here, the change moved so
little that it could have been switched off and nobody would have known: the difference between
having it and not having it was smaller than the difference between two runs of the *same* version.
Rather than look at the pictures again, the renderer was made to COUNT what it was doing — how many
surfaces asked for a parent to lean on, and how many were turned away. The answer was that **sixty-four
per cent of them were being turned away**, because the parent has to be reasonably well measured
itself and the test for that had been copied from somewhere it meant something different. Loosened to
the honest test — is the parent better measured than the surface asking? — it now serves a third more
of them. The remainder are the first two or three frames after you spin round, where nothing above is
any better informed either, and those fill in immediately afterwards.

**What could not be checked here is the building itself.** The machine these sessions run on shares
its memory with several others, a picture of the facility needs about nine gigabytes, and three
attempts were killed by the system part way through — one of them sixteen minutes in. The facility is
where the twenty-three per cent was measured and it is where this should show best, so that is the
first thing to look at on a machine with a graphics card.

## You can look at the clips on your phone now

**<https://worldshapergame.github.io/WorldShaper/>**

Every clip in the project — the whole facility, each of its twenty parts, and the little test
scenes — is now a web page. Open it on a phone, pick one from the list, and drag to turn it round.
The slider along the top cuts the clip in half, and you can slide the cut all the way through: the
front wall goes and you are looking into the rooms, with the stone showing solid where it has been
sliced rather than hollow. Press **Walk** and you are standing on the lawn at eye height. Walk up
the steps and in through the bronze door. The up button jumps; tap it twice and you fly, and then
the two buttons take you up and down.

**It rebuilds itself.** The parts of the building are being worked on all the time, and the site
follows: every time one is changed and pushed, it is rebuilt within a few minutes, and if you have
that part open on the page it reloads itself with the camera exactly where you left it. So you can
watch a piece of the building change while somebody is changing it.

**And it opens straight away, even when it is busy rebuilding.** Working out a building of this size
one voxel at a time is real work, and the first time it is done it takes minutes however it is
arranged — so it is spread over twelve machines at once, each taking every twelfth clip, and none of
them redoes a clip that has not changed since last time. Ahead of all of that, the page itself goes
up within about half a minute, showing every clip that has been built before. The new ones appear on
the list on their own as they finish. Nothing to reload, and never a blank site while it works.

The materials are real — glass is see-through, bronze and gilt look like metal, the copper dome is
green, the marble is polished. What it does *not* do is the light. The game traces light properly
and no phone can, so the site draws the light in advance and paints it on: the inside of a room is
dark because it has been worked out in advance that little sky reaches it, rather than because the
light got there. It is the difference between a photograph and a very good drawing. Everything about
*what the building is* is exact — it is built by the same code the game builds it with, from the
same files — and everything about *how it is lit* is an approximation.

**It found a real fault, and not in the website.** When each part of the building is drawn on its
own, the dome came out as a twelve-metre saucer less than a metre tall, in the wrong colour. The
building is written to be positioned by one line that moves the whole thing down 3.5 metres so that
where you spawn is the middle of the rotunda floor; that line moved the building and the paint, and
did not move the *names* of the parts. Nothing had ever asked for a part by name afterwards, so
nothing had ever noticed. It is the kind of mistake that produces no error and no crash — just a
wrong picture — and a picture is exactly what was missing until now.

## The website now draws light the way the game does

The paragraph above says the site does not do the light, and it is now half out of date, so here is
what changed.

Fifteen separate pieces of the game's own way of lighting a scene have been ported to the website
and put into one build. What that means to look at:

**The sun casts shadows.** Before, a wall was bright if it faced the sun and dark if it did not,
whether or not the building was in the way. Now the portico throws its shadow across the steps and a
column throws one across the wall behind it, with the soft edge a real shadow has — the sun is half
a degree wide, so a shadow thrown three metres has a crisp edge and one thrown forty metres has a
soft one, and both are drawn.

**Corners are dark.** A coffer, a flute in a column, the back of a niche — every recess in the
building is now darker than the flat wall beside it, which is what makes carved stone read as carved
rather than as a photograph of stone.

**Light bounces, and it carries colour.** A white vault over a porphyry floor now goes faintly warm,
because the light that reached it came off the floor. That single effect is most of the difference
between a room that looks lit and a room that looks painted.

**A polished floor reflects the room.** Not a picture of the sky — the actual posts, walls and
furniture standing around it. Two metals a polish apart now read as two different metals.

**Glass has something behind it.** Look through a pane at an angle and what is behind it moves, the
way it does through real glass, and a thick piece of coloured glass takes more colour out of what
passes through it than a thin one does.

**Lamps light the room they are in.** A sconce in one hall now lights that hall and not the one next
door.

**Cutting the building open shows the right stone.** Drag the slider through a wall and the cut face
is the material actually there — porphyry where the bands are, limestone where the wall is — instead
of one colour for the whole clip.

**And the ◉ button's colours are the clip's own.** That button draws the building as it was written,
with no voxels at all, at infinite sharpness. Until today its colours were made up: every shape got
*a* material rather than *its* material. Now it runs the clip's real paint rules at the point you
are looking at, so the moss is where the moss is. The two views will never agree pixel for pixel —
one asks "what is at this exact point on the surface" and the other asks "what is at the centre of
this voxel", and where a pattern is finer than a voxel those are honestly different questions — but
both are now answering with the same rules.

One piece was deliberately left out: a faster version of the shader for weak phones, which needs
rebuilding against the new material model rather than merging. Nothing looks different without it.

## The facility is not one building any more

Until now "the facility" meant one thing: the big neoclassical block with the dome, and everything
around it was empty ground. The other buildings existed, but each one lived in its own separate
file, on its own patch of nothing, and you could only ever look at one at a time.

They are being moved into the same place. Four of them are in so far:

**The bell tower** stands off to the east, about thirty metres from the block, an eleven-metre
square of lawn and gravel around its foot.

**The grotto** is away to the west and slightly north — the sunken, encrusted one with water in it.

**The garden theatre** is west as well, the big one: a curved bank of seating, a stage, and clipped
hedge wings either side. It is the largest of the four by some way.

**The glass pavilion** is at the far north end, on an island in a still basin, with its floor set
lower than the ground so the water sits sunk into the garden rather than perched on top of it. That
is what it is meant to do, and it is the sort of number somebody tidies up by accident later, so it
is written down in three places now.

Two of them are not standing where their own files said they should. The theatre was drawn to sit
somewhere that turned out to run straight through the west wing of the main block, and it also
landed on the grotto — so the theatre moved six metres west and the grotto moved two metres north.
Neither collision was visible until they were in the same frame, which is rather the point.

**What to look for:** they should be four separate buildings on one estate, each on its own ground,
none of them touching. The bell tower to the east, the grotto and the theatre to the west, the
pavilion straight ahead to the north past the fountain.

Three more are still to come: the colonnade, the fountain and the orangery.
