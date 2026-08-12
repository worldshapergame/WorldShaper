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
