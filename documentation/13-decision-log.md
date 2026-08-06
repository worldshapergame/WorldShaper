# 13 — Decision Log

Decisions made, when, and why. A decision here is settled; reopening one is fine but costs a note explaining what changed.

---

## Round 1 — after the first answer pass

### Locked by your answers

| # | Decision | Source |
|---|---|---|
| D01 | C++20, Vulkan 1.3, Lua 5.4 modding | A1 |
| D02 | Windows primary, Linux kept building (Steam Deck), no macOS | A5 |
| D03 | **Minimum spec: Steam Deck**, 1280×800 / 30 FPS locked, dynamic resolution allowed | A6, D1, D2 |
| D04 | Dev target: RTX 5060 Ti 16 GB, 1440p / 60–90 | A4 |
| D05 | The Unlicense, open source, Steam release eventually | A10–A12 |
| D06 | 32 players, distributed region authority, no host | B11, J1, J4 |
| D07 | Simulation tick 20 Hz, GPU, integer-only | E1, E2 |
| D08 | Fluids: pressure + momentum + current + mixing | E4, E5 |
| D09 | Gases: mixing, buoyant, **exactly conserved** | E6 |
| D10 | Full heat diffusion including air temperature | E7 |
| D11 | Explosions as propagating pressure waves | E14 |
| D12 | Verlet for cloth/rope/soft bodies | E12 |
| D13 | Fluids soak into cloth/soft bodies and leave stains | E13 |
| D14 | Rigid bodies dent *and* fracture; auto-detach on cut; no hard limits | E9–E11 |
| D15 | Euphoria-style active ragdoll with balance and protective behaviours | B2 |
| D16 | Characters: freeform voxel sculpt + automatic rig + procedural gait | G2, G4 |
| D17 | Full-body first person, camera between the eyes | D15 |
| D18 | Diegetic (in-world) UI, pixel font | D16, L3 |
| D19 | Chisel as the only build tool for now (drag a box, place or carve) | H1 |
| D20 | Creations are "clips", saved to a personal library, importable/exportable | H2, H4, H5, B8 |
| D21 | Single `.wsworld` file per world, saved on every edit, zero stall | K1, K3 |
| D22 | Terrain: fractal open-ended biomes, node-graph world-gen editor, flat world stays shipped | F1, F2, F5, F6 |
| D23 | Water bodies born settled, dynamic only when disturbed | B10 |
| D24 | Logic: node-graph programming + physical, destructible, physics-attached wires and components | I1, I3 |
| D25 | Vehicles are player-built; momentum is inherited when jumping off moving objects | I5 |
| D26 | 2 bounces + sky default; any voxel can emit light | D4, D6 |
| D27 | Volumetric fog, and the full physically-derived post stack incl. speed-based motion blur | D13, D14 |
| D28 | Fast approximations preferred over reference-correct light transport where they look the same | N4 |
| D29 | Connectivity: IPv6 → LAN → STUN hole punch → peer relay → Steam relay later; no infrastructure of ours ever | M1, M2 |
| D30 | Invite codes carrying a chosen username, duplicate names suffixed | J2 |
| D31 | Long, thorough foundations authorised | N6 |
| D32 | Out of v1: audio, weather, vegetation, chat, localisation, permissions, anti-cheat | E16, B9, F9, J8, L6, J9, J7 |

### Made by me, on your instruction to decide

| # | Decision | Question | Reasoning |
|---|---|---|---|
| D33 | **Structural integrity by support propagation**, not a stress solve | E8 ("you decide") | Cheap enough to run on a Steam Deck, produces convincing collapses, and is a strict subset of a stress solve so it can be upgraded later without changing any interface |
| D34 | **Logic gets its own event-driven tick**, synchronised to the sim tick, not polled inside the cellular pass | I2 ("whatever is better") | Logic is sparse and event-shaped; running it inside the cellular pass would cost a full-grid sweep for a handful of active wires |
| D35 | Network budgets: ≤120 KB/s upstream worst case, ≤25 KB/s typical | J10 ("whatever you choose") | Fits the slowest common home upstream (~1 Mbit) with headroom, using gossip trees so the number does not grow with player count |
| D36 | Voxel type ids are **32-bit** (4.29 billion distinct types per world) | C1 ("infinite") | True infinity is not storable. 32 bits is far beyond any real world's needs and keeps bricks compact; widening to 64 later costs ~15% brick memory |

### Design resolutions for "infinite" answers

| # | Your answer | How it is delivered |
|---|---|---|
| D37 | Per-voxel properties, colour, tags, transparency (C2, C4, C11) | **Interned voxel types** — identical voxels share one record automatically, unique voxels cost one record each. Genuinely per-voxel, genuinely affordable |
| D38 | Infinite per-voxel state fields (C3) | **Registry-driven per-brick layers**, allocated only on bricks that use them. Mods can register new fields |
| D39 | Infinite materials (C1) | 32-bit ids, see D36 |
| D40 | Infinite rigid bodies / infinite voxels per body (E10) | No coded cap. Cost scales with *contacts*, not voxels; settled bodies bake back into world voxels; over-budget bodies sleep early rather than dropping frames |
| D41 | Reactions authored three ways (C7) | Data files and Lua at Stage 14; the node editor UI at Stage 20, reusing the logic node editor |

### Conflicts resolved

| # | Conflict | Resolution |
|---|---|---|
| D42 | "No steps at all" (N2) vs. any hierarchy having discrete levels | **Stochastic level blending** — dithered per-pixel choice between adjacent levels, resolved by temporal accumulation. Detail becomes a continuous function of distance with no discrete transition in the math |
| D43 | Lua uses floats; shared-state simulation must be integer-only | **Scripts emit ops, never mutate state.** Mods may use floats freely with zero determinism risk |
| D44 | Distributed authority (J4) + 32 players (J1) + no infrastructure (M2) | **Region ownership** (one owner per 64 m cell, leased and re-elected) + **gossip trees** instead of a full mesh + peer relay for unreachable pairs |
| D45 | Exact gas conservation (E6) vs. cost of tracking invisible smoke forever | **Atmosphere pool** — thin gas merges into a per-region tracked quantity. Still in the ledger, no longer per-voxel work |
| D46 | Frozen distant regions (E3) vs. "world persists" (B12) | Distant regions **freeze exactly and remember everything** — fluid levels, temperatures, velocities, stains — and resume bit-perfectly. No catch-up simulation, no settling while you are away. *Assumption flagged as question O2* |
| D47 | Logic that creates/destroys matter (I6) vs. conservation | Explicit **accounted source/sink ops**, so drills and printers are ledger entries rather than leaks |

---

## Round 2 — follow-ups

| # | Decision | Source | Note |
|---|---|---|---|
| D48 | License switched from The Unlicense to **MIT-0** | O1 | Same practical freedom, no public-domain-dedication ambiguity. Third-party notices still shipped |
| D49 | **Regions catch up when you return**, but a *closed* world does not advance | O2 | Enables weathering/aging of structures. Implemented as a bounded, deterministic, mass-exact fast-forward, not tick-by-tick replay — see `05-simulation.md` §12 |
| D50 | Explosions are propagating pressure waves | O3 | Confirmed |
| D51 | **Visual and behavioural voxel data are split.** Behaviour records dedupe and may auto-merge when near-identical; visuals never merge and support billions of unique combinations via per-brick visual layers | O4 | See `03-voxel-data-model.md` §2 |
| D52 | Chisel: **both** points are camera-distance based, scroll-adjustable while holding a key; distance 0 snaps to the aimed voxel. Extra constraint points via middle click that the shape must touch at its edge. Shapes beyond boxes come later | O5 | Full spec in `07-roadmap.md` Stage 5 |
| D53 | Survival hooks (per-player matter ledger, owner-side validation) built now | O6 | Nearly free now, expensive later |
| D54 | Gravity warping is a **voxel property**, not a special entity — point attraction and/or repulsion | O7 | Keeps the "everything is voxels" pillar intact |
| D55 | Character limits set by me: 0.5–4 m tall, ≤4× default volume, no fully invisible materials in multiplayer | O8 | Adjustable |
| D56 | A clip carries a flag for whether stamping produces world voxels or a free-standing physics object, chosen when saved | O9 | Basketball → object; house → world |
| D57 | **Unlimited mixed fluid components per voxel**, pay-per-use (1–2 inline, overflow sparse) | O10 | Each component ledger-tracked separately |
| D58 | Accept worst-case bandwidth + automatic animation-fidelity falloff for distant players | O11 | |
| D59 | Everything diegetic, including the main menu, except where it is prohibitively hard | O12 | Escape hatch: a flat fallback for anything that would cost weeks |
| D60 | `WorldShaper.exe`, `.wsworld`, `.wsclip`, `.wsmod` | O13 | |
| D61 | Performance overlay ships, off by default, toggled in settings | O14 | |
| D62 | **No Steam Deck hardware available** — perf gates run against a "Deck-equivalent profile" (locked resolution, clock/bandwidth-scaled budgets, simulated network limits) and are validated on real hardware later | O15 | Standing risk, logged |
| D63 | Stages 3–5 use a **scripted test scene** (rooms, towers, tunnels, overhangs), which doubles as the regression benchmark | O16 | |
| D64 | **Native compiled plugins allowed** alongside Lua | O17 | Same op-emitting rule; explicit per-mod consent; friends-only by default; sandbox limitations documented honestly |
| D65 | Mods auto-download from peers, **only from friends**, and only after showing exactly what will be downloaded | O18 | |
| D66 | One node editor, two node sets (world generation and logic) | O19 | |
| D67 | NPCs are player-authored via the same systems, with built-in examples made the same way and fully editable | O20 | |
| D68 | UI **visual style only** to be referenced from the deprecated project — "transparent opposing ink" look and its control tooltips. No structure, no code, no architecture | user, round 2 | Supersedes A8 for this narrow purpose. Needs a repository link — question O21 |

## Stage 5 decisions

| # | Decision | Source | Notes |
|---|---|---|---|
| D69 | **Undo is unlimited**, and is stored as the *ops that recreate the prior state* rather than as a copy of the voxels | user, Stage 5 | A snapshot costs four bytes a voxel, so unlimited depth would run out of memory. A description costs one op for a region of one material however large it is, so the price tracks how much information the edit actually destroyed. `world/region.cpp` |
| D70 | Undo and redo replay through `apply_op` and append to the op log like any other edit | D69 | Keeps "a world is generate(seed) + replay(ops)" true, so undo needs no special case in saving, replay or networking. Covered by a test that replays the log and compares hashes |
| D71 | Undo is **per player**; a player's undo never touches somebody else's edit | H3 | Two players editing the same voxels can still produce an undo that no longer describes what is there. That is a reconciliation question and is deliberately left to Stage 16 |
| D72 | The chisel preview is drawn **analytically in the resolve pass**, not as geometry | — | It is a description of an edit that has not happened, so it has no place in the world data. Costs 0.035 ms at 1440p and needs no vertex buffer, depth pass or second pipeline |
| D73 | Preview colours: **carving takes the inverse of the backdrop, placing takes the material's own colour.** Where the preview is hidden behind something, carving switches to the colour of the material it is about to remove and placing switches to the inverse of the material | user, Stage 5 | The outline is drawn through geometry deliberately — carving happens *inside* rock, so an outline that respected depth would be invisible in exactly the case the tool exists for |
| D74 | A single edit is capped at **8 million voxels** (a 6.25 m cube) | measurement | An edit runs in one frame and cannot be interrupted. At about 6 ns a voxel through non-uniform geometry, 8 M is a ~50 ms hitch; the previous 64 M cap would have been a multi-second freeze. Raising it means slicing the work across frames, which Stage 16 needs anyway |

## Stage 5 decisions — tools and the clipboard

| # | Decision | Source | Notes |
|---|---|---|---|
| D75 | **Nine tool slots. Tapping a number selects that slot; holding the number and scrolling cycles the tools on it.** Every tool starts on slot 1 | user, Stage 5 | The wheel is the most contested input in the game — the chisel takes it with G for its working distance, the clipboard takes it bare to slide a ghost, the free camera takes it for speed. Claiming it for tool selection only while a number is held is the only arrangement that never takes it from the tool currently in use |
| D76 | The chisel's **P** toggles whether placing overwrites what is already there; **O** toggles whether the first point lands against the face or on the voxel you are looking at | user, Stage 5 | P is a `WriteMask` on the op, not tool state, because a peer replaying the log has to reach the same answer and cannot see the tool |
| D77 | **Clip rotation is a three-shear (Paeth) decomposition, not a resample** | conservation | A nearest-neighbour rotation reads some source voxels twice and misses others, so a wall of 1,000 bricks comes out as 987 or 1,013. Three shears is a bijection, so the count is preserved by construction. Whole quarter turns are done separately by permuting axes, because tan(θ/2) runs away toward half a turn |
| D78 | **Rotation always bakes from the originally captured clip**, never from the previous result | measurement | Twelve successive 7.5° rotations of a re-rotated clip visibly chews the shape; from the original it is one operation however many times the key was pressed |
| D79 | A clip carries an **`inside` mask** alongside its voxels | D77 | A rotated clip is a tilted box in an axis-aligned array, so the array has corners that were never part of it. Those are not air — air is a material the clip can stamp — they are *outside*, and a stamp must not touch them |
| D80 | Three paste modes: **replace** (the clip's empty parts clear what they cover), **matter only** (its empty parts leave things alone), **into empty space only** (existing matter is never disturbed) | user, Stage 5 | The third is for trim and moulding: adding detail to a surface without eating it |
| D81 | **Grid snap (O) means the offset rounds to whole clip lengths**, so repeated copies tile exactly. It also switches the turn step from 7.5° to 11.25° | user, Stage 5, interpreted | Both angles divide 90° evenly (a twelfth and an eighth), so a quarter turn is always reachable exactly. The tiling reading of "aligned to the world grid" is an interpretation — it is the reading that makes the setting do something useful, and it is easy to change |
| D82 | **The ghost is marched in the resolve pass, against an uploaded copy of the clip** — not in the visibility pass, and not as world geometry | — | A ghost is not part of the world: nothing should collide with it, nothing should stream because of it, and nothing in the marcher should have to know it exists. Cost 0.17 ms at 1440p for a typical row of four, 0.79 ms against a 0.80 ms budget for sixteen copies each filling the frame — which is the case that sets the step cap |
| D83 | Interface positions come from **ImGui's display size, never the swapchain extent** | bug | The swapchain is in pixels and the interface is in the window's logical units. On a display with scaling those differ by the scale factor, which put the tool readout off the bottom of the screen. A one-line report at startup names the mismatch when it exists |

## Stage 5 decisions — the second pass on the clipboard

| # | Decision | Source | Notes |
|---|---|---|---|
| D84 | **The window is clamped to the desktop's usable area**, at its requested aspect ratio, unless a size was asked for explicitly | bug | `SDL_CreateWindow` takes *screen coordinates*, not pixels. On the dev machine's 1366×768 desktop the default 1600×900 window opened with its edges — and therefore the whole interface — off the screen. An explicit `--width/--height` still wins, so a scripted 1440p measurement is still 1440p |
| D85 | **The right button abandons whatever the clipboard is doing** — a selection in progress or a held ghost | user | One button meaning "not this" wherever the tool happens to be |
| D86 | **Middle click sends the ghost to the crosshair**, resting against the face that was hit | user | Scrolling a ghost across a room one voxel at a time is not a way to place something |
| D87 | **Scrolling accelerates while the run continues** and resets on reversing, changing axis, or stopping. The first click of any run is always exactly one voxel | user | Acceleration you cannot escape makes lining a ghost up to the voxel impossible; a flat large step makes crossing a room impossible. The step is taken from the heat *before* the current click is counted, which is what guarantees the first one is small |
| D88 | **Copies carry a share of the transform**: copy n of N gets n/N of the rotation and the resize, so the last one carries all of it | user | Turns a spiral staircase or a tapering spire into one gesture. The cost is that each copy is its own baked clip and its own GPU upload — so when there is no transform, every copy shares one shape, one bake, one upload |
| D89 | **Resizing is whole-number ratios only** (1/7 … 7×), and does *not* preserve the voxel count | user, mathematics | It cannot: making a thousand-brick wall twice as big means eight thousand bricks. What it does preserve exactly is the **shape** — growing replaces each voxel with a solid k³ block, shrinking replaces each k³ block with what it held most of. Fractional ratios are refused because rounding is exactly what puts stripes of doubled and missing voxels through a wall |
| D90 | **"/" cycles what "," and "." adjust** — copies, roll, resize. The arrow keys always turn | user, interpreted | Roll is otherwise unreachable, so it gets a home here rather than two more keys |
| D91 | The ghost march's **step budget is per copy, not shared** | bug | A shared budget was spent by the first ghost the loop reached, so every ghost behind it drew nothing — copies vanished exactly when they overlapped, which is when a row of them most needs looking at. Total is still capped: 0.24 ms at 1280×800 with sixteen overlapping copies filling the frame, against this pass's 0.80 ms budget |
| D92 | **The binary prints its compile time at startup**, and `build.bat clean` exists | bug | The build tool reported "no work to do" over a source file that had changed and left a stale executable behind, so a measurement was taken against code that no longer existed. One line makes that visible instead of silent |

## Stage 5 decisions — the third pass

| # | Decision | Source | Notes |
|---|---|---|---|
| D93 | **Down is C, not Ctrl** | user | Ctrl is half of every editing shortcut a person already knows. Reaching for Ctrl+Z and sinking through the floor is not something anyone should have to learn around. Cancel moves to Backspace |
| D94 | **Chisel on slot 1, clipboard on slot 2** | user | One tool per slot by default; stacking is what holding the number and scrolling is for |
| D95 | **Putting a tool away drops what the clipboard was holding** | user | A ghost surviving a trip through the chisel and reappearing later is a surprise, not a convenience |
| D96 | **The clipboard only claims the wheel once it holds something.** With nothing selected the wheel is flight speed | user | Which is what you want while flying somewhere to make a selection |
| D97 | **Roll is gone**; "/" cycles "," and "." between copies and size | user | The arrows already turn the clip |
| D98 | **Resizing is any ratio, per axis, uncapped down to a single voxel** — 4.5×, 9.843×, whatever. In resize mode the up and down arrows stretch the axis you are facing | user | Replaces the whole-number-ratio scheme. The guarantee weakens accordingly and is stated plainly in `game/clip.hpp`: exact shape preservation only holds at whole ratios; at any ratio the destination is never torn, because every output voxel is decided by the source region that maps onto it rather than by pushing source voxels forward and rounding |
| D99 | **Copies ramp the size geometrically**, not linearly | — | Half way to twice the size is 1.41×, not 1.5×, which is what makes a row of them read as an even progression |
| D100 | **Grid snap (O) means quarter turns** | user | Both 11.25° and 7.5° looked identical — neither is square to the world, so the setting appeared to do nothing. A quarter turn is the only rotation a voxel grid represents without resampling at all, so snapped now means exactly that, and the difference is unmistakable. Offset tiling stays |
| D101 | **Scroll acceleration is kept alive by a timer, not by consecutive frames** | bug | A wheel reports separate notches with idle frames between them, so decaying the speed on every quiet frame wiped it between notches and it never sped up. The run now survives a quarter second of quiet |
| D102 | **Clips carry an occupancy mask**, one byte per 8×8×8 block, uploaded with the cells | user | A selection is mostly air. Without it a ray entering a clip walks every empty voxel to the first thing worth drawing, so a large ghost exhausted its step budget before reaching anything and drew blank. A 120-voxel cube of mostly air now ghosts in 0.09 ms and shows its contents. It costs about 15% in the opposite case — sixteen small dense clips overlapping — because the extra read happens whether it skips or not |

## Stage 5 decisions — the fourth pass

| # | Decision | Source | Notes |
|---|---|---|---|
| D103 | **Air gets no vote when a resize shrinks a clip** | user | A plain majority deletes exactly what is worth keeping: shrink a one-voxel-thick diagonal by half and every block it crosses is seven-eighths air, so the slope disappears. The same arithmetic shaves a right-angled corner (one voxel of eight) and thins a one-voxel wall away. If any matter falls in the region the answer is matter, and which kind is settled among the materials present. The cost is a shape gaining slight bulk rather than losing pieces — the right way round to be wrong |
| D104 | **Transforms are applied turn-then-resize**, not resize-then-turn | user, measurement | Two reasons. Correctness: resizing first stretches the clip's *own* axes, so after a quarter turn "stretch the way I am looking" grew a different axis. Cost: three shear passes over an enlarged clip means growing three times costs twenty-seven times the shearing for the same result |
| D105 | **The size limit is applied before anything is baked**, and as one number across all three axes | user, bug | Growing past the limit used to build the oversized clip, time how long that took, throw it away and show the *unscaled source* — an original-sized copy dropped into a row of resized ones. Clamping axis by axis was a second bug: it spent the whole budget on x, so "twice as big" came out as thirty times along one axis |
| D106 | The accumulated rotation is **not wrapped at 360°** | user, bug | The copies carry fractions of it, so wrapping snaps the whole row back at once. Past a full turn the row keeps winding, which is how a spiral of more than one revolution is made. Clamped at ten turns each way |
| D107 | The quarter-turn fold **subtracts before masking** | user, bug | It masked first, so four quarters became zero, nothing was subtracted, and the shears were handed an angle near a full turn. Only visible above 315°, where a smoothly rotating row collapsed all at once — which is exactly how it was reported |
| D108 | **`kMaxBakedCells` is three million**, from measurement | measurement | Baking runs at roughly 16 ns a cell, so three million is about fifty milliseconds — a visible hitch when a resize key is held with a full row out, and the point past which it stops being a hitch and becomes a stall |
| D109 | **O is about position, not rotation.** On, the clip moves in whole clip lengths so it carries its own spacing; off, a voxel at a time. The turn step is always 7.5° | user | Reverses D100. Twelve presses is a quarter turn exactly, so a right angle needs no snapping mode |
| D110 | **A stamped clip is on the world lattice, always. A clip with its own lattice is a free-standing object, not world voxels** | user question | Answered in full in `12-plain-english.md`. A voxel world has exactly one grid — that is what makes it a voxel world, and everything downstream is built on it: bricks, the octree, the occupancy masks, the marcher, residency, the op log, replay. What the engine *is* designed for is the other thing: D56 already says a clip carries a flag for whether stamping produces world voxels or a free-standing physics object. That object keeps its own grid at its own angle and offset. It arrives with rigid bodies in Stage 12, and nothing about the current design blocks it |

## Stage 5 decisions — the fifth pass

| # | Decision | Source | Notes |
|---|---|---|---|
| D111 | **O restores the 11.25° turn step** alongside the position tiling; free is 7.5° | user | Reverses the "always 7.5" part of D109. Both divide a quarter turn evenly — an eighth and a twelfth — so a right angle is exactly reachable either way. The position tiling stays, and D110 still stands: a stamped clip is on the world lattice |
| D112 | **The copy count is signed.** Positive fills the gap between the original and the ghost, with the last copy landing on the ghost. Negative continues *past* the ghost, repeating the step | user | Zero is not a value the count can hold — no copies at all is what dropping the clip is for — so the counting runs on a line with zero removed: …−3, −2, −1, 1, 2, 3… |
| D113 | **Extrapolated copies bend rather than run straight when the ghost is also turned.** Each step is the move from the original to the ghost, turned by however much the clip has turned by the time it is taken | user | A straight move repeats in a straight line; a move that also turns walks the row round a curve. Nothing is rotated about a pivot — each copy is one more step from where the last ended. Checked by a test that turns a quarter per step and asserts four steps close a square |
| D114 | Interpolated copies stay **linear**, not composed | — | Composing would mean the last copy no longer lands exactly on the ghost when the row is turning, and aiming the ghost has to aim the row |

## Stage 5 decisions — the sixth pass

| # | Decision | Source | Notes |
|---|---|---|---|
| D115 | **Every cap on the clipboard is gone**: no limit on selection size, on how far a clip can be resized, or on how many copies it can have. The floor is one voxel on an axis, because a clip with a zero-width side is nothing | user | What replaces the caps is a refusal to attempt what the machine cannot do: a clip costs five bytes a cell, so an allocation that fails is caught and reported instead of taking the game down |
| D116 | **Only the copies that are drawn are baked and held; stamping bakes each copy in turn and lets it go** | D115 | This is what makes an unlimited count affordable. A thousand copies do not mean a thousand clips in memory — the preview holds at most `kMaxPreviewInstances`, and putting a thousand down costs one clip at a time. Copies that were never drawn are stamped exactly as those that were |
| D117 | The GPU clip pool is a **preview budget, not a limit**. A clip too large for it is still selected, transformed and stamped exactly; it is drawn as an outline rather than as voxels | D115 | The shader's parameter block holds sixteen ghosts, so past that only some are drawn — and the one being steered is always among them, or the tool has no cursor |
| D118 | With copies past the ghost, **the ghost is the last of the chain, not the first** | user | Dragging the far end of a row and having the row follow is the useful gesture. Steering the first link and watching the far end fly off at a multiple of the input is not |
| D119 | The step of a bent chain is **solved for, not assumed**: `(Σ R(i·θ)) · step = offset` | D118 | So the last copy lands exactly on the ghost however much the chain is bending. With no rotation the sum is `count` times the identity and this reduces to `offset / count` — the straight case falls out of the same expression instead of being special-cased. A closed loop makes the matrix singular, and that falls back to an even spread |
| D120 | The repository is **public at github.com/worldshapergame/WorldShaper** | user | MIT-0, as decision O1 set. `build/` is ignored; the tree is under a megabyte |

## Releases and distribution

| # | Decision | Source | Notes |
|---|---|---|---|
| D121 | **The version lives on the `project()` line and nowhere else.** The window title, the executable's version block, the startup log, the release tag and the update check all read it from there | — | The release workflow **fails a tag that disagrees with the source**. That check is what makes the update comparison meaningful: a release called v0.6.0 cannot contain a build that thinks it is something else |
| D122 | **The game checks for updates on startup and offers them; F8 installs. Nothing downloads on its own** | user | An update that installs itself while you were trying to play is not a feature. The check runs on a background thread, is silent on failure, is off in debug builds and behind `--no-update-check` |
| D123 | The updater speaks **HTTPS through the operating system** (WinHTTP), looks only at this repository, and validates the download URL against the exact prefix a release asset here has | D122 | No HTTP library to depend on, licence or patch — which the free-to-publish constraint (documentation/08) makes a real consideration. And a hostile or broken reply cannot redirect the download somewhere else |
| D124 | The running executable is **renamed aside, never overwritten**, and the old one is swept up on the next start | — | Windows would not allow the overwrite anyway; doing it deliberately means a failure leaves a working game rather than none |
| D125 | **Code signing is wired but not done.** It needs a certificate bought from a certificate authority, which is a cost and an identity check that cannot be carried out on someone else's behalf | honesty | Two repository secrets turn it on with no other change. Until then that step is skipped and the rest of the workflow runs |
| D126 | What ships instead: a **build provenance attestation** signed by GitHub, a **SHA-256**, a **version block** and an **icon** | D125 | None of it removes the SmartScreen warning, and the release notes say so plainly. The attestation is in one way stronger than a signature — a signature says who built a file, an attestation says which commit and workflow it came out of |
| D127 | A store is a **half-answer, and which half depends on the client** | user question | The warning comes from the Mark of the Web, which browsers attach and store clients do not. A zip downloaded from an itch.io page in a browser warns exactly as one from GitHub does; the same file installed through the itch app, or through Steam, does not. itch.io is free but only helps players who use its app; Steam helps everyone and costs $100 once. Written up in `15-releases.md` |

## Open items carried forward

- **O21.** Link to the deprecated WorldShaper repository (UI style reference only).
- Round-1 and round-2 questions are otherwise closed. Round-3 questions will be raised per stage as they arise.
