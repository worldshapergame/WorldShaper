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

## Streaming and marching after large edits

Three defects found from two user reports — chunks flickering after a large build, and faint dotted lines over a flat surface. All three share a shape: something the code could only have learned by being *told*, and was not.

| # | Decision | Source | Notes |
|---|---|---|---|
| D128 | **The brick-slot budget is counted in slots, not in bytes.** `max_bricks` is derived from the slot size, not from an assumed payload per brick | measurement | The old figure assumed roughly a kilobyte of payload per brick. A large flat build is almost entirely *uniform* bricks, which cost eight bytes of payload and a whole slot each — so the payload pool read 8 MB of 1 GB while slots ran dry. 651,465 resident bricks over 128 chunks rounds to the 8192 slot class, and 8192 × 128 is exactly 1,048,576: the cap, hit to the byte. The visible symptom was chunks blinking in and out forever, because eviction cannot free what the wrong pool is short of |
| D129 | **A full block pool hands out a larger block rather than refusing.** The upgrade is recorded so release returns it to the class it came from | D128 | Refusing while holding free space in other classes makes the caller evict and retry, and evicting a chunk with a small brick run does nothing for a chunk that needs a large one — so it evicts again, forever. Returning the block to the wrong list would be worse than the bug: the next few allocations from that list would overlap it |
| D130 | **Slot-run size classes gain halfway steps** (…1536, 3072, 6144, 12288, 24576…) | D128 | Rounding waste falls from 2× to 1.5× |
| D131 | **An edit *pushes* invalidation to streaming, and the invalidation is remembered until it is served** | user report | This cannot be pulled. Feedback reports chunks the renderer wanted and *could not find*; a chunk that is resident but stale is found, so it is never reported, so it is never refreshed. A one-frame request is not enough either — the frame budget serves four chunks and a large edit touches hundreds, so all but the first four were dropped and stranded, drawing pre-edit contents until something unrelated evicted them. A chunk that is not resident is dropped from the set rather than uploaded: it arrives correct on demand, and uploading it early would starve the chunks actually on screen |
| D132 | **Inside a brick the ray marches at single-voxel resolution always. The detail level chooses the colour, never the shape** | user report | It used to step in 2- or 4-voxel cells at reduced detail, and a cell counts as solid when any of its voxels is — so a cell straddling a surface stands half a cell proud of it and a grazing ray clips its *side*. The geometric error is one voxel, below what the pixel can resolve by construction; the shading error is lit-top against dark-side, which is not subtle. On flat ground: 680 wrong pixels in 63,000 at level 1 against 2 in 270,000 at level 0. The ordered dither is what scattered them across the whole surface, by giving neighbouring pixels different levels — so the artifact read as faint dotted lines rather than as a band at one distance. Measured 1,722 wrong pixels before, 11 after, at 1.886 ms against 1.929 ms: marching a brick at voxel resolution costs at most 24 inner steps instead of 12, inside data already fetched, and did not show up in the frame time at all |

## Render distance

Reported as "far away voxels are no longer rendered and it's just sky, and it isn't fog". It was not a distance limit at all — streaming was dying, and the death was silent.

| # | Decision | Source | Notes |
|---|---|---|---|
| D133 | **Level 0 of the world-occupancy grid describes only a window around the camera. The levels above it describe the whole world** | user report | The two answer different questions and only one can afford to be wrong. Levels 1 and up are used to *skip* empty space: a collision there costs a skip that did not happen, never a wrong picture. Level 0 answers "the world has a chunk here and you do not have it", which is what the renderer reports and streaming acts on. A collision there invents a chunk that does not exist. It is rejected on the CPU, and because a ray reports only its **nearest** miss, the phantom permanently hides the real chunk behind it — so streaming asks for the same nothing every frame while the world stays unloaded. Measured on a 400 m platform: 5,438 reports a frame, **0 accepted**, resident frozen at 1,605 of 20,402, no eviction, no deferral, no memory pressure. After: converges to 437, feedback 0, phantoms 0. The old code called the collision case "wrong in the safe direction… free". It was neither |
| D134 | The window is **one less than half the grid**, so two chunks inside it can never collide | D133 | Correctness by construction rather than by assuming a streaming radius. The vertical dimension went from 16 to 64 as well: at 16 the window reached ±64 m, so flying higher than that put the ground outside it |
| D135 | **A rebuild sends only the cells that changed** | D133 | The grid now follows the camera, not just world edits, and re-sending all 1.3 million cells on every move is 21 MB for a window that shifted a few chunks. Almost every cell survives a rebuild unchanged. A grid the GPU never received still forces a full resend — correctness over traffic |
| D136 | **Phantom reports are counted and logged** | — | The stall was invisible: every counter that normally says "streaming is struggling" — evicted, deferred, out-of-memory — read zero, because nothing was struggling. It was asking for the wrong thing, perfectly happily. That state now has a number next to it |
| D137 | Past the window, chunks are **not streamed at all** — they are drawn from summaries above chunk level | D133 | This is the other half, and it is what "infinite render distance" actually requires: a chunk currently has to be *fully resident* to draw at all, so distance is bounded by memory however well streaming behaves. `03-voxel-data-model.md` §Node has always called for a sparse octree above chunks for distant rendering; Stage 4 built only its *occupancy*, not its colour, and was marked done anyway |

## Thumbnails, and unbounded render distance

| # | Decision | Source | Notes |
|---|---|---|---|
| D138 | **A chunk has a second, cheap representation: 8×8×8 cells of a cubic metre, colour plus coverage.** Two kilobytes against a chunk's megabytes | D137 | One metre is not arbitrary: a cell falls below one pixel at about 515 m and the full-detail window is 256 m, so the two *overlap* rather than leaving a band where neither is good enough. Bricks already know their own filtered colour, so building one never touches a voxel |
| D139 | **Coverage never rounds to nothing.** A brick with one solid voxel, and a cubic metre with one solid voxel, both report "present, however faintly" | — | `(1 × 255) / 512 = 0`, and zero is how "entirely air" is spelled. One voxel in a cubic metre is 0.003%. Without the floor a railing or a wire would not look thin at a distance, it would vanish — at exactly the range where you can no longer see well enough to notice. The same mistake as letting air win a majority vote when a clip is shrunk (D95) |
| D140 | **The thumbnail tier is pushed from the camera, not pulled from the view** | D131 | The opposite of full residency, deliberately. Pull is right for detail — it follows the view, so a chunk behind a wall costs nothing — but it can only ask for what a ray reached, and it took a stall that froze the world to make that obvious. The CPU knows the whole world and where the camera is, so it simply keeps the nearest chunks within a radius. No feedback, no round trip, nothing to deadlock. Affordable *because* a thumbnail is 2 KB: being approximate about which to hold costs almost nothing |
| D141 | Eviction is **by distance, and only ever displaces something further away than the candidate** | — | Otherwise the near set is evicted by chunks behind it and churns without settling |
| D142 | **The work list is rebuilt when the world changes, not only when the camera moves** | measurement | It was camera-only, so a region built *after* the list was made was never in it and never got a thumbnail however long you stood there. Measured: 98 thumbnails for a 20,402-chunk world, all of them the original test scene. Rate-limited to every 30 frames, because holding the mouse down edits every frame and the rescan walks the world |
| D143 | **The `empty_run > 24` guard is gone** | measurement | Described as "a guard against a pathological ray rather than a distance limit". It was both. Every pass advances the ray to at least the next block boundary, so it cannot fail to progress and `kMaxSteps` already bounds the loop — what the run bounded was *distance*: 24 consecutive single-chunk steps is 192 m, and near geometry the coarse levels have nothing large to skip. Looking down from 700 m, only rays going almost straight down reached the ground; everything at an angle gave up. It drew as one small square of world in the middle of an empty screen |
| D144 | The thumbnail upload gets **its own barrier**, and a partial upload **re-offers the whole batch** | — | `upload()` skips its barrier entirely when it staged nothing — which is exactly the case when the camera is outside the full-detail window and everything on screen is a thumbnail. And contents are sent before the grid: a grid entry pointing at contents that did not fit draws some other chunk from somewhere else, where losing the grid entry only draws nothing for a frame. Only one of those is a wrong picture |
| D145 | Thumbnails take **15% of the video-memory budget**, full detail dropping to 45% payload / 40% slots | D138 | They buy something the other two cannot buy at any price. Full detail is bounded by memory however it is split; past that bound the world is not drawn coarsely, it is not drawn. Measured on this machine: 78,033 slots, 153 MB, a 1.28 km radius |

**Measured.** An 800 m platform seen from 700 m up, which rendered as *pure sky* before: now the whole thing, from 20,402 thumbnails and **zero** resident chunks. Ground level on the same world: 3.9 ms GPU, 435 chunks resident. The default scene is unchanged at 1.3 ms.

## Three bugs found while attempting the tiers above thumbnails

The tiers themselves did not land (see below), but building them exposed three faults in what had already shipped. All three are the same shape as D131: something was sent, or reported, or reachable — and then quietly was not.

| # | Decision | Source | Notes |
|---|---|---|---|
| D146 | **A coarse-grid rebuild diffs against what the GPU has been *sent*, not against the last CPU state** | measurement | The grid follows the camera *and* world edits, so two rebuilds can happen before one upload. The second then compares the new state against the first one's state, finds them identical, and sends nothing — the first rebuild's changes are lost permanently. Exactly what happened at startup: built once for the initial camera, again on the first frame after the camera was placed, and the GPU received an empty grid. The marcher read the whole world as open sky, no ray reported a miss, and residency sat at **zero** chunks with feedback at **zero** reports. Every counter calm, nothing drawn |
| D147 | **A chunk is reported missing before its thumbnail is drawn, not after** | measurement | A thumbnail hit returns from the marcher, and the report was assembled below it — so the moment a chunk had a thumbnail it stopped being reported, streaming never fetched it, and it drew as one-metre blocks forever. The tier that exists to stand in for detail was suppressing detail |
| D148 | **Rays are clipped to the world's extent, not to the resident set** | measurement | The clip was written when a chunk had to be fully resident to appear at all, so clipping to residency plus a margin lost nothing. It does now: a thumbnail a kilometre out is real geometry. Widening it to a large fixed number instead measured **23.7 ms against 0.78** — the clip is not slack, it is what stops a ray that will never hit anything from stepping until its budget runs out. Bounded by the world it is exact and free |

## The summary octree, second attempt

The first attempt is below. It failed on one thing, and fixing that thing made the rest work.

| # | Decision | Source | Notes |
|---|---|---|---|
| D152 | **A node is folded from its eight children, never summarised from the world** | D149 | Level 0 covers a chunk at a metre a cell; every level doubles, so level k covers 2^k chunks with cells of 2^k metres, at two kilobytes a node throughout. Folding is exact: every voxel contributes to exactly one cell at every level, so coverage halves as cells double and floors at *present* rather than at zero. A single voxel is still visible nine levels up, where its cell is 256 m across — the test the sampling version failed |
| D153 | The recursion is **bounded by an index of which blocks contain anything** | measurement | Without it, building level 9 descends into eight children unconditionally: 8^9 is 134 million nodes for a world of a few hundred chunks, and the first version never returned |
| D154 | **A block carved empty gives up its slot** | measurement | The tree correctly answers "nothing here"; the cache skipped that case rather than releasing, so hollowing out a hill left the hill in the distance for good |
| D155 | The level is chosen by **rounding** what the pixel resolves, not flooring | measurement | Each level is held for a fixed radius *in blocks*, so using a level finer than intended halves how far it reaches. At 3 km that put the ground 94 blocks out against a 96-block radius — rays straight down found a summary and rays at any angle did not |
| D156 | **The step past a skipped block scales with distance** | measurement | The one that was actually stopping everything. A 32-bit float's own step at t = 96,000 voxels is 0.0156, so `t + 1e-3` rounds straight back to t: the ray stops advancing and spins on the same boundary until its budget runs out. From 3 km that was every pixel — the step-count view came back saturated, and raising the budget eightfold changed nothing, because the ray was not stepping slowly, it was not stepping. It is the same mistake as the fixed nudge behind the dotted lines (D132), at a thousand times the distance |

**Measured.** An 800 m platform from 3 km up: was blank at 19.4 ms, now drawn at **0.545 ms**. From 700 m: **14.2 ms to 1.05 ms**. The reference camera is unchanged at 0.77 ms with 98 of 98 chunks. Eight levels reach 98 km.

## Tiers above thumbnails: the first attempt, reverted

Blocks of 4, 16, 64 and 256 chunks summarised the same way as a chunk, to carry distance past the thumbnail tier's ~1.3 km. Built, measured, and reverted to `wip/summary-tiers`.

| # | Decision | Source | Notes |
|---|---|---|---|
| D149 | **A block cannot be summarised by sampling** | measurement | Sampling was the whole premise: a fixed number of samples per cell makes every tier cost the same to build, which is what makes a 256-chunk block affordable at all. It misses thin structure, and thin structure is most of a world. A one-brick-thick floor sampled at stride four is invisible: the tier drew nothing over ground that was plainly there. This is the same failure as D139 and D95 — air winning a vote it should not be in — one level up, where it cannot be patched by a floor value because the sample never sees the matter |
| D150 | Coarser tiers must be built **from finer summaries**, not from the world | D149 | Which needs a summary pyramid maintained as the world changes — a real subsystem, and the thing `03-voxel-data-model.md` §Node has been describing all along. It belongs with terrain (Stage 19), which needs exactly the same structure for generation |
| D151 | A tier is chosen by what the pixel resolves, and **never falls back to a coarser one** | measurement | Falling back is the obvious thing and is badly wrong: a near sky chunk has no thumbnail, so the ray drops a tier, and a two-kilometre block containing ground a mile away reads as occupied — drawing a 256 m blob a few metres from the camera. Testing every tier at every step instead measured **57 ms a frame**. Recorded because both mistakes are natural to make again |

## Path tracing, and shipping a build that only ran here

| # | Decision | Source | Notes |
|---|---|---|---|
| D157 | **A reference path tracer comes next, as Stage 6A — ahead of materials, and not as the shipping renderer** | user | You cannot judge a material you cannot see lit: sandstone and rusted iron under flat shading are two shades of grey-brown. It is a separate mode with its own pipeline, never in the frame path, accumulating while the camera is still, with no frame budget — seconds an image is the point, not a problem. The real-time pipeline is untouched, and its reason for existing stands: per-pixel path tracing cannot meet 9.5 ms on a Steam Deck, which is exactly why the face cache exists |
| D158 | It is **additive, not a reorder** | D157 | Nothing is skipped or renumbered, and nothing has to be built first — `VisualRecord` already carries albedo, opacity, roughness, metallic, IOR, emissive and tint, and Beer-Lambert absorption, in 16 bytes already on the GPU. It also makes Stages 7–9 *more* verifiable: direct light, bounces and caustics are all approximations, and an approximation with no ground truth is a guess |
| D159 | **Shipped builds find their shaders beside the running executable** | user report | They came from a path CMake baked in at build time, which resolves on the build machine and nowhere else. v0.6.0 passed 265 unit tests, both audits and the version check, and then opened a black window and closed on every other computer. The console it explained itself to closes with it |
| D160 | **Packaging ends by running the finished zip from a temporary directory** | D159 | Every other check ran the executable inside the build tree, where the baked path happens to work — so none of them was a player. This gate fails the way a player fails, and is the only one that would have caught it |

## The path tracer and the face cache

| # | Decision | Source | Notes |
|---|---|---|---|
| D161 | **Direct light is computed per pixel; only indirect is cached** | — | Direct sun has hard shadow edges that one value shared across a face would smear, and it costs one shadow ray. Indirect is smooth, expensive, and looks the same from anywhere on a face — exactly what is worth computing once |
| D162 | **A cache entry is keyed by a voxel face in world coordinates, and stores irradiance rather than colour** | user | World coordinates are the point: turning the camera does not throw the work away, where screen-space accumulation loses everything the moment you look elsewhere. Irradiance rather than outgoing colour keeps the entry independent of the material, so it stays correct when a surface is repainted — which for a game about repainting things is not a small thing |
| D163 | **An indirect ray reads the cache where it lands**, rather than tracing deeper | D162 | This is what makes the bounce count unbounded for nothing: what it reads there already contains its own indirect from previous frames, so light gains a bounce per frame and keeps it. No path is traced deeper and none has to be |
| D164 | Contributions are **clamped before they enter the cache** | — | A single path that finds the sun through a gap carries a spike. In a per-pixel average that is a firefly that eventually washes out; in a cache it does not average away, it persists — one face wears it for thousands of frames |
| D165 | Fixed-point sums with integer atomics, and a **capped sample count** | portability | Atomically adding floats needs an extension that is not core. 4096 steps per unit is finer than any display, and the cap keeps the sum well clear of wrapping |
| D166 | **The cache is cleared when the world changes, and never when the camera moves** | D162 | A face's cached light describes a world that no longer exists the moment something is carved beside it, and unlike screen-space accumulation it would never wash out on its own — a stale face keeps its old light until something evicts it, which is never |
| D167 | **The tool previews are drawn by the tracer too, from the same code** | user | A tracer you cannot edit inside is a screenshot generator, not a way to judge a material in place. Drawn after tone mapping and outside the accumulation, because a preview is interface rather than light: it must not be averaged in, must not converge, and must not be dimmed by an exposure curve |
| D168 | **One declaration of the parameter block, in `params.glsl`** | measurement | std140 lays out by position, so a shader declaring fewer fields than the buffer holds reads everything after the gap at the wrong offset — silently. That cost two debugging sessions: once a broken screen, once a wrong picture with no error anywhere |

**Measured.** Clean at 60 samples where the uncached tracer needed 500, at 5.5 ms a frame for 800×450. With the mode off the real-time path is unchanged at 0.779 ms and 98 of 98 chunks.

## Face cache: seeding, and where it goes next

| # | Decision | Source | Notes |
|---|---|---|---|
| D169 | **A node blends its parent's estimate in until it has samples of its own** | user report | The first look at anything is noisy because a new node has one sample. Its parent covers eight times the volume and has been gathering from everything around it, so it is a far better first guess than a single ray. Blended rather than switched at a threshold: a switch puts a visible seam wherever the threshold falls, which on a wall lit from one side is a line across it. Measured on a cold start: neighbour-difference noise 6.25 to 3.09 at two samples, 3.80 to 2.06 at four |
| D170 | **Every pixel contributes to its own node and its parent** | D169 | One extra set of atomics, and it guarantees there is always a parent to inherit from — including the first time anything looks that way. Without it, seeding only helps when a surface was seen from further away first |
| D171 | **Debug view 5 shows where the cache is working**: red fell back to per-pixel, green is held | — | This class of fault was reported twice and both times had to be deduced from a photograph. The view answered it in one frame, and the answer — misses rising with distance — was not the collisions everyone would have guessed |

### Radiance cache: built, and not the way this section expected

The plan below was written before it was tried, and the part it got wrong is worth keeping
next to the part it got right.

Right: an irradiance entry is the wrong shape for a shiny surface, the cost is per entry
rather than per pixel, and directional storage is what a reflection needs.

Wrong: **"one extra ray on shiny pixels only, costs nothing"**. A specular ray beside the
diffuse one lost the device on every scene tried. It is not the ray's cost — striding it down
to one pixel in sixteen did not help, and a runtime cost responds to being run less often.
`march` is enormous and GLSL has no calls, so every mention of it is another inlined copy; the
shader already carried three, and a fourth is past what the driver will take. The same code
wrapped in `if (false)` compiled away and the crash went with it, which is what proved it.

So the bounce **chooses a lobe** instead of adding a ray. One ray, sampled from the cosine
lobe or the GGX lobe in proportion to Fresnel, and its result goes to whichever cache it is a
measurement of: irradiance for diffuse, a direction bin for specular. Each cache averages only
its own kind, so each stays unbiased and neither needs the other's weighting.

Also wrong: spherical harmonics. Twenty-four direction bins — dominant axis, sign, and a
two-by-two split of that cube face — cost one entry each and need no new storage layout at
all, because a bin is just another key into the table that already exists. SH would have meant
a new entry format for every kind of entry.

| ID | Decision | Asked by | Why |
|---|---|---|---|
| D180 | **A radiance entry is keyed by face *and* direction bin**, in the same table as irradiance and shadow | — | The table already has eviction, probing and a budget; a fourth structure would need all three again. A bin is one more field in the key |
| D181 | **The bounce picks a lobe rather than tracing both** | the driver | A fourth inlined `march` loses the device regardless of how rarely it runs. Bisected: not the cache, not the claiming, not the cost |
| D182 | **A bin is shaded from only once it holds several samples** | — | One sample is not an estimate, it is that sample, and samples are clamped at 8 — so a single lucky path leaves a white speck. The diffuse cache learned this as scattered stars on dark walls |
| D183 | Near-mirrors are **not shared**: below a roughness threshold a pixel uses its own sample | — | A bin is a quarter of a cube face. That is honest for the roughnesses a voxel world is mostly made of and visible nonsense for a mirror |

### The original assessment, kept because its reasoning still holds

The current entry is an **irradiance** cache: one value per face for light arriving from everywhere, which is exactly right for a diffuse surface and wrong for a shiny one. Glossy and metallic surfaces currently read that same diffuse value, so a polished surface reflects an average of its surroundings rather than an image of them.

A **radiance** cache stores light per *direction*, which is what a reflection needs.

**Worth it, and not yet.** The reasoning:

- It is the difference between metal looking like grey plastic and looking like metal, and there is no cheaper way to get that — a screen-space reflection cannot see anything off screen, which in an enclosed voxel build is most of the room.
- The cost is per entry, not per pixel: four spherical-harmonic coefficients per channel is 12 values against today's 3, so the table grows about four times, or the entry count shrinks by four for the same memory. That buys plausible low-gloss response but *not* a mirror — SH cannot represent a sharp reflection at any order worth storing.
- Sharp reflections need a different mechanism entirely: trace the specular ray per pixel and let *it* read the cache where it lands. That is one extra ray on shiny pixels only, costs nothing on the rough surfaces that are most of a world, and gives a true mirror rather than an approximation of one.

**So the plan is both, in that order, and after Stage 6:** SH irradiance for low-gloss response, plus a per-pixel specular ray that reads the cache at its far end for anything smooth. Doing it before materials exist would be tuning a reflection model against six placeholder substances, which is how you end up rewriting it once the real ones arrive.

## The renderer rewrite — stage R0, the instruments

The rewrite planned in `21-renderer-rewrite.md` begins by fixing the thing that measures it. The
first hour of it found that the pass under rewrite has never been timed.

| # | Decision | Source | Notes |
|---|---|---|---|
| D201 | **The GPU profiler keeps a stack, and a pass claims its slot when it opens rather than when it closes** | measurement | It kept one open index and did not claim the slot until `end_pass`. The cloud dispatch was later moved *inside* the path tracer's pass, so the inner `begin_pass` took the outer one's slot and overwrote its name, the inner `end_pass` cleared the open index, and the outer `end_pass` found nothing open and wrote no end timestamp at all. Everything after the inner pass — which is the entire tracer — fell outside every timestamp in the frame. It read as **0.253 ms of GPU work against 37.7 ms of wall clock**, with a plausible-looking list of passes above it, and nothing anywhere said a pass had gone missing. The damage is confined to measurement: auto-quality observes wall-clock frame time and was never misled |
| D202 | **A reported figure is a mean over a window, with the warm-up thrown away and the worst frame beside it** | D201 | One frame's GPU time moves several per cent from clock and scheduling alone, so a single-frame number cannot detect a 3% regression — which is the tolerance every stage of the rewrite is gated on. The engine now averages from half way through a scripted run and reports mean, worst and frame count. Worst is there because a locked frame rate is decided by the worst frame and not the mean one (`09-performance-budgets.md` §9) |
| D203 | **The path-traced figures in `19-auto-quality.md` are withdrawn.** 2.495 ms outdoors and 19.988 ms enclosed cannot be reproduced by any build in the tree | D201 | They predate both the nesting and most of what the tracer now does — next-event estimation, directional radiance bins, the glass segment loop, the three-ring eight-tap face gather, the level pyramid. Every one of those is per pixel and none of them was ever timed. Measured now on the same machine at 1280×800: **12.28 ms outdoors and 40.07 ms enclosed** |
| D204 | **Measurement lives in one place**: `tools/baseline.ps1` over a fixed grid of cameras, resolutions and modes, writing a file that the next run diffs against | — | Every performance figure in `documentation/` before this was taken by hand, once, at whatever camera and resolution happened to be in front of somebody, and then quoted for months. Two of them turned out to be wrong rather than merely old. The cameras live in `tools/_grid.ps1` so that no two tools can measure different things and call the numbers comparable |
| D205 | **Debug view 11 writes each pixel's face key out as four exact bytes**, so the number of distinct faces in a frame can be counted rather than assumed | — | The whole of `21-renderer-rewrite.md` rests on face count growing far more slowly than pixel count, and nothing in the engine had ever measured it. A cardinality cannot be sampled, so it is written to the image and counted on the way out: the screenshot path writes four channels through `stbi_write_png`, PNG is lossless, and a unorm8 channel returns exactly what `byte / 255.0` put in |

## The renderer rewrite — stage R1, the node pool

Built and tested headless before anything in the renderer moved, which is how residency was built
in Stage 2 and for the same reason: a structure the renderer walks and nobody compares against the
world is a renderer debugging a mirage.

| # | Decision | Source | Notes |
|---|---|---|---|
| D206 | **One sparse octree at every scale replaces the chunk grid, the brick mask, the brick-mask pyramid, the coarse occupancy grids, the summary octree and the thumbnail tiers.** A node's coordinate is its voxel coordinate shifted by its level | D187 | Four addressing schemes glued end to end, every seam between them with its own entry in this log, and all the same fault: two structures answering one question and allowed to disagree |
| D207 | **A leaf is a brick, at level 3.** Below that nothing changes — palette encodings, the 64-byte occupancy, the two mips, and a ray marching at single-voxel resolution | D132 | Bricks are the storage and compression unit and they work. Chunks are the fixed-size box and they do not |
| D208 | **The eight children of a node are contiguous, so a descent is `children + octant`** | measurement | One hash to enter the tree; pure arithmetic below it. Against two dependent loads per chunk entered, up to thirty-two times along each axis crossed, plus a coarse-grid fetch per skip |
| D209 | **A leaf carries a leaf id, not its data.** The header and the sixty-four bytes of occupancy live in their own array | bug | The first version stashed the brick's encoding in the spare bits of the coverage word. It collided with the -z byte, so `index_bits` read back as a coverage value and `decode_voxel` divided by it. A crash rather than a wrong picture, which was luck. Moving a node now copies thirty-two bytes and never its occupancy |
| D210 | **Allocation is a bump pointer and two free lists, never a search for a contiguous run** | bug | The first version carved runs off the tail of one free list and checked the eight it found were consecutive. They are — until something is freed, after which the check fails, allocation returns "nothing", and the caller reads that as an empty region. **A tree that stops building because it ran out of memory must not look like a tree that stopped because the world is empty**, so `out_of_room_` tells the two apart |
| D211 | **A build is gated on an index of what the world holds** | measurement | Without it a node at the entry level descends into eight children unconditionally: 8¹¹ is 8.6 billion nodes for a world holding one brick. The summary octree hit this once and never returned (D153); this hit it and was rescued by the allocator failing, which was worse |
| D212 | **A radius of zero means no proximity residency at all** | bug | A loop that still runs once at radius zero holds the camera's own node resident for ever. Caught by an eviction test, not by looking |
| D213 | **`mirror_voxel` walks the pool exactly as the shader will**, and is asserted against the world over every voxel of a scene with a slab, a one-voxel wall and an isolated voxel | D207 | The successor to the mirror check that has guarded residency since Stage 2. It immediately caught a payload base passed as the pool rather than the brick — which decoded the one brick at offset zero correctly and read a neighbour's palette for every other |

| # | Decision | Source | Notes |
|---|---|---|---|
| D214 | **A child mask records what the WORLD has, not what the pool has built.** A child the world holds and the pool has not built is left at level nought in its slot | bug | Setting the bit only for built children makes a node whose budget ran out indistinguishable from a node over empty space — so no ray reports it and nothing is ever streamed. That is D133 and D147 arriving for a third time, and this is the version where it is a property of the structure rather than a bug to be found again |
| D215 | **A descent returns three answers, not two: here, empty, and wanted** | D214 | They drive three different behaviours — draw, jump, report — and the whole of streaming is the difference between the last two. `NodeFind` carries it on the CPU and `Found` in the shader, from the same walk |
| D216 | **The coarse skip falls out of the descent.** When a child mask bit is clear, the empty cell's size is known, so a ray jumps its width | D215 | This is what five separate wrapped occupancy grids were carrying, and it is now a consequence of the tree rather than a structure beside it |
| D217 | **The walk keeps the ancestors it descended through, and which of them survive a step is computed rather than stored** | — | Two points share every ancestor above the highest bit in which they differ, so one XOR and one `findMSB` says where to restart. No coordinates are kept and no loads are spent finding out |
| D218 | **The entry bucket is a 32-bit mix over truncated coordinates, mixed one axis at a time**, and the same function exists on both sides | D205 | The shader has no 64-bit arithmetic in its inner loop, and a hash short enough to compare by eye is worth more than one that is merely good. Mixed per axis rather than by XORing three products, because voxel coordinates are a dense lattice and XOR-of-products over a lattice drops whole planes into the same buckets — the fault the face-cache key already hit |

**Where R1 stands.** The pool is built, correct and tested — 424 test cases, 18.0 M assertions —
and `shaders/node.glsl` walks it, compiled, with `node_visibility.comp` writing the existing
visibility format so `resolve.comp` reads it unchanged. It sits *beside* the old marcher rather
than replacing it, so the two can be rendered from one camera and diffed: R1 claims the picture
does not change and the cost falls, and both halves need measuring rather than asserting.

**Known gap, carried to R2.** A node the world has and the pool has not built is currently skipped
like empty space, so an unstreamed region draws as sky for a frame or two rather than showing its
parent. Drawing the parent instead is the right answer and it needs the rule D151 already
records — never fall back to a level coarser than the pixel resolves, or a two-kilometre block
containing ground a mile away draws as a blob a few metres from the camera. That is residency
policy, and residency policy is R2.

## The renderer rewrite — the node pool becomes the default marcher

R1d proved the pool on every camera in the grid; this is the change that makes it the one the game
actually runs. The old marcher stays reachable until R1e deletes the addressing behind it, because
two marchers that can render one camera are how a disagreement gets settled rather than argued.

*Numbering note: `21-renderer-rewrite.md` §8.0 credits R1c to "D219–D223", but no such entries were
ever written into this log. The numbers are left unused rather than back-filled from memory, which
is the mistake `19-auto-quality.md` made and D204 exists to prevent.*

| # | Decision | Source | Notes |
|---|---|---|---|
| D224 | **The game launches on the node pool. The chunk marcher moves behind `--chunk-marcher`** | D206, R1d | The flag inverts rather than disappearing, because R1e has not happened: `world.glsl`, the coarse grids and the eight thumbnail tiers are still what the path tracer reads, and until it is ported the two marchers have to coexist. Measured at the default camera, 1280×800 quality 7 over 150 frames: visibility **1.105 ms against 0.719**, total GPU 2.163 against 1.773, with the pictures apart by 0.006 mean and 54 pixels of 1,024,000 past a threshold of eight. That regression is the enclosed room and it is the known one — R1d records it across the whole grid, where the pool is faster on six views of seven and three times faster where distance dominates. It is a real cost paid at the one camera the pool helps least, in exchange for the six it helps and the 4.8 MB against 57.7 |
| D225 | **A feedback entry's format is decided by which shader wrote the buffer, not by which marcher is configured** | bug | Both marchers write one buffer: the chunk one puts a chunk coordinate and an unused detail level in it, the node one puts a node coordinate at its own level. The consumer keyed off `use_node_pool_`, which describes what the *visibility* pass would do — and in the path tracer the visibility pass does not run at all, so `pathtrace.comp` filled the buffer with chunk coordinates that were then shifted left by a detail level. Streaming spent the frame asking for chunks kilometres from the ones that were missing: **52 of 68 resident against 57**, and the node pool built **488 nodes** toward keys the world has nothing at. Neither counter said so — the phantom count reads zero because the shifted coordinate lands on a chunk that does exist. The condition is now `use_node_pool_ && !path_trace_`, which is the question actually being asked |
| D226 | **`tools/baseline.ps1` passes no marcher flag, so it measures whatever the game launches with** | — | It always did, which is why every figure it has ever produced is a chunk-marcher figure. From here it produces node-pool figures without being touched, and that is the right behaviour: a baseline should measure the game rather than a configuration of it. R0d's grid has still never been recorded, so nothing already on disk is invalidated |

## The renderer rewrite — the enclosed room, and the measurement that could not see it

The one camera the node pool lost on, which R1d left open and the handover made the gate before R1e.
Finding the cause took one measurement; trusting the fix took considerably longer, because the
harness it was being measured with turned out to be racing the scene.

| # | Decision | Source | Notes |
|---|---|---|---|
| D227 | **A descent caches two ancestors below the root, at fixed levels 8 and 5, in named scalars** | measurement | The root cache saved the hash and nothing else: every step still walked eleven levels from 512 m down to a brick. What said so was the step count, which the visibility buffer has carried all along and nobody had read — **9.12 steps a pixel against the chunk marcher's 31.27**, at 1.088 ms against 0.723. Three and a half times fewer steps and half again the time means the step is what costs, and the step is the descent. Two points in one cell at level L share every ancestor above L, so they take the same octant at every level and every child-mask test gives the same answer: entering from a cached ancestor is the same walk, and the enclosed picture is bit-identical before and after. Levels chosen against how far a step moves — marching bricks, a ray crosses a 1 m cell every four steps and an 8 m cell every thirty-two |
| D228 | **Named scalars, never an array indexed by a level** | D227 | The first attempt at keeping the descent kept all twelve in an array, which a GPU holds in registers only while every index is known at compile time. Indexed by a run-time level it went to scratch and cost 11.52 ms. That is why this caches two fixed levels rather than the path: a constant index is the whole of the difference |
| D229 | **The scene the engine is judged against was never finished when it was measured, and no figure said so** | measurement | The facility sharpens region by region in the background and the result is cached — but the cache is written only when the *last* region lands, and refinement skips regions behind walls (`start_refinement`'s occlusion test), so from a fixed camera it settles three to six regions short and the cache is never written at all. `facility.clip.world` is the only clip cache missing from the tree, and that is why. So every run rebuilt the scene while being timed, and a 300-frame screenshot caught whatever had been built by then. **The bias runs against whatever is being tested**: a build that renders faster reaches frame 300 sooner, has less world in front of it, and flatters itself. Measured: two runs of one binary on the `close` camera differed on **52,292 pixels of 1,024,000** |
| D230 | **`--settle` starts the measurement window when refinement stops, and the state has to hold for 240 frames** | D229 | A fixed point rather than a stopwatch reading. It has to hold because "nothing selectable" is transient: `pump_refinement` marks a box done and calls `start_refinement` *before* pasting it, so the pick that decides whether anything is left is made against the world as it was before the box landed, and a box that lands can uncover regions the occlusion test was rejecting. Latching on the first quiet frame started the window mid-build — two runs measured 82,718 and 95,638 nodes and disagreed on 65,316 pixels, and a **longer** window made it worse, which is the signature of a world still changing rather than a picture still converging. Held, the world is identical across runs to the voxel: 120 chunks, 126,794,016 solid voxels, 15 of 18 regions both times |
| D231 | **A timing is reported with the scene it was taken against** | D229 | Chunks, solid voxels, and regions sharpened, printed beside the pass table. Every performance number in this repository before this is a time with no scene attached, and the scene is not a constant. Two figures are comparable only when these three match, and that is now checkable rather than assumed |
| D232 | **R1d's image-diff column is withdrawn; its timing column is not** | D229, D230 | The gate was *"`Measure-ImageDiff` worst ≤ 2 on every view"*, and it was measured without `--settle`, so the run-to-run floor on the views that see the building was tens of thousands of pixels — larger than the quantity being gated. The timings survive because the chunk marcher reproduces on the same harness to a thousandth (0.699 against 0.701 enclosed, 1.574 against 1.575 outdoor); it is the *comparison between two builds of different speed* that the race corrupts, and R1d's old-versus-new pairs were taken in one binary on one frame schedule. This is D203 happening again — a figure that no build in the tree can reproduce — and it is recorded the same way rather than quietly restated |

| # | Decision | Source | Notes |
|---|---|---|---|
| D233 | **The node pool does not converge, and that is carried to R2 rather than papered over** | measurement | With the world provably identical and the camera still, it was still building 273–385 nodes a frame **three thousand frames** after the world stopped changing, and two runs finished with 89,560 against 81,464 nodes. Eviction is not the cause; it reports none. One contributor is now visible and was not: a request whose root is already live but whose frame has spent its 16,384-build budget fell off the end of the branch **uncounted**, so `deferred` read nought while the tree failed to fill in — and a `refine` walks a whole path allocating eight siblings a level, so a few hundred reports can exhaust a frame. Counted now, which is what D136 did for phantom reports. Residency policy is R2 and this is residency policy |
| D234 | **A number that cannot repeat cannot regress** | measurement | The sky camera read 0.484 against 0.759 between two builds, which on the floor view would be a 57% regression and a serious one. Run three times on **one** build against **one** scene it gives 0.481, 0.763, 0.472 — a 51% spread, and the two builds had landed in different modes of a bimodal figure. The mechanism is D233's: a ray is clipped to `residency_.resident_bounds` (D148), the resident set is what a run converges to, and it does not converge to the same thing twice. So the empty-space views inherit the pool's irreproducibility, and a single sample on one of them is not evidence in either direction. This is the rule that should have caught the sky claim before it was written down, and it is why the enclosed and close figures — 26% and 33%, on the two views that carry the cost — are the ones quoted |

## The renderer rewrite — what the player was actually waiting for

The user reported the new renderer as laggy, slow to load, and probably never going to be as fast as
the old one. Every measurement in this repository said the opposite, and the user was right anyway,
because the measurements were all of the wrong pass. A screenshot of the in-game overlay settled it
in one line: **GPU 0.92 ms, frame 247.51 ms, 99th percentile 2,234 ms.**

| # | Decision | Source | Notes |
|---|---|---|---|
| D235 | **The node pool uploads what changed, not what exists** | user report | It copied every array's whole used prefix on any frame that touched anything. That is free while the tree is converged and quiet — which is exactly what a fixed camera measures — and it is **10 MB a frame** while somebody walks across the building. Measured on a flight: the `nodes` pass at **2.725 ms mean and 8.915 ms worst against a 0.80 ms budget**, the largest single cost in the frame and eleven times over. Now every write into the arrays marks itself and runs are coalesced with a gap tolerance, because a copy has a fixed cost of its own and a few spare records inside one are cheaper than a second. **2.725 → 0.028 ms mean, 8.915 → 0.257 worst**; total GPU on the same flight 5.292 → 2.684 |
| D236 | **`NodeBuffers::audit` is what made D235 safe to attempt**, and it earned its keep three times in one sitting | — | A missed dirty mark is a stale byte on the card and a wrong picture, and all three were invisible to any other check. The entry table's empty value is `kNoNode` rather than zero while a device buffer starts zeroed, so the initial state was itself a change nobody had marked. `refine` **moves** a built shell into its child slot, so both ends change and marking the build alone leaves the destination stale. `release` zeroes nodes on their way to the free list. Each was named to the byte — "first at byte 18,240 of 277,792, card 0x00, host 0x01" — rather than deduced from a photograph, which is the difference between this and D131's class of fault |
| D237 | **A node the pool has not built draws its parent, not empty space** (R2d) | user report | Falling through to the skip treats a region the world *has* as air, so a ray flies through a building that has not streamed and draws sky. The old renderer never did this: it pushed a coarse thumbnail from the camera, so there was always something to show (D140). Only when the missing cell is the size the pixel resolves, which is D151 and not a tuning choice — the descent returns WANTED at a level *above* the one it was asked for, so the stand-in is never finer than the pixel and allowing coarser is what puts a two-kilometre blob a few metres from the camera |
| D238 | **F6 swaps marchers where you stand** | — | Both are built and both are fed every frame while R1e is outstanding, so it costs a branch. It is the only way to compare them on what a fixed camera cannot show — loading, and turning round — and it is the way back to the old renderer without a restart, which is what makes trying the new one a low-risk thing to ask somebody to do |
| D239 | **The hitching is the region paste, and it is not the renderer's** | measurement | The scene sharpens region by region; the *sampling* runs on a background thread and the **paste does not**. Measured on one flight: pastes of **12,448 and 13,620 ms**, blocking the main thread. That is the 6,282 ms worst frame and the 2,234 ms 99th percentile the overlay reported, against a GPU that was doing under a millisecond. Both marchers suffer it identically — 50 stalls totalling 62.5 s against 3 totalling 18.9 s differ only because the two runs got different distances through the sharpening — and it predates the whole rewrite. D74 already names the fix and calls it Stage 16's problem: *"an edit runs in one frame and cannot be interrupted… raising the cap means slicing the work across frames"*. It is now the largest thing standing between this engine and being judged fairly |
| D240 | **A performance complaint is answered with the player's overlay before it is answered with the grid** | user report | Four exchanges were spent quoting settled per-pass means at somebody describing stalls, and the settled grid is blind to stalls *by construction* — `--settle` exists to discard exactly the transient the complaint was about. The overlay's own three numbers named the culprit immediately. Ask for those first |

## The renderer rewrite — the world that was rebuilt on every launch

D229 recorded that `facility.clip.world` is the only clip cache missing from the tree, and named the
reason: the cache is written when the *last* region lands, and the last region never lands. It was
recorded as an explanation of a measurement problem. It was also two minutes of everybody's time,
every single launch, and the whole of R0d's debt — a forty-two run grid that nobody has ever
completed because each run rebuilds a hundred and twenty-five million voxels before it starts.

| # | Decision | Source | Notes |
|---|---|---|---|
| D241 | **The clip cache is written when refinement settles, not when it finishes**, and it carries the list of which regions are sharp | D229 | "Finished" is unreachable from a fixed camera by design: a box behind a wall is skipped by `start_refinement`'s occlusion test for as long as the camera stands there, and the facility settles at 14 of 18. The fixed point is what gets written, with a `CachedRegion` per box saying whether it has been paid for — without that a half-built world is indistinguishable from a finished one, every later launch loads the blocky version, finds nothing to do, and the building never comes good again, which is exactly the trap the old comment in `build_world` was written to avoid. Measured on the default camera, 1280×800, quality 7, `--settle`: first run **133.3 s** (unchanged: 133.0 s before), every run after it **6.6 s**. The write itself is **18 MB in 36 ms**, against pastes of up to 17.4 s in the same load, so it is not a stall anybody can find |
| D242 | **A world cached half-built is carried on from, and the world converges across runs rather than being thrown away at the end of each** | D241 | A later run standing somewhere else sharpens what *it* can see and writes again. Measured: default camera settles at 14 of 18 and writes; one run from `close` finishes the remaining four in 10.5 s and writes 18 of 18; every run after that loads a complete world in 5–7 s and reports `no ladder`. The finished world reaches **127,198,381 solid voxels in 74 chunks**, which is voxel-for-voxel what the headless streaming audit gets building the whole clip from scratch in 204 s by a completely different route |
| D243 | **The scene line carries the world's content hash** | D231 | D231 put chunks, solid voxels and regions beside the timings so two figures could be told apart. Those are a description, not an identity — two different worlds can share all three. `World::content_hash` is the identity, and it skips empty chunks, so a world built here and the same world read back from the cache agree despite one of them never having been compacted (108 chunks against 74, same hash `252d0711845cbb65`). It is what proved the cache round trip: **the world that comes back is the world that went in**, which no count could have established |
| D244 | **The picture a run draws depends on whether it watched the world sharpen, and the world is not what differs** | measurement | Cold and warm runs of one binary, world proven identical by D243's hash, differ on **87,357 pixels of 1,024,000** (mean 4.07, worst 173). Each is bit-reproducible on its own — three warm runs and two cold runs are pixel-identical within their kind — so this is systematic, not noise. What differs is renderer state: the cold run's node pool was built against a world that changed under it 15,639 frames' worth and **evicts nothing** (D233), so it holds nodes derived from geometry that has since been replaced; the warm run builds every node once from the final world. This is a point in the cache's favour and a warning about every figure taken before it: **the grid's old numbers are not comparable with the grid's new ones**, and the content hash is how that gets checked rather than argued |
| D245 | **A world with no ladder is settled, and saying so out loud fixed a hang** | D241 | `start_refinement` returned early when there was no refinement script and left `refine_settled_` false, and `--settle` waits on that flag — so a run with no ladder waited for a fixed point that had already happened, for ever. It was reachable before (`--clip-coarse 1 --settle`) and nobody had hit it; D241 makes it the normal case, because a finished cached world has no ladder. Nothing here can be improved, which is what settled means |
| D246 | **The region grid is compared corner for corner rather than trusted to the cache key** | D241 | The key covers the clip text, the resolution and the modification times of `src/forge`, `src/world` and `src/game/clip.*` — deliberately, so that editing a menu label does not throw away a minute of resampling. The region planner is in `src/app/main.cpp`, which is in none of them, so changing how the grid is cut would leave every cache file matching its key while its flags referred to boxes that no longer exist. Adding that file to the key would invalidate every built world on every edit to five thousand lines of renderer, HUD and command line. So the corners are compared instead and a grid that has moved re-sharpens from scratch: slow once, rather than a building quietly coarse in the wrong places for ever |

## The renderer rewrite — the pool was throwing away the scene it was drawing

D233 recorded that the node pool "does not converge" and carried it to R2 as an open question, with
D234 recording that the empty-space cameras were bimodal and blaming the same unknown. They were one
bug, and the reason it took three sittings to find is that every symptom it produced looked like a
different subsystem.

| # | Decision | Source | Notes |
|---|---|---|---|
| D247 | **A node is evicted only when the pool is under pressure** | measurement | `last_wanted` is refreshed in exactly one place — the request loop — and requests come from feedback, and **feedback reports misses**. So the moment a tree finishes building, the rays stop missing, nothing is requested, no timestamp advances, every node goes cold on the same frame, and six hundred frames later the pool evicted the whole scene *including everything the rays were reading that frame*. Then they missed, it rebuilt, converged, went quiet, and did it again. Measured on a static camera over a cached world: 8,684 nodes at frame 400 and **1,713** at frame 1000, with the converged frame disagreeing with the chunk marcher on **767,526 pixels of 1,024,000** because it had been caught with the tree mostly gone. Now: 8,696 nodes at 400, 1000, 2000 and 4000 frames, `built 0`, `evicted 0`, and the two converged frames are **bit-identical** |
| D248 | **D233 and D234 are closed, and both were this** | D247 | D233's "still building 273–385 nodes a frame three thousand frames after the world stopped changing" was the rebuild half of the cycle. Its "two runs ended at 89,560 against 81,464 nodes" was two runs caught at different phases of it. D234's bimodal sky — 0.481, 0.763, 0.472 from one build against one scene — was the ray clip following `resident_bounds` (D148) as the pool emptied and refilled; the same three runs now measure **0.834, 0.841, 0.846, a spread of 1% against 51%**. One fault, four write-ups, and every one of them had been filed as a separate open question |
| D249 | **Image diffs can gate again** | D247 | The floor D232 withdrew R1d's image column over was this churn. The node pool now differs from the chunk marcher by **176 pixels of 1,024,000** on the enclosed camera — 0.017%, and that residue is the coarse-level colour rule D149 and D152 settled rather than an error. R1e's gate, "the grid table does not move", is checkable for the first time |
| D250 | **The test that asserted eviction was asserting the bug** | — | `a node nobody wants is evicted` passed throughout, because the pool did exactly that and it was wrong to. Replaced by two: a built tree survives being quiet however long the frames run on, and — the half that still has to work — under a budget small enough to cross the pressure mark, the coldest node still goes. A test that passes while the thing it covers is broken is worth less than no test, because it is read as evidence |

**Still open, and it is the right shape of open.** A node ought to be marked wanted when a ray *uses*
it, which needs the marcher to report hits and not only misses. Until then "under pressure" decides
eviction and `last_wanted` orders it, which is sound while there is room and approximate when there
is not. That is residency policy and it belongs to R2 — but it is now a design question with a
known answer rather than a mystery blocking a gate.

## The renderer rewrite — the 275 ms frame, which was never the renderer

The player reported 4 FPS with the overlay showing **GPU 0.92 ms** and later **6.81 ms** against
frames of 247 and 275 ms. Four exchanges went on the renderer before anything measured the gap.

| # | Decision | Source | Notes |
|---|---|---|---|
| D251 | **The node pool's CPU update is timed, like the chunk path has been since Stage 2** | measurement | It never was, so the cost of building the tree appeared in no figure anywhere — and it turned out to be the entire frame. With a carve applied: **`CPU node pool 252.763 ms, worst 267.495`** against a 3.8 ms GPU. The engine's own CPU counter said 236 ms and nothing said where it went. A subsystem nobody times is a subsystem nobody can be wrong about |
| D252 | **A request is served once however many times it is asked for** | measurement | Feedback saturates at its 131,072 capacity the instant a large edit drops the tree, and `stream()` dilates every entry to its six face neighbours — so **917,504 requests** arrive each frame for a handful of distinct nodes, and each is walked from its root through eleven levels of dependent loads to discover it is already built. `built 914333` in one frame. Deduping is one hash against eleven dependent loads, and the duplicates are the overwhelming majority rather than an edge case: **252.8 ms to 18.8 ms**, total CPU 236.8 to 47.2, `built` 914,333 to 18,812 |
| D253 | **A stand-in must come from a node that has been folded** | user report | The R2d stand-in drew whatever colour the WANTED node carried, and at WANTED that node can be a shell — never folded, colour nought. It painted the facility black in silhouette, columns and all. The guard against exactly this sits twenty lines above in the same function and states the consequence in words: drawing a shell "paints black geometry over open sky". Refused on the way down, reintroduced on the way out |
| D254 | **Invalidation drops one root per edit, not every ancestor of every brick** | measurement | It walked leaf to level 24 for each dirty brick, released every ancestor — and releasing a root frees its whole subtree anyway — then scanned the whole 262,144-entry table without breaking on the match. 97,500 dirty bricks is some twenty million full-table scans in a frame. This is a retreat rather than the answer: refreshing the leaf in place and re-folding its ancestors is right, and needs the parent's child mask refreshed from the world, which is R2 |
| D255 | **`--screenshot-frame` counts frames, so a scripted measurement on a slow build is a long one** | user | 1200 frames at 4 FPS is five minutes of a game window on somebody's machine, twice, while they waited. Measure with sixty frames and raise it only when the figure needs it |

## The renderer rewrite — the cost of an edit

Editing is what this game is for, so the cost of an edit is the cost of playing it.

| # | Decision | Source | Notes |
|---|---|---|---|
| D256 | **An edit drops the brick it touched, not the half kilometre around it** | measurement | Invalidation released the entry-level root, which is 512 m across, so chiselling one voxel threw away everything within half a kilometre and rebuilt it at the rate pixels asked for it. Measured by a test written before the fix: **37,497 nodes before a single-voxel edit and 0 after**. It now walks down to the brick, drops that alone, and walks back up re-deriving each ancestor's child mask **from the world** and re-folding it. The mask matters as much as the fold: a bit left set over an emptied brick is a ray reporting a node the world does not have, every frame, for ever, which is D133 exactly. A chisel-sized edit now costs **0.001 ms** and leaves the tree identical to no edit at all |
| D257 | **Each ancestor is refreshed once, deepest first** | measurement | An ancestor is shared by everything beneath it, so a large carve walks the same upper nodes tens of thousands of times, and one mask is eight `world_has` calls that below level 8 walk bricks in a chunk. Per brick, that measured a single frame of **22,481 ms**. Deduped, the same carve's worst frame is **355 ms**. The order is not incidental either: a fold reads its children, so every brick must be dropped before anything above it folds, and the ancestors must go deepest first or a parent averages a child that has not been refreshed |
| D258 | **The test that covered edits was asserting the root disappeared** | D256 | Its stated intent was right and is the reason it exists — "a resident-but-wrong node is the one thing feedback can never discover, because a ray that finds it does not report it (D131)" — but it checked the *root* was gone, which was standing in for "the stale brick is gone" and passed for the wrong reason. It now asserts the brick went and the root stayed, which is both halves |

## The renderer rewrite — R2b, and the reason it is not a small step

| # | Decision | Source | Notes |
|---|---|---|---|
| D259 | **Age decides eviction, without a pressure gate** | D247, D251 | The gate was the bandaid, not the policy. While it was there nothing was ever evicted, so resident memory was a high-water mark of everything ever asked for. It is safe to remove now only because a miss is no longer the only thing that says "wanted" (R2a). Verified not to churn: 8,452 nodes at frame 600 and at frame 4,000, nothing built, nothing evicted, the GPU mirror matching at both |
| D260 | **R2b's gate fails, and residency granularity is why** | measurement | The gate is *resident bytes at half resolution within 30% of a quarter of the full-resolution figure*. Measured: **6.04 MB at 1280×800 against 4.87 MB at 640×400**, where a quarter is 1.51 MB — 3.2× over. Enabling eviction moved it to 5.39 and 3.90 and evicted nothing at all, which is the finding rather than a disappointment: **`live_` holds entry-level roots only**, a root is 512 m, and the whole facility sits inside one. So eviction can drop the entire scene or nothing, and there is no granularity in between at which memory could track the screen. The first half of the rule already holds — the marcher never *requests* below the pixel footprint — and it is the "never stored" half that has nowhere to happen |
| D261 | **R2b is per-node residency, and that is the core of R2 rather than a sub-step of it** | D260 | Nodes have to be tracked, aged and released individually instead of by the root they hang from, which means an LRU over the whole pool rather than over the entry table, and a release path that detaches a node from its parent's `children` run without freeing its siblings. Sizing it honestly: this is most of what L was meant to cover, and the sub-step list under R2 reads as though it were a flag to set |

## The renderer rewrite — residency per node

| # | Decision | Source | Notes |
|---|---|---|---|
| D262 | **Residency is tracked per node, and the tree erodes from the leaves** | D260 | A node with no *built* children that nothing has read for `cold_frames` gives up its subtree; next frame its parent has no built children either. Only childless nodes are candidates, so a parent is never dropped from under a child something is still reading. What is left behind is a node at level nought — the same state an edit leaves (D256) — so a ray that wants it again reports it and it returns at the level that ray asks for. The slot stays in its parent's run of eight, because the run is the allocation unit and the subtree underneath is where the bytes were |
| D263 | **A ray reports the node it stopped on, by slot** | D262 | The root only says a ray entered a 512 m block. A slot rather than a key because the ray descended to it and knows it, so the CPU stores instead of walking the tree to find it again — sixteen thousand reports a frame is sixteen thousand stores. The two sides agree about slot numbering by construction and `NodeBuffers::audit` checks it byte for byte. Sampling went from one pixel in 256 to one in 64: a node must be read once inside `cold_frames` or it is dropped, and at one in 256 a node covering a single pixel is missed for 600 frames about **one time in ten**, where the cost of being wrong is geometry that vanishes and comes back. At one in 64 it is seven in a hundred thousand |
| D264 | **A node is born fresh, not born stale** | bug | `node_last_read_` starts at nought, so every node built after frame `cold_frames` was already past the threshold and was evicted before a ray had read it once — built and thrown away in the same breath, for ever. Caught by two tests that could no longer build a tree at all. Set at allocation, and on the destination when `refine` moves a shell into a child slot |

**Two things are open and neither should be built on until they are settled.**

**The gate is still not met.** Erosion is one level per `cold_frames`, so a tree eleven levels deep
takes some 6,600 frames to shed its fine detail after a resolution change. Resident bytes do now
follow the screen — 75,295 nodes fell to 66,630 at 640×400 over 1,200 frames — but *slowly*, and
R2b asks for a ratio, not a direction. Either the erosion needs to key on the level a node's
footprint now resolves to rather than on age alone, or `cold_frames` needs to be a function of
depth.

**A GPU mirror mismatch was seen once at 640×400 and did not reproduce.** The audit passed on every
other run including a direct retry of the same command. Erosion is the only thing that changed and
is the obvious suspect: it is the first code to release nodes in bulk while the renderer is
running. It must be understood before anything relies on the erosion, because an intermittent
mirror mismatch is the exact shape of the fault that took R1 five readings to find.

| # | Decision | Source | Notes |
|---|---|---|---|
| D265 | **The audit waits for the frame before it touches the staging ring, not only after submitting** | measurement | It reads the device buffers back into `staging_`, which is the same ring `upload` writes its dirty ranges through — and an upload recorded into the frame's command buffer has not necessarily executed. Copying over those bytes hands the pending upload whatever the audit just read, so the card receives the wrong thing and then disagrees with the pool: **an audit that causes the fault it exists to report**, intermittently, depending on whether an upload was in flight. Per-node eviction made uploads happen on most frames rather than a few, which is what surfaced it. Four consecutive runs at the resolution it failed on now match with identical node counts — which is consistent with the fix and is not proof, an intermittent fault being what it is |
| D266 | **The erosion sweep repeats until nothing more is cold** | measurement | A node is a candidate only once its children are gone, so one sweep takes one level: an eleven-level dead subtree needed eleven times `cold_frames`, or twenty seconds at three hundred frames a second. Bounded by the depth of the tree by construction, and each pass is a scan of a few hundred kilobytes |

**R2b's gate is still not met, and the remaining gap is not understood.** Half resolution holds
3.95 MB against a target of 1.36 MB — **2.90× over**; quarter resolution is 1.62× over. Erosion is
demonstrably working (82,798 nodes to 64,846 at half resolution) but it removes 22% where the rule
predicts 75%, and the reason is not yet measured. The next thing to do is count resident leaves per
*level* at each resolution rather than in total: the rule says halving the resolution should move
every ray's stopping point one level coarser, and a per-level histogram either shows that happening
or shows exactly which levels are not shifting. Guessing at it twice has produced two changes that
were worth making and did not close the gap.

| # | Decision | Source | Notes |
|---|---|---|---|
| D267 | **Resident bytes are reported apart from the entry table, and the gate is measured on the first** | measurement | `total_bytes` includes the entry table, which is sized once from the budget and never changes — 1,048,576 bytes at the default. On the distant camera that is **95% of the total**, so the number moved 3% while the thing R2b is about moved by 3.4×. A gate measured against a figure with a megabyte constant in it cannot be met by any amount of eviction, and two changes were made chasing it before the histogram showed where the bytes were |
| D268 | **R2b is met where a pixel is coarser than a brick, and cannot be met nearer than that** | measurement | Measured on screen-dependent bytes, half resolution against a quarter of full: **far 1.17×, distant 0.68× — both inside the 30% tolerance.** The near cameras are over (outdoor 2.64×) and it is structural rather than a defect: a brick is the leaf, and at 1280×800 with a 90° lens a brick covers a pixel at **100 m**, so everything nearer is pinned at the floor and halving the resolution cannot coarsen it. The facility spans ±17 m and the outdoor camera sits at 60 m, so the whole scene is inside that floor. The histogram says it plainly — level 3 holds 27,443 nodes of 38,438 at full resolution and only falls to 12,902, while on the far camera level 6 disappears altogether and the whole histogram shifts by one, which is the rule working exactly as written. **This is the same floor R8 lifts from the other end**: sub-voxel levels are what make the near case behave like the far one |
| D269 | **Resident nodes are reported per level** | — | A total says memory fell by 22% and cannot say which levels failed to move. The histogram answered in one reading what two changes and a session of guessing had not: the levels were shifting correctly and the metric was carrying a constant |

## The renderer rewrite — R2c, the twenty metres

| # | Decision | Source | Notes |
|---|---|---|---|
| D270 | **The proximity radius asks the world what it holds, not the volume what it contains** | D199 | It asked for a single node at the *entry* level: it stepped by `1 << kEntryLevel`, which is 512 m, over a range of 640 voxels, so the loop ran exactly once per axis. It was not holding twenty metres of anything — it was making sure the root existed, and collision, physics and editing cannot be served by a 512 m shell. Twenty metres is eighty bricks, so the cube around the camera is **4.2 million cells** and almost all of them are air the world has no record of; `world_has` at leaf level is a chunk lookup and one brick test, so iterating what the world holds visits the handful that exist |
| D271 | **The sweep is resumable and bounded, and anchored at two metres rather than at a brick** | measurement | Even 4.2 million cells is too much for one frame while somebody is walking, so it carries a cursor and does a slice a frame — about a hundred and thirty of them to cover the radius. It restarts when the anchor moves, and **a brick is twenty-five centimetres**: anchored there, a walking player restarts it four times a metre and it never finishes, which is the exact opposite of a guarantee. Anchored at 64 voxels it restarts every two metres. Measured in the facility: 0.490 ms of CPU steady, 27.6 ms on the frames the sweep is running |
| D272 | **It is a background guarantee, not an instant one** | D271 | Standing still finishes it; walking keeps it a couple of seconds behind. That is the honest description and it is what the test asserts — a slab two metres away that no ray has ever looked at becomes resident, and one a hundred and twenty-eight metres away does not |

**Noted, not fixed.** The steady 0.490 ms is mostly the erosion sweep rather than the proximity one:
it scans the node watermark up to twelve times a frame, which at 268,761 slots is some three
million checks. It is inside the 0.8 ms budget for streaming but it is most of it, and the obvious
improvement is to sweep only when something has actually gone cold.

## The renderer rewrite — the erosion sweep, and a worst frame that was startup

| # | Decision | Source | Notes |
|---|---|---|---|
| D273 | **The erosion sweep tests the timestamp before the node record, and walks a slice a frame** | measurement | It read the 32-byte record *first* and then the four-byte timestamp, so a settled frame touched every node in the pool — 268,761 records is **8.6 MB of memory traffic to discover nothing had gone cold** — and it did that every frame when `cold_frames` is six hundred. Now the one-megabyte timestamp array is walked and only a slot that is genuinely cold costs a look at its record, an eighth of the range at a time. **0.490 ms to 0.060 ms.** A freed slot is stamped on release as well, or it reads as cold for ever and costs the record read the ordering exists to avoid |
| D274 | **Ask when the worst frame happened before trying to make it smaller** | measurement | Three changes were made chasing a 27 ms worst frame: caching the chunk across the proximity sweep (29.9 → 27.4), quartering the build budget (27.4 → 23.2), quartering the proximity slice (23.2 → 22.9). Each moved it less than the last, which was the signal. **It was frame 16** — during load, before the world settled at frame 240 — so nothing done to the steady state could touch it, and two of those three changes were pure cost: a smaller build budget is slower fill-in and a smaller proximity slice is four times as long to reach twenty metres. Both restored. The worst-frame report now names its frame, which is the one line that would have prevented all three |

**Where it stands.** Steady-state node pool CPU is **0.063 ms on the close camera and 0.072 ms
outdoors**, against 0.490 before this and 252.8 before the request dedupe. The worst frames are 36
and 96 ms and both are during load — real, worth fixing, and a different problem from the one this
was about.

## The renderer rewrite — load time, which was mostly the system nothing reads

| # | Decision | Source | Notes |
|---|---|---|---|
| D275 | **Loading reports where it went** | measurement | The per-stage times have been measured since the loading bar existed, to weight the bar, and were never printed — so "it takes about five seconds" had no breakdown behind it in a project whose first rule is to measure. They are printed now, with each init phase stamped against process start. The bar's own attribution turned out to be misleading on a cache hit, because a skipped stage absorbs the gap before the next one: it credited 2,450 ms to *sampling* on a run that did no sampling at all |
| D276 | **The chunk system is sized for what still reads it, which is the path tracer alone** | measurement | Stamped against process start, a warm load read the world from its cache at **t+126 ms** and was not ready until **t+2,033 ms**. The gap was `residency_.create` at **1,432 ms** and the eight thumbnail tiers at **266 ms** — **83% of the load**, spent zeroing pools sized for most of the card, that the frame never touches. `visibility.comp` is behind `--chunk-marcher` and `pathtrace.comp` is the only thing left including `world.glsl`. At a tenth of the share: chunk residency **163 ms**, tiers **63 ms**, **ready at t+548 ms against t+2,033** — and the wall clock of a scripted run from 3.85 s to 2.11 s |
| D277 | **`--pathtrace` still takes the whole share** | D276 | It is the one thing that needs it, and it is a reference mode with no frame budget (D157), so it pays the 1,428 ms when it is asked for. **F4 from a default start therefore traces on a tenth of the budget** — the facility fits, and a larger scene may not hold every chunk. That is a real consequence and is stated rather than discovered: R1e removes the choice by removing the system |

**What is left of load.** Ready at 548 ms, of which the world itself is 126. There is no single large
item left in it — the next thing worth attacking is the *cold* path, where a first build still
sharpens region by region and pastes on the main thread for up to 17.4 s at a time.

| # | Decision | Source | Notes |
|---|---|---|---|
| D278 | **R3 is done before R1e, reversing the plan's order** | — | R1e requires moving `pathtrace.comp` from `world.glsl` to `node.glsl`, which §8 calls "the bulk of the work" — and §9 **deletes** `pathtrace.comp` at R3 and replaces it with the face pass. Porting it means building something to throw away one stage later, and doing R3 first leaves R1e with nothing to port. What it costs is the chunk system staying in the build a while longer, which since D276 is 226 ms of load and about 12 ms of CPU a frame rather than 1.7 s and the same 12. That is a much smaller price than the throwaway |
| D279 | **`12-plain-english.md` covers the rewrite** | — | It is the one document written for the person the work is for, who does not read code, and it had nothing about any of this — while the decision log had seventy entries. Owed since the handover was written and noted as owed each time, which is not the same as writing it |

## The renderer rewrite — R3, the face store

| # | Decision | Source | Notes |
|---|---|---|---|
| D280 | **The dirty-range map is shared rather than copied** | — | `DirtySet` moved to `core/dirty_set.hpp` when the face store became the second structure to mirror a CPU array onto the card. A coalescing rule written twice is a coalescing rule that drifts, and a dirty map that disagrees with itself is a stale byte on the GPU — the one class of fault that shows up as a wrong picture rather than an error |
| D281 | **The face store gets the node pool's audit, before any shader reads it** | D236, D265 | Same shape throughout: two device buffers mirroring two CPU arrays, dirty ranges rather than whole prefixes, and a read-back that names the first byte that disagrees. On the node pool that check found three stale-byte bugs in one sitting and each was named to the byte. It also waits for the frame *before* touching the staging ring rather than only after submitting, which is the fault D265 records — an audit that causes what it exists to report |
| D282 | **An evicted bucket is a tombstone, and a tombstone is not a slot** | bug | Emptying a bucket mid-probe cuts every face behind it out of its own sequence: they are still in the table, can no longer be found, and nothing anywhere says so. So eviction leaves a marker that ends nothing and is reusable on the way past. `find` stopped only on the *empty* marker, read the tombstone as a slot number, and indexed `faces_[0xFFFFFFFE]` — SIGSEGV on the first run of the test written for that case, which is the headless-first rule earning its keep before a shader existed to blame |

| # | Decision | Source | Notes |
|---|---|---|---|
| D283 | **A face request goes down the feedback buffer that already exists** | — | It is the same four integers a node request is — a coordinate and a level — with the direction packed beside the level, so it needs no second buffer, no second capacity, no second readback and no second barrier. The node the ray stopped on and the direction it was hit from are both already in hand at the moment it stops, so nothing is recomputed and the marcher and the face pass cannot disagree about which face they mean |
| D284 | **The premise of R3, measured on the real thing at last** | measurement | The stage rests on there being far fewer faces than pixels, which D205 checked by counting distinct face keys in an image before any of this was built. Now the store itself says it: **64,000 requests a frame collapse to 4,178 live faces enclosed and 14,232 close** — fifteen to four times fewer, from one pixel in sixteen. Sampled at one in sixteen rather than one in four because these share the feedback buffer's 131,072 capacity with the node reports, and one in four at 1280×800 is 256,000 requests, which truncates silently and costs convergence |
| D285 | **The face figure is reported apart from its table**, immediately | D267 | 14,217 faces is 455 KB of faces and 8.8 MB with the bucket table, which is fixed and has nothing to do with what is on screen. D267 was a session spent chasing a gate that could not be met because a megabyte of constant was buried in the number being measured. Reporting both from the first line written is cheaper than learning it twice |

| # | Decision | Source | Notes |
|---|---|---|---|
| D286 | **The CPU owns which face a slot is; the card owns what arrives there** | — | The whole point of the stage is that the card writes the light, so a byte-for-byte mirror check would report a mismatch on every frame that shaded anything — an audit that cries wolf until somebody turns it off, and this one has already caught three real stale-byte bugs a photograph never would have. So the bucket table is compared exactly and a face is compared on **identity alone**: key, level, direction, flags. A slot the CPU thinks is face A while the card thinks it is face B writes light onto the wrong surface and looks like a shading bug for ever, which is the fault worth keeping the check for. What is *in* a face is the card's business and is not compared |

## The renderer rewrite — the face pass shades

| # | Decision | Source | Notes |
|---|---|---|---|
| D287 | **`shade_faces.comp`: one invocation per face, dispatched by face count and not by pixels** | — | Sun visibility, one shadow ray per face per frame, averaged so the penumbra resolves over samples on the *face*. Measured: **0.162 ms at 1280×800, 0.170 at 1440p, 0.177 at 4K** against a 4.40 ms budget — flat across a 8.1× change in pixels, which is the claim the whole stage is for. Visibility over the same range goes 1.62 → 4.91 → 10.03 |
| D288 | **The audits were an illegal copy, and had been since they were written** | `--validation` | The device buffers were created without `TRANSFER_SRC`, so every `vkCmdCopyBuffer` in both mirror checks was undefined behaviour that happened to work on this driver. The check that found three real stale-byte bugs was itself unsound, and nothing had run with `--validation` since it was written — which is trap 1 in the handover almost exactly: ask whether the tool is connected before trusting what it says. Found by running validation because a pipeline was new, which is the rule that trap states |
| D289 | **A sampling rate that does not follow the resolution measures its own truncation** | measurement | Face requests at a fixed one-in-sixteen are 518,400 at 4K against a 131,072 buffer, and node read-reports at one-in-sixty-four are another 129,600. Two thirds were dropped, and the face pass then measured **faster** at 4K than at 800p — 0.038 ms against 0.195 — purely because it had been handed a third of the work. That reads exactly like the resolution independence this stage exists to demonstrate, which is the worst way for a measurement to be wrong. Both strides now scale with the pixel count: **0 dropped at every resolution**, and the flat figures above are real |

## The renderer rewrite — the composite reads the faces, and five bugs between here and a shadow

| # | Decision | Source | Notes |
|---|---|---|---|
| D290 | **The marcher resolves the face slot; the composite only reads it** | — | `shade_faces.comp` had been computing sun visibility for two stages and **nothing read it** — `resolve.comp` still lit every pixel itself, with no shadow term at all, so the real-time path had never had a shadow. The slot is looked up once in `node_visibility.comp`, where the key is already in hand, and written to an R32_UINT image the composite samples. Rebuilding the key in the composite from a depth and a normal would be a second piece of code that has to agree with this one about which face a pixel is on, and the two would eventually not — D133 and D147 through a third door. One word a pixel: the whole key is four, which at 4K is 265 MB a frame for something a single probe answers |
| D291 | **A sampling lattice that never moves is not a sample** | measurement | Face requests were taken at a fixed one-pixel-in-sixty-four. That does not sample one face in sixty-four; it samples *the same* faces every frame and the other sixty-three never exist. The store settled at **630 faces against about thirty thousand on screen** and stayed there — 278,195 claims of which 277,565 were for a face already claimed. Walking the phase one column a frame visits every offset in stride² frames, about a second, and the cold window is 600: **630 → 15,038 live**. The node read-reports had the same fixed lattice and the same latent fault, and now walk with it |
| D292 | **A shadow ray must not stand in for what the pool has not built** | measurement | R2d has a primary ray draw the parent when the world says a cell is occupied and the pool has no detail there, because the alternative is sky where a building is. A shadow ray doing that treats "I do not know" as "opaque" — and the tree is refined only where the *camera* looks, while a shadow ray leaves the surface towards the *sun*, so it meets an unbuilt cell almost at once. Measured with it on: 15,038 faces settled, 14,325 fully shadowed, and **not one face anywhere in the scene lit**, on an outdoor camera at midday. `node_march` now takes the behaviour as an argument. The failure it swaps to is a shadow that arrives late instead of a world that is black and stays black |
| D293 | **Visibility is two counts, not a running mean** | measurement | An eight-bit mean cannot converge: once it is within half a count of its target the update rounds back to where it started. A face lit by every one of **494 rays read 245 of 255** and would never have read more, so "always lit" and "nearly always lit" were the same number and no test could tell them apart. The word now holds rays cast and rays that reached the sun, and the fraction is worked out where it is read — exact at any sample count, at the same eight bytes. Both halve at 256 so a face still notices the sun moving. **brightest 245 → 255, 0 fully lit → 8,006 of 11,214** |
| D294 | **The shadow ray is jittered across the face and across the sun's disc** | picture | One ray, always through the face centre, always exactly at the sun, gives every sample of a face the same answer — so the fraction can only be nought or one and the shadow edge lands on face boundaries, which at the leaf level is a 25 cm staircase across a flat terrace. Spreading the samples over the face makes the fraction mean what it says, and the edge resolves *within* a face over frames at no extra cost per frame. That is §6's claim exactly: the penumbra costs the face a few samples instead of costing every pixel looking at it a ray of its own, for ever |
| D295 | **A record written from both sides cannot be uploaded by range** | — | The CPU owns which face a slot is and the card owns the light on it (D286), and the uploader coalesced dirty runs across up to sixty-three clean records — sending the CPU's copy of each, whose counters are nought because the CPU has never seen what the card computed. Every upload wiped the sun off the faces around each new one. It is now one copy with many regions and no gap, so exactly the changed records go. Splitting the record so the two owners never share a copy is R3d's |
| D296 | **What the card wrote is reported, split by whether the face can see the sun** | D288 | The mirror check compares identity and deliberately not light, so the one field nothing could see was the one the stage exists to produce. A picture cannot tell "every face is shadowed" from "every face is unlit" — both are a dark building and they have nothing in common to fix. Four of the five bugs above were found by reading one log line, and the first three were invisible in a screenshot |
| D297 | **The face pass and its store were never torn down** | `--validation` | Three buffers alive at `vkDestroyDevice`, and `ComputePipeline`'s own destructor then ran against a device that no longer existed — an access violation inside the driver with no WorldShaper frame in the stack, which reads as a driver bug and is a missing line. The comment warning about exactly this was ten lines above the omission. The cloud history had been leaking the same way since it was written |

**What it costs.** Deck resolution, realtime, mean ms, against the same grid without the composite:
enclosed 2.382 → 2.500, outdoor 1.709 → 1.892, close 2.841 → 3.009, mid 1.073 → 1.221, far 1.393 →
1.552, distant 1.461 → 1.499, sky 1.287 → 1.364. **Three to eleven per cent for a shadow the
real-time path has never had** — and the speckle figure falls with it, close **98.3 → 24.6**,
enclosed 17.5 → 8.2, outdoor 15.4 → 10.6, because pixels on one face now read one answer instead of
each deciding for themselves.

## The renderer rewrite — faces are voxels, and two collisions on the value zero

| # | Decision | Source | Notes |
|---|---|---|---|
| D298 | **A face is the voxel the ray stopped on at the level the pixel resolves, not the brick the outer walk was stepping** | picture | The outer walk never steps finer than a brick, because the level decides the colour and must not decide the shape (D132) — and the face was being reported at that level. **Every face in the store was level 3**: 19,196 of them on the close camera, all 25 cm, so the smallest shadow the renderer could cast was a 25 cm block and flat stone came out chequered. Debug view 11 has keyed on the voxel at the pixel's level since before this marcher existed and counts **609,592** faces on the same view, which is the figure §6's arithmetic is written against — *a voxel covers a whole pixel at 22.5 m, so everything nearer is at level 0*. Now **416,261 level 0, 59,758 level 1, 1,603 level 3**. `voxel` is brick-aligned and `inner` is under eight, so at level 3 and above the shift is unchanged: the refinement is in the near field and nothing else moves |
| D299 | **Two things meant "nothing" by being zero, and both stopped being true the moment a face could be a voxel** | user report | Neither produced an error and both produced a plausible picture. **(a)** `NodeHit.face_level` used 0 for "this ray stopped on nothing", so `node_visibility.comp` skipped the face lookup for every level-0 face — which is every face within twenty metres of the camera. The composite got `kNoFace`, fell back to full sun, and **the shadows vanished from exactly the region the change was meant to sharpen**. **(b)** `pack_face(0, 0, 0)` — level 0, direction +x, no flags — is literally zero, which was the store's spelling of an empty slot, so **one live face in six** was skipped by the shading pass, never settled, and read as fully lit for ever. `kNoFaceLevel` and a `kFaceLive` marker bit replace both. A sentinel that is also a legal value is a bug waiting for its range to widen |
| D300 | **The shading pass has a budget, and it is a cap on convergence rather than on framerate** | measurement | Faces went from nineteen thousand to four hundred and seventy-seven thousand on one camera, and shading all of them every frame is milliseconds. One face in `face_stride` is traced each frame, round robin, with **a face that has not settled never held back** — so a surface that has just come into view converges in a handful of frames however large the store is, and only the refresh rate of settled faces falls. That is the right thing to give up: they are watching a sun that has not moved. This is R3a's face-select box, folded into the shading pass rather than run as its own compaction, because what costs is the march and not the record: an invocation that is not due reads thirty-two bytes and stops. **0.185 ms at 19k faces, 0.413 ms at 477k, 0.477 ms at 639k** |
| D301 | **A full face store says so** | — | 639,233 faces at 4K on the close camera against a budget of 1,048,576. A larger scene reaches the cap, and a cap nobody reports looks exactly like geometry that will not take a shadow — the same failure D299 had just produced by a different route |

| D302 | **A shell is opaque to a ray that is deciding what you can see THROUGH** | user report, measurement | A shell — a node the world says is occupied whose children have never been built — is deliberately not drawn by a primary ray: it has been folded from nothing, its colour is nought, and drawing it paints a building black (R2d). So it falls through and the ray carries on as though the cell were empty. That is right for deciding what you can *see* and wrong for deciding what you can see *through*. Indoors, where every correct answer is full shadow, **47,353 of 93,745 sun-facing faces leaked about a tenth of their rays**, with not one pixel of the room above 0.9 for that light to have come from — the sun coming in through walls that had not finished streaming. Occlusion now stops at WANTED with no colour and no conditions, because for occlusion there is nothing to draw. **Enclosed mean visibility 0.05 → 0.00, fully shadowed 46,392 → 93,741 of 93,745, and 255,999 of 256,000 sampled pixels below 0.1** |
| D303 | **Correction to D292** | — | D292 said turning the stand-in off for shadow rays was what stopped the scene being uniformly black. Measured in isolation afterwards, it moves 3,840 faces of 93,745 — about four per cent. **The counters (D293) were the fix**; the stand-in was changed in the same edit and took the credit. What it *was* hiding is D302, which the stand-in could never have fixed anyway: a shell fails the `foldable` test, so it was passed through whatever the stand-in said |

**The speckle figure, which turned out to be measuring this.** Deck realtime, enclosed / outdoor /
close, across the four states — no shadows, brick faces, voxel faces with the leak, voxel faces
without it: **17.5 / 8.2 / 21.7 / 3.8**, 15.4 / 10.6 / 10.5 / 9.8, 98.3 / 24.6 / 22.8 / 19.3. The
enclosed regression noted below was the leak and nothing else: a tenth of the rays escaping through
unbuilt walls, independently per face, is precisely a per-voxel speckle pattern. It now reads
**better than the renderer did before shadows existed at all**, on every view.

**What it costs, and one honest regression** — the regression is resolved by D302 above, and the
paragraph is kept because the reasoning that led to it is what found the leak. Deck realtime, mean
ms, across the three states —
no shadows / brick faces / voxel faces: enclosed 2.382 / 2.500 / **2.774**, outdoor 1.709 / 1.892 /
**1.797**, close 2.841 / 3.009 / **3.273**, mid 1.073 / 1.221 / **1.170**, far 1.393 / 1.552 /
**1.418**, distant 1.461 / 1.499 / **1.445**, sky 1.287 / 1.364 / **1.305**. Distance views came
back down to the no-shadow baseline, because there is little at level 0 in them.

Speckle went **17.5 → 8.2 → 21.7** enclosed and **98.3 → 24.6 → 22.8** close. The 8.2 was flattery:
sixty-four pixels reading one brick's answer is a blur, and that blur is the blockiness this
change removes. Against the *no-shadow* baseline the enclosed figure is 21.7 against 17.5, and the
difference is per-face Monte Carlo noise at about a hundred samples a face. **R5 — face denoise —
is what that stage is for**, and this is the first measurement that says how much it has to do.

## The renderer rewrite — the face store only ever grew

| # | Decision | Source | Notes |
|---|---|---|---|
| D304 | **`FaceStore::evict_cold` was written, tested, and never called** | user report | A store that only grows reaches its cap and then refuses every face after it. The shape of that failure is not the one anyone would guess and is worth writing down: **shadows do not stop being drawn**. Faces that already have light keep it, so the picture holds whatever set of shadowed surfaces it had at the moment the table filled, and every surface discovered afterwards is lit by the composite's fallback. Reported as *"after some time the shadowed voxel faces stop being produced, so it stays as whatever was produced before"* — the mechanism described exactly, from the outside. Nothing was in a bad state and no counter read wrong; the store was simply full and nobody was emptying it. It is new in practice rather than in theory: at brick granularity one camera claimed 19,196 faces against a budget of 1,048,576 and a session would rarely reach it, but per voxel the same camera claims **477,622**, so two or three positions fill the table |
| D305 | **A full store takes the coldest it has; it does not wait for the cold window** | measurement | `cold_frames` is six hundred, so the ordinary rule gives up nothing until ten seconds after nobody has asked — and a player who fills the table faster than that gets no new faces in between, which is the bug above with a timer on it. When the table is full the threshold halves until something comes free, down to a floor of 32 frames that is far above the handful a face needs to settle. Rate limited to once in sixty frames, because it scans the whole table several times over and a store whose faces are all genuinely hot must not pay that every frame — that would turn "no new faces" into "no new faces and a frame rate to match". The ordinary sweep is **a slice a frame**, eight slices to the table, the same shape and the same reasoning as the node pool's erosion sweep (D273) |
| D306 | **`--face-budget`, so the full-table path is reachable in one run** | — | At the real budget it takes a player moving about for a while, which is precisely why this was found by playing and not by any test. Forced to 40,000 faces on a camera that wants 477,622: **510,373 evictions over 900 frames** and shadows still being produced, against a frozen set and zero evictions before. Two unit tests cover it headless — a full store that must recover inside the cold window, and a face asked for every frame that must survive however many sweeps run |

| # | Decision | Source | Notes |
|---|---|---|---|
| D307 | **A mirror check compares nothing while bytes are still owed to the card** | `--validation` | Both audits waited for the device before reading — D265's fix, for work already submitted — and neither asked whether anything was still *queued*. An upload too large for its staging ring in one frame leaves its ranges marked and sends them on the next, deliberately and documented; comparing during that backlog reports a host that has moved on against a card that has not been told yet, and calls it a stale byte. It presented exactly like a real fault: **intermittent, three runs in five, and only on the larger tree** — because a large tree is what produces uploads that do not fit. Caught firing on the frame it happens: at frame 25, mid-stream, *"mirror not compared: 6923 faces and 6923 buckets still owed to the card"*. Five clean runs after, including one with 364,687 evictions in flight. The failure mode this avoids is the one both audits' own comments name — a check that cries wolf until somebody turns it off, on the check that has caught four real stale-byte faults |

## The renderer rewrite — the shadow that arrives a second late

Reported from playing: *"shadows that are occluded by things are not drawn until you actually see
them, so when you move the camera indoors there is no shadow behind things for some time."* It is
not a bug in the shadows. It is the discovery latency of the face store, seen through a fallback
that is wrong in exactly the place it is most visible.

**The chain, in frames.** A face is claimed only when a primary ray lands on it and that pixel is
one of the one-in-sixty-four the request lattice is asking with. So a surface that was hidden and is
now visible waits for the lattice phase to reach its own pixel (up to 64 frames), then two frames
for the feedback readback and the claim, then four samples before the composite will read it —
**about seventy frames, over a second**. Until then the composite has no face and falls back to
`sun_visible = 1.0`. Indoors, where D302 measured 93,741 of 93,745 faces fully shadowed, full sun is
the most wrong answer available, and it is applied to precisely the surfaces the player just
revealed.

| # | Decision | Source | Notes |
|---|---|---|---|
| D308 | **A face with no light of its own reads the coarse face standing over it** | user report | R2d's rule — *draw the parent while waiting* — applied to light instead of colour, which is what R9d always said and what the plan now records the working form of. The stand-in is **three levels up, not one**: the quantity that matters is not how much coarser it is but how many faces share it. Three levels is 512 fine faces to one and about sixty-four times the screen area, so the request lattice that takes 64 frames to land on one pixel **cannot miss the stand-in at all** — it is claimed the frame a region appears and settled a few frames later. One level up is 4:1 and would wait nearly as long as the thing it stands in for. Three is also the brick, which is what every face in the store was before D298, so the picture has already been seen at this granularity and it read as blocky rather than wrong. Both the fine face and the stand-in are gated on being *answerable* — settled, not merely present — since the composite treats "missing" and "unshaded" identically |
| D309 | **The stand-in is derived from the request, not reported beside it, and claimed only when the face under it is new** | measurement | The marcher could report both keys. It must not: an ancestor key is a *shift* of its descendant's, so sending it is bandwidth spent on something computable, and it would double face traffic through the one buffer that is already the binding constraint — 57,600 face samples a frame at 1440p against a capacity of 131,072 shared with the node reports. The CPU shifts the key instead, at no traffic at all. Then the same argument again one level down: claiming the stand-in on *every* report is 16,000 extra probes a frame that change nothing, measured at **+0.24 ms of CPU while turning**, and the case they would serve cannot occur — a stand-in is wanted for geometry the store has not seen, and geometry the store has not seen has no repeat reports to hang a claim off. Gated on `FaceStore::claim`'s new `was_new`, which costs nothing because `claim` has already done that probe. **The convergence curve is identical with the gate and without it**, which is the measurement that says the repeats bought nothing |
| D310 | **Debug view 16 painted "no geometry" the same black as "fully shadowed"** | measurement | Trap 7 — *"nothing here" and "I could not fit it" must never be the same answer* — in the one place built to answer questions, and it produced a wrong number immediately: the first histogram of this change read the enclosed room as **0.3% surface at frame 40**, because by then nearly every pixel had settled at visibility 0 and had been counted as sky. Sky is green there now. The view is otherwise unchanged: grey is the visibility fraction, magenta a face the composite could not find, blue one it will not believe yet |
| D311 | **`Copy-Item` preserves the source's timestamp, so restoring a shader leaves a stale `.spv`** | measurement | The A/B was run by editing a shader, measuring, and copying the saved original back. The restored file carried an mtime *older* than the SPIR-V built from the edited one, ninja called it up to date, and the "after" run measured the "before" build — **identical to the digit at five frames**, which is what gave it away. Trap 2 with a different cause and the same shape: a stale `.spv` behind a build that reports success. Touch a restored file, or restore it with `git stash pop`, which stamps it |

**What it measures.** The instrument is debug view 16 with the fix in D310, histogrammed over the
enclosed camera from a cold face store — which is the same state as geometry that has just been
revealed, since neither has ever been claimed. Share of surface pixels falling back to full sun,
1280×800, quality 7, one scene (content `766f2fd63f1a01c4`) across every run:

| frame | 15 | 20 | 25 | 30 | 40 | 50 | 60 | 80 |
|---|---|---|---|---|---|---|---|---|
| before | 98.9% | 81.9% | 54.2% | 41.0% | 27.7% | 14.0% | 5.2% | 0.1% |
| **with the stand-in** | 91.9% | **45.2%** | **11.4%** | **0.7%** | **0.3%** | **0.2%** | **0.1%** | 0.0% |

Under one per cent at frame 30 rather than frame 78, and at frame 40 it is **2,978 wrong pixels
against 283,291** — ninety-five times fewer. What is left at frames 15–20 is the store having
nothing settled at all yet, stand-in or otherwise; nothing can fix that but the four samples.

**What it costs.** Nothing measurable on the GPU, still or moving: three views, deck resolution,
300 frames, `-Tolerance 0.03` — *nothing regressed*, both from a still camera and turning at 60°/s.
The settled picture is **bit-identical**, 0 pixels of 1,024,000 — the stand-in is read only while a
face is unsettled, so once the store has converged this change is not in the frame at all. The
store grows by the stand-ins themselves: **113,043 faces → 116,389** on the enclosed camera at frame
200 (+3.0%, 107 KB), of which levels 3–4 hold 4,681 against 1,466 before. CPU while turning is
inside the harness's own run-to-run spread on that column (0.7 ms between two runs of one build),
which is why D309's ungated variant needed three runs a side to be seen at all.

## The renderer rewrite — shadow latency, stage one of getting it to a hundredth

D308 gave a face with no light of its own something to read. This is the other half of the same
report: how long a face waits before it has light at all. Asked for as *"make shadows load faster
drastically and less falling back to full sun — in stages, to 1% of what they currently are"*.

| # | Decision | Source | Notes |
|---|---|---|---|
| D312 | **`--cut f,x,y,z,yaw,pitch`, because none of this was measurable** | — | Every other camera in the harness moves smoothly, and smooth motion reveals a sliver of new world per frame — so it measures the *rate* the store converges at and hides what happens when it is handed a whole screen at once. A cut is the worst case and also the ordinary one: turning round in a doorway is a cut as far as the face store is concerned. Counted in *measured* frames, so under `--settle` it fires after the world has stopped building and the only thing the new view is missing is light. The first thing it showed was that the cold-store curve everything had been measured against was half streaming |
| D313 | **The face store's mirror was uploaded above the line that fills it** | measurement | `face_buffers_.upload` sat a few lines *before* `stream()`, which is where claims happen — so every face claimed on a frame missed that frame's copy and reached the card on the next one. A whole frame of latency on exactly the faces that had none, spent on nothing, and invisible because the picture it produces is the one that arrives anyway a frame later. `shade_faces` is dispatched hundreds of lines below, so nothing ever needed the earlier position |
| D314 | **The composite reads a face at one sample, not four** | user report | Four was three more frames of the fallback, and the fallback is full sun. The argument for four — one sample is a coin toss, not a penumbra — is true and is the wrong comparison, because it leaves out what is shown *instead*. In a room where every correct answer is shadow, the choice is **one face in twenty wrong for one frame** against **every face wrong for four, in the same direction**. Bias is what a player reads as "the shadows have not arrived yet"; variance is a speckle gone before it registers, and R5 is the stage that exists to take it. The old four keeps its other job under its own name, `kFaceEager`: a face that may be *read* at one sample is still *converging* at four, so it is never held back by the shading stride until then, and the audit counts it as evidence only above it |
| D315 | **Rejected: tracing the missing samples as a burst in one invocation** | measurement | It works, and it costs more than it is worth. With the trip count dynamic the pass measured **0.699 ms against 0.559** in a frame where every face was settled and the loop ran exactly once — a quarter of the pass, paid for ever, to buy what D314 gives for nothing. Hoisting the first ray out of the loop and leaving only the extras inside made it **0.769**: two inlined marches are worse than one dynamic loop. Written down because the idea is the obvious one and will occur to the next person |

**Measured, and this is the figure to beat.** Enclosed room, settled world, camera cut through 180°,
share of surface lit by the full-sun fallback:

| | cut+1 | cut+2 | cut+3 | cut+5 | cut+8 | cut+15 | cut+30 |
|---|---|---|---|---|---|---|---|
| before | 100% | 100% | 100% | 100% | 6.8% | 0.3% | 0.1% |
| **after** | 100% | 100% | **73.9%** | **2.6%** | **0.6%** | **0.2%** | 0.1% |

Five frames of a completely unshadowed room, then a sixth still mostly wrong, becomes two frames and
a third that is a quarter right. The cold-store curve moves with it: under 1% at frame 25 rather
than 30, and 21.0% against 45.2% at frame 20.

**What is left is the feedback round trip**, and it is exactly the two frames still reading 100%: a
face is reported by the visibility pass, read by the CPU two frames later because that is when the
frame it was written in has retired, and claimed then. Nothing on the CPU can shorten it. **Stage
two is claiming on the card**, in the pass that discovers the face, which is the only thing that
removes those two frames — and the reason it is its own stage is that it moves who owns a face's
identity, which is the fault line D295 and D307 are both written about.

## The renderer rewrite — stage two: the card claims its own faces

D312–D315 took a hard 180° cut from five frames of a completely unshadowed room to two. Those two
were the feedback round trip and nothing else: the GPU writes a report on frame N, the host cannot
read it until N+2 because that is when frame N retires, and no arrangement of host code shortens it.
This is R3e.

| # | Decision | Source | Notes |
|---|---|---|---|
| D316 | **The card may claim a face, in the pass that discovers it** | measurement | A provisional table, holding **stand-ins only** — the coarse face `kFaceAncestorStep` above the fine one, which 512 faces share, so a screen needs a few thousand of them rather than the 477,622 the close camera claims per voxel. Records live in the **tail of the same buffer** the store mirrors into, above `max_faces`, so a slot is a slot: the composite reads `faces.items[slot]` with no tag bit and no second binding, and `resolve.comp` needed no change at all. The two allocators can never meet — the host hands out `[0, watermark)` and nothing in the upload path or the audit walks past the watermark — which is what keeps D295 unrepresentable rather than merely avoided. The buckets are its own, because the store's bucket array is uploaded from the host every frame and would overwrite anything the card wrote into it |
| D317 | **The slot IS the bucket, and that is the whole trick** | — | There is no allocator: a face's slot is the bucket its key hashes to, so a claim is one `atomicCompSwap` — and **the pixel that loses the race learns the winner's slot from the value the atomic hands back, in the same instruction**. That is what matters, not the saving. The alternative — allocate a slot, then publish it — leaves every other pixel on that face reading what the winner wrote, and nothing orders two workgroups, so they would all wait a frame. That frame is the one this stage exists to remove. The word carries the frame it was claimed in and a 24-bit tag from a second, independent hash, so a stale entry is free without any sweep and a false match needs the same bucket *and* the same tag — about one pair in five hundred billion, costing one coarse face one frame of another's shadow |
| D318 | **The visibility pass writes faces now, so the shading pass has to be told to wait** | — | The only barrier in the frame was the one before the composite. A claim and the ray that fills it are otherwise a race, and its failure mode is a face reading fully lit for one frame — which is exactly what this stage removes, so it would have looked like the change not working. A provisional record also keeps `kFaceLive` for ever, so the shading pass gates on the mark's frame stamp: without that, every bucket ever used would be traced again every frame, thirty-two thousand rays a frame spent on stand-ins nothing is looking at |

**Measured.** Enclosed room, settled world, camera cut through 180°, share of surface lit by the
full-sun fallback:

| | cut+1 | cut+2 | cut+3 | cut+5 | cut+8 | cut+15 |
|---|---|---|---|---|---|---|
| before stage one | 100% | 100% | 100% | 100% | 6.8% | 0.3% |
| after stage one | 100% | 100% | 73.9% | 2.6% | 0.6% | 0.2% |
| **after stage two** | **0%** | **0%** | **0%** | **0%** | **0%** | **0%** |

The cold-start curve goes the same way: **0% at every frame measured**, against 30.8% at frame 15
and 21.0% at frame 20 after stage one, and 91.9% / 45.2% before it.

**And the answer is right, not merely present.** At cut+1 the enclosed room reads 1,023,994 pixels
fully shadowed with a mean visibility of 0.0000 — *identical to the same camera 120 frames later*.
On a cut where the answer is not uniformly black (outdoors to the steps), cut+1 reads a mean
visibility of **0.0489 against 0.0446** converged, with 0.9% still on the fallback: the stand-in is
about a tenth too bright and sharpens over the next hundred frames, against a fallback that was
1.0 — twenty times too bright — for eight of them.

**What it costs: nothing measurable.** Three views, deck, 300 frames, `-Tolerance 0.03`: *nothing
regressed*, still or turning at 60°/s, and the enclosed view sits at 2.739 ms against 2.743 before
any of this work. Two runs at one frame are bit-identical (D194), the mirror audit passes by
identity, and `--validation` is clean. No host work was added and nothing new is uploaded.

## The renderer rewrite — an edit's own shadow

R3e made a *revealed* surface light up in the frame it appears. An EDITED one did not, and for the
opposite reason: its face is already in the store, already converged, and confidently wrong.

**What it looked like.** A slab placed in mid-air over sunlit roof, camera framing the lit side.
Share of the frame fully shadowed and its mean sun visibility, against the same scene with the slab
present from the start (the converged truth: **7,493 shadowed, mean 0.8303**):

| | edit+1 | edit+3 | edit+10 | edit+30 | edit+300 |
|---|---|---|---|---|---|
| before | 3,614 / 0.861 | 3,614 / 0.861 | 3,682 / 0.894 | 3,609 / 0.890 | **3,903 / 0.8435** |
| after | **19,798 lit → shadowed begins / 0.759** | — | 3,661 / 0.799 | **6,998 / 0.822** | **7,222 / 0.8292** |

The old number to read is the last column: after **three hundred frames** the picture had reached
**52%** of the shadow it was supposed to have, and it was still climbing. The slab's *own* faces
were right immediately — they are new, and R3e claims them in the frame they appear — so what a
player saw was a block that casts no shadow while looking perfectly lit itself.

| # | Decision | Source | Notes |
|---|---|---|---|
| D319 | **A sample that contradicts a unanimous history is the world having changed, not noise** | measurement | The window alone cannot do this and the arithmetic says why: a lawn face sits at 256 lit of 256, so the first ray blocked by a new wall moves it to 256 of 257, and it needs 128 more to reach a half — at one ray every `face_stride` frames. Shortening the window is the wrong fix; it costs every face its penumbra. **Unanimity is what makes a contradiction meaningful**: a face that has seen the sun on every one of two hundred rays and is now told it cannot is not observing noise. Such a face keeps its answer and loses its confidence — two samples, same ratio — so the next ray moves it a third of the way and, being under `kFaceEager`, it is shaded every frame until it re-settles. A face already at a half has no claim to make and is left alone. The false positive is a face whose true visibility is a shade under one: it dips for two or three frames and climbs back, which is a cost paid by nearly-unanimous faces rather than by the penumbra, where a wrong answer would actually show |
| D320 | **An edited region goes to the front of the shading queue** | measurement | D319 fixes how fast the answer moves once a ray notices; this is how long until one does. A settled face is refreshed one frame in `face_stride` — three at the enclosed camera, seven at 4K — so detection alone was up to seven frames. The region is the edit's own bounds grown by the reach of a shadow, computed on the host and **already in the parameter block**, because the path tracer has needed that box since long before the face store existed (`kEditShadowReach`, `kShadowRefreshFrames`). Reading it in the shading pass is a bounds test and nothing else. Detection went from three frames to **one** |
| D321 | **Rejected: asking the node pool for the bricks an edit invalidated** | measurement | It looks like the fix for the carved-skylight case below, and the chunk half of `invalidate_edited_chunks` is written around exactly that argument — *"a chunk the player just built is not a guess about what might be looked at"*. Tried, and it moved no number in three hundred frames, while costing a request per brick per edit; the pool's own counters showed **nothing deferred and nothing starved**, so the premise that the rebuild was being delayed by budget was simply wrong. Reverted. Written down because it is the obvious next thing to try and somebody will try it again |

**Measured after.** Detection in **one frame**, 93% of the converged shadow by **edit+30**, and 7,222
against a truth of 7,493 by edit+300 — where the old behaviour was 3,903. Nothing regressed on the
grid, still, at 3% tolerance; **speckle is unchanged** at 3.8 / 19.3 / 9.9 enclosed / close /
outdoor, which is the figure that would have moved if the demotion were firing on ordinary faces;
450 tests pass; and the R3e reveal case still reads 0% on the fallback at cut+1.

**The carved-skylight case was never real, and the control says so.** Three hypotheses were raised
about why light through a freshly carved hole "arrived at edit+300"; the fourth measurement was the
one that should have been first. **A run with no edit at all reads the same number**: mean visibility
0.0458 with 68,142 partial pixels at frame 700, against the carve's 0.0457 and 68,048. Whatever was
being watched had nothing to do with the edit.

| Hypothesis | How it died |
|---|---|
| The pool is slow to rebuild the carved bricks; ask for them outright | No number moved in 300 frames, nothing deferred, nothing starved (D321) |
| An unbuilt cell occludes (D302), so the hole is opaque to the sun and transparent to the eye | Turning `occlude_unknown` off leaks the sun through unstreamed walls exactly as D302 says (0.0000 → 0.0092 *before* any edit) and still does not deliver the beam |
| The onset is the 600-frame cold window recycling stale faces | **Predicted**: quartering `cold_frames` to 150 moves the onset by the same factor. **Measured**: the onset does not move at all — still nought at edit+220, present at edit+300 |
| — | **Control with no edit reproduces the number to three decimal places** |

**What was actually found, and it is worth more than the question that led to it.**

| # | Decision | Source | Notes |
|---|---|---|---|
| D322 | **A sealed room fills with sunlight as the node pool sheds, with a static camera and no edit at all** | measurement | Enclosed camera, nothing touched, `--debug-mode 16` mean sun visibility against the pool's own node count: frame 500 **442,968 nodes → 0.0000**; frame 660 **266,840 → 0.0000**; frame 700 **6,972 → 0.0458**; frame 900 **6,972 → 0.0596**, still climbing. The room is sealed and every correct answer in it is nought (D302 measured 93,741 of 93,745 faces fully shadowed). The shedding itself is R2 doing its job — pixel-driven residency keeps what the screen needs — so the fault is that **a shadow ray gets a different answer from geometry the pool has given up than from geometry it holds**, which is D302's rule arriving from the eviction side rather than the streaming side. Reproduced on `f902a00`, *before* any of this session's work: **0.0266** at frame 900 on the same camera, same collapse |
| D323 | **D314 and D319 amplify it, and that is the right way round** | measurement | Same camera, same frame 900: **0.0266 before this session, 0.0596 after.** A face may now be read at one sample (D314) and a unanimous face demotes to two on a contradicting ray (D319) — so a single leaked "reached the sun" moves a wall from 0.00 to about 0.33 where it used to move it to 1/257. Both rules are doing exactly what they were built to do; what they are amplifying is a wrong ray, and the answer is to stop the ray leaking rather than to go back to hiding it. Written down because the tempting response to the number going up is to revert the change that made it visible |

**Where to start on it.** The pool sheds to 6,972 nodes for the enclosed view while the roof, the
outer walls and everything else the camera cannot see are given up — and a shadow ray needs exactly
that geometry. Either an evicted subtree must keep reading as WANTED to an occlusion ray (which is
what D302 already says a *shell* must do, and would make it dark rather than bright), or residency
has to count a shadow ray's needs as use. The second is the honest one and is what R9's off-screen
set is for: light is a world-space quantity and the set that keeps it resident cannot be the set the
camera can see.

**Done, and it was the first of the two.**

| # | Decision | Source | Notes |
|---|---|---|---|
| D324 | **A cold root sheds its subtree and stays standing as a shell** | measurement | Root eviction used to free the subtree, clear the node, clear its entry and drop its `live_` record. That changed the pool's answer for a whole 512 m block from *"something is here I have not built"* to *"nothing is here"* — the one distinction this structure exists to make unrepresentable (D133, D147) — and occlusion believes it: an unbuilt cell is opaque, an empty one is open sky (D302). A sealed room's roof is never on screen, so it always goes cold, and the room filled with sunlight. **A cold root now keeps its entry, its `live_` record and its node, and only `children` goes.** What is left is exactly what `build_shell` makes, both sides already read it as WANTED, and `refine` rebuilds it in place because it allocates a run whenever `children` is `kNoNode`. Effectively all the memory is in the subtree, so shedding recovers it anyway: the enclosed view still settles to **7,168 nodes**. Keeping the entry repairs a second, latent fault — the entry table is open addressing with linear probing and both `find` and `locate` stop at the first empty bucket, so clearing a cell mid-probe-run cut every root behind it out of the table, which then rebuilt as a *second* copy of a root already resident |

**Measured, enclosed camera, 1280×800, quality 7, static, no edit.** The instrument is the faces
audit at the screenshot frame, printed at four decimals — two was not enough, because a mean of
0.0049 is one face in a thousand wrongly in full sun and prints as `0.00`.

| | frame 500 before | frame 900 before | frame 500 after | frame 900 after | frame 5,000 after |
|---|---|---|---|---|---|
| mean sun visibility | 0.00 | **0.02** | 0.0002 | **0.0000** | **0.0000** |
| brightest face | — | — | 0.9910 | **0.0000** | **0.0000** |
| fully lit faces | 9 | **1,163** | 9 | **0** | **0** |
| fully shadowed | 96,323 | 90,567 | 96,324 | **92,337** | **92,342** |
| roots (level 14) | 8 | **4** | 8 | **8** | **8** |

Half the roots were being thrown away and the leak is exactly that. It holds to frame 5,000 with
`built 0 evicted 0` — a steady state, not a slower version of the same collapse — and the screenshot
shows the room dark: a studded far door and two lit urn alcoves where before it was sunlit.

**It costs nothing measurable.** The whole 42-run grid (7 views × 3 sizes × 2 modes) was measured on
this build and again on a control built from the same commit with the change stashed out, because the
committed baselines under `documentation/baselines/` are dozens of commits old and a move against
those would prove nothing about *this*. The grid total moved **1,672.5 ms → 1,680.3 ms, +0.46%**, and
**speckle is identical to two decimals in all 42 cells** — the same pictures, not merely the same
frame times. Three cells moved over 5% on one pass and all three were noise: realtime/sky/high read
11.162 → 15.274 and **11.183** on a repeat, pathtrace/close/deck 41.418 → 45.555 and **40.803**, and
realtime/distant/high moved 6.6% the *other* way. This is what one expects from the change — it
removes work from eviction rather than adding any, and the extra resident nodes are eight shells.

**Three things this does not claim.** The **9 fully lit faces at frame 500 are unchanged by it** —
they are there at full residency, identical in the control, so the R9i gate's literal *"stays at
0.0000 from frame 500"* is met at 900 and at 5,000 but not at 500, and the residual is a separate
leak and the obvious next target. The **`--cut` reveal case reads 0% on the fallback at cut+1
because there is no surface to read**: one frame after a 180° turn out of a fully shed pool the
newly revealed wall is not resident, so the shot is sky and a horizon. It is back and pixel-identical
to the settled image by **cut+30**. That is streaming latency, not a lighting leak, and it was not
measured on the control, so it is not known to be new. Finally the control's frame-900 mean read
**0.02** here against D322's **0.0596** on the same camera; different instrument (the faces audit,
not `--debug-mode 16`) is the likely reason and it was not chased, so treat the two columns above as
comparable to each other and not to D322.

**The test that used to assert the fault.** `a tree nothing reads is evicted, and that is the point`
checked `find(kEntryLevel) == kNoNode` — the root itself gone. That assertion *was* the bug. It now
checks `find(kLeafLevel) == kNoNode`, which is where the memory actually is and what the test's own
prose always said it was about; the companion case `a cold root sheds its subtree and stays standing`
asserts the other half, including that the rebuilt root is not a duplicate.

## The renderer rewrite — R10a, the ambient term gets its visibility

| # | Decision | Source | Notes |
|---|---|---|---|
| D325 | **Ambient occlusion is one cosine-weighted hemisphere ray per face per frame, and nothing else is new** | — | `resolve.comp` applied `kSkyAmbient * (0.5 + 0.5 * normal.y)` to every surface in the frame: how much sky a surface sees, decided from which way it points and from nothing else, so a wall at the back of a corridor received the whole dome. It is not an effect being added — it is the missing visibility on a term already being applied, and the reason it was never there is that visibility used to mean a per-pixel gather. Same face, same one writer, same two counts, same round robin, same `face_accumulate` as the sun: **a different set of directions and nothing else.** Malley's method, so the cosine weight is carried by the sampling and the mean IS the answer. `occlude_unknown` is on for the reason D324 gives — an unbuilt cell is matter, or a sealed room reads as open sky again |
| D326 | **It lives in a card-only array, not in `GpuFace`** | D295 | That record has two owners and D295 cost a session to it: the uploader sent the CPU's zeroed bytes over light the card had written. AO is written by the card, read by the card, and never looked at by the host, so it goes in `src/gpu/face_light.*` — allocated with the store, zeroed once at creation, never uploaded, never mirrored, never audited, never in a dirty range. There is no code path by which the host can overwrite it, which is a stronger guarantee than remembering not to. It also starts paying down R3d's standing debt rather than adding to it. A recycled slot is caught by the sun's sample count being nought, which is the host's own "this slot is new" signal |
| D327 | **The ambient constant was standing in for two things, and only one of them is occluded** | picture | Multiplying the whole of `kSkyAmbient` by measured sky visibility took the enclosed room to near black — correct about the sky, wrong about the room, and wrong in the direction that looks like the feature is broken. A windowless room gets no sky and is still not black, because its walls light each other. `kIndirectFloor` is the share of that constant which was never sky. Its value is **chosen, not tuned**: the orientation heuristic it replaces averaged 0.5 over a surface whose normals point every way, so 0.5 holds the enclosed room at the mean brightness it already had and the occlusion spends itself on **contrast** instead. Anything lower darkens the whole room uniformly, and a uniform darkening is indistinguishable from a lower exposure — the one failure the R10 gate names. It goes to zero when R3c's lamps and R9's bounce compute that light instead of assuming it |
| D328 | **Debug view 17: sky visibility on its own** | D296 | A shaded picture cannot tell a darker AO from a lower exposure, so "the room got darker" is not evidence that anything was measured. Grey is the fraction of the hemisphere reaching sky, magenta a pixel with no face, blue one not yet sampled. It is what the gate below is read from |

**Measured against a same-commit control**, built by stashing the change out — the committed
baselines are dozens of commits old and a move against those would prove nothing about this.

*The gate.* Enclosed mean sky visibility **1.00 → 0.019**; outdoor keeps a large population at open
sky — **656 pixels at exactly 1.00 and 2,039 in 0.9–1.0**, grading down through the colonnade. Both
halves matter: a change that darkens everything is indistinguishable from one that darkens nothing
but the exposure.

*The cost*, deck realtime, mean ms, control → R10a:

| view | control | R10a | |
|---|---|---|---|
| enclosed | 2.809 | 3.430 | +22% |
| outdoor | 1.752 | 1.981 | +13% |
| close | 3.211 | 4.539 | **+41%** |
| mid | 1.127 | 1.169 | +3.7% |
| far | 1.383 | 1.435 | +3.8% |
| distant | 1.407 | 1.416 | +0.6% |
| sky | 1.283 | 1.274 | −0.7% |

**The plan predicted the enclosed room would be the *cheapest* case here — "an AO ray dies at the
first thing it meets" — and that is not what happened.** It is the second dearest. The prediction
was about ray *length* and the cost is dominated by ray *count*: before this, a face pointing away
from the sun returned without tracing anything, which is about half of every building, and now every
face traces one ray whatever it faces. Enclosed is where faces are densest. The `faces` pass goes
0.50 → 1.26 ms there and becomes the dominant pass for the first time.

Speckle rises with it — enclosed 3.8 → 5.2, close 19.3 → 29.8 — because AO is a second Monte Carlo
estimate per face and starts un-converged. **R5 (face denoise) is what that stage is for**, and this
is the second measurement telling it how much work it has.

| # | Decision | Source | Notes |
|---|---|---|---|
| D329 | **R10a on its own is not ambient occlusion in any room, and the claim that it was is withdrawn** | user report | D325 reported the gate met — enclosed mean sky visibility 1.00 → 0.019 — and said the occlusion "spends itself on contrast". The first half is true and the second is false, and the mean is what hid it: **1,619 of 1,671 enclosed surface pixels fall in the lowest tenth**. An unbounded ray indoors always hits something, so the far field saturates at nought on every surface in the room, every one of them gets the identical `kIndirectFloor`, and the whole interior shifts by a constant — which is a change of exposure, not of shape, and is the exact failure the stage's own gate names. It was reported by the player as *"I don't see any ambient occlusion"*, correctly. **A mean is not evidence of variation, and this stage is about variation** |
| D330 | **The near field is the term that carries shape, and it is the same ray** | measurement | A ray that dies 5 cm away in the corner of an alcove and one that crosses the room before hitting the far wall are both "not sky" and are not remotely the same occlusion. So the ray's **first hit distance** goes through a falloff over a metre and accumulates beside the far field: `[0]` sky visibility as counts, `[1]` the contact sum in fixed point, halving together so the fraction cannot drift against the count it is divided by. Enclosed near field spreads across the whole range — 45,310 pixels fully open, 42,686 in 0.6–0.7, 12,342 in 0.4–0.5, 60 fully occluded — where the far field had everything in one bucket. In **metres and not voxels**, so a coarse face at 200 m and a level-0 face at arm's length darken over the same physical distance. It costs 3.430 → **3.455 ms** enclosed on top of R10a: one ray, two answers |
| D331 | **The far field multiplies the sky, the near field multiplies both, and each appears once** | — | `kSkyAmbient * mix(kIndirectFloor, 1, open_sky) * unoccluded + kGroundBounce * unoccluded`. The dome term says how much sky is there to be seen; the contact term says how much of what is left is shut out by whatever is within a metre — and the ground bounce is occluded too, because a crease does not receive bounced light either. Applying either twice double-darkens every crease, and that failure looks like a taste problem rather than a bug |

**Not yet verified: the self-occlusion gate.** The stage requires the contact term alone to read
white everywhere on a flat wall in the open, because a falloff that darkens a plane against itself
is the classic form of this bug and is invisible once the two terms are multiplied. The band
measured read 2,080 of 6,459 pixels fully unoccluded — but that band contains the portico's columns
and their recesses, where occlusion is correct, so it is **not a clean test of a flat wall** and the
gate should be treated as open. Debug view 18 shows the term on its own, which is what to read it
from.

| # | Decision | Source | Notes |
|---|---|---|---|
| D332 | **The face pass bounds its own writes, because a stride the host and the shader agree on by comment is not an agreement** | `--validation` | R10b doubled the face light to two words a slot. Run against a binary where the host had allocated one — a stale build, not the committed source — the pass wrote past the end of its buffer and into whatever the allocator had placed next, which was the node pool. It surfaced as the **node** mirror disagreeing in `nodes`, `leaves` and `occupancy` at scattered offsets: three buffers this pass has no business touching, reported by the one check that would notice, with nothing anywhere pointing at the face light. Three runs in three, and clean on the parent commit, so it bisected straight to a change that had not caused it. The index is bounded against `words.length()` now: a stride mismatch is a write that does not happen instead of a corruption somewhere else in the frame. **The audit earned its keep again** — this is the fifth stale-byte fault it has named, and the first one from a shader writing outside its own allocation |

| # | Decision | Source | Notes |
|---|---|---|---|
| D333 | **The self-occlusion gate, closed properly** | measurement | D329 left it open because the band measured contained the portico's columns, where occlusion is correct, so it was not a test of a flat plane. There is a rigorous version that needs no new render and no hand-picked region: **a face whose sky visibility is ≥ 0.95 demonstrably has nothing above it**, so its contact term must read unoccluded. Over 8,481 such pixels the mean contact is **0.9966**, with 6,053 at exactly 1.00 and two below 0.8. A falloff that darkened a plane against itself would show here and does not |
| D334 | **R10c: the gradient over the face, from moments the samples already carry** | — | AO was one number per face however close you stand, so it got blockier the nearer you got — the failure D298 fixed for shadows, arriving again through a different door. The face pass already chooses a point on the face and was throwing its position away. Keeping the **first moments** in the face's two axes turns the record into a continuous field, and there is no fitting step at all: the jitter is uniform over the sampled span, so the Legendre basis on [−1,1] is already orthogonal under it and each coefficient is an independent running mean of the sample weighted by a polynomial. **No rays, no passes, no least squares** — cost 4.539 → **4.455 ms** on the close camera, which is unchanged within noise. Measured on a terrace patch, adjacent pixels holding an identical value fall from **48.7% to 23.2%**: the field varies across a face where it used to be constant within one. The composite reads it for two multiply-adds from the record it was already fetching |
| D335 | **The gradient's halving is arithmetic, and this is not a detail** | — | The near-field moments are signed — a corner's gradient is negative on one side of the face — and they halve with the sample count at `kFaceWindow` like everything else. A logical shift there folds a small negative into a very large positive exactly once every 256 samples, which reads as a face flashing bright at a regular interval and nothing in the picture says why. The scalar terms beside them are unsigned and shift the other way, in the same block, which is precisely how this gets written wrong |

| # | Decision | Source | Notes |
|---|---|---|---|
| D336 | **The quadratic terms were built, measured, and taken out again** | measurement | §8 R10c calls for six terms and says the quadratics are what make a voxel cylinder read as a cylinder rather than as a faceted prism. Built — `uw`, `P₂(u)`, `P₂(w)`, seven words a slot, the same running-mean-of-a-polynomial as the linear ones — the picture moved by a mean of **2.14/255**, and the columns the prediction was about moved by **2.24/255**, which is the same as everywhere else rather than concentrated on them. Then the number that settled it: **two runs of the same build differ by 2.50/255**. The quadratics change the picture by less than the renderer's own run-to-run noise. Reverted |
| D337 | **Why the prediction failed, which is worth more than the terms would have been** | — | The quadratics describe how occlusion varies *within one face*, and since D298 a face **is a voxel** — 3.125 cm. A voxel on a column is flat; there is no curvature inside it to fit. Curvature on a voxel cylinder lives **across** faces, not within one, so the term was fitting a quantity that does not exist at the scale it was stored at. The plan's intuition was written when a face was a brick, eight voxels across, where it would have had something to say. What is definitely paid for it is memory: **16.9 MB → 29.6 MB** of face light, and seven words read and written per shaded face instead of four. The timing difference between the two builds fell inside run-to-run timing noise, so no cost claim is made from it |

**R10 is done as far as it earns its keep.** The far field, the near field and the linear fit are in.
The quadratics stay written down here rather than in the code: if a face ever spans more than one
voxel again — a coarse face at distance is exactly that — they become worth re-testing, and the
accumulation is four lines beside the ones already there.

## The renderer rewrite — a shadow that outlives what cast it

| # | Decision | Source | Notes |
|---|---|---|---|
| D338 | **Deleting geometry does not clear its shadow, and the cause is D302's rule meeting an edit** | user report, measurement | Reported as *"shadows still don't react when I delete a part of the facility"*. Reproduced with `--edit "-600,96,-600,600,640,600,0" --edit-frame 300` on the close camera — everything above three metres removed, **36,163,624 voxels**, solid count 127.2M → 91.0M, and 39% of the picture changes, so the renderer does react. The **terrace under the deleted roof does not**: sun visibility reads **0.0056 at a hundred frames after the edit and 0.0045 at six hundred**. Not slow — stale. Against the same frame built with unbuilt cells not blocking, it reads **0.3608**. So what casts the shadow is not matter; it is cells the pool has not built, which occlusion treats as opaque (D302, D324) |
| D339 | **The instrument: a face records whether its sun ray stopped on matter or on ignorance** | — | Both are `hit` for occlusion, and in a picture they are the same shadow. `NodeHit` now carries `unknown`, the face pass keeps it in bit 31 beside the step count, and the audit reports it. **Control: 18,789 of 105,931 fully shadowed faces are shadowed by a cell the pool has not built. After the edit: 54,941 of 94,576** — so after an edit the *majority* of the shadow in the frame is cast by ignorance rather than by geometry. That number is what any fix has to move, and without it "the room is dark" and "the room is dark for a reason that no longer exists" are one observation |
| D340 | **An attempted fix that did not work, recorded so it is not tried twice** | measurement | An edited brick is wanted by construction, so the dirty pass was made to re-request every invalidated brick the world still has matter in. It changed nothing: 0.0056 and 0.0045, to the digit. The pool loses about **6,100 bricks** to this edit — level 3 goes 13,651 → 7,555 — and they do not come back, but re-requesting them at the point of invalidation is not what brings them back either. The budget is 16,384 builds a frame and the request is queued before the serving loop, so the obvious explanation is ruled out and the real one is not yet known. Reverted |

| # | Decision | Source | Notes |
|---|---|---|---|
| D341 | **A shadow ray reports the one cell that stopped it, and R9i's second half is that line** | measurement | The debug view settled it: on the close camera **the same step reads fully lit on its left and fully shadowed on its right**, with nothing between them to cast anything. The boundary is the edge of what the camera has caused to be built. The mechanism was already all but present — the WANTED branch calls `node_note` — but a shadow ray passes `report = false`, so the note was recorded and thrown away. It now flushes regardless. This is the narrowing D292 always needed: that rule forbids a shadow ray from dragging residency towards everything it *crosses*, and this is the single cell it was **stopped by**. **Faces shadowed by a cell the pool has not built: 18,820 → 0** on the static camera, and the pool builds what it actually needs — bricks 13,651 → 41,814 |
| D342 | **What it costs, and one number that moved the wrong way** | measurement | Deck realtime: outdoor 1.981 → 2.060 (+4.0%), mid 1.169 → 1.264 (+8.1%), everything else inside noise. But **speckle: enclosed 5.2 → 27.4, close 29.8 → 60.3**. The picture is better rather than worse — the interior reads as a room, with pilasters, alcoves and floor pattern legible where it was uniformly black — and that is exactly why the figure rose: a surface that was flat black has no high-frequency content to measure. It is the same flattery this metric gave in D329, where 8.2 was a blur and 21.7 was the blur removed. **The figure is recorded rather than explained away**: some of the rise is genuine per-face sampling noise on shadows that now have structure, and R5 is the stage that owes it |
| D343 | **The edited case is improved and not fixed** | measurement | After deleting 36M voxels the count goes 54,933 → 50,967 at frame 900 and **51,326 at frame 3,000** — it does not converge. Feedback sits at **124,621 reports of a 131,072 capacity**, so the requests are being made; the pool is not building them, and its brick count is flat at ~18,000. Requests that build nothing, repeated for ever, is the D133 phantom signature exactly: something claims occupancy that the world no longer has. Two places to look, both cheap: `occupied_` is rebuilt only when the chunk COUNT changes, so an edit that empties part of a chunk without removing it leaves the level-8-and-above answer stale; and the dirty pass's chain walk stops at the first unbuilt node, so ancestors below one are never refreshed. Also new after the edit: **7,870 faces lit only by a ray that ran out of steps**, up from 0 — with the roof gone the rays travel far enough to reach the 512-step cap, which fails towards light and is benign, but it is a second thing the edit changed |

| # | Decision | Source | Notes |
|---|---|---|---|
| D344 | **An edit leaves the pool asking for a million nodes a frame that cannot be built, and it predates the shadow-ray report** | measurement | The counters, after deleting 36M voxels and settling six hundred frames: **requests 634,769,296, hits 13,175,913, built 15,191**, against a control's 20,118,014 / 749,667 / 35. About a million requests a frame, almost none of which build anything. That is D133's phantom — a descent answering WANTED over a region the world no longer has anything in, so the request is served, finds nothing, and is made again next frame for ever. It is also what the 51,326 faces still shadowed by ignorance are standing on: the same cells. **The cost is pre-existing**: the same edit measured **10.6 ms of node-pool CPU a frame before D341 and 13.1 ms after**, so the shadow-ray report adds about a fifth of it and the other four fifths were there already, unnoticed, because nothing had counted requests against builds after an edit |
| D345 | **The stale chunk index is NOT the cause, tested and reverted** | measurement | `occupied_` answers `world_has` at level 8 and above and is rebuilt only when the chunk COUNT changes, so an edit that empties a chunk without removing it should leave it claiming occupancy for ever. Re-indexing on every edit frame is two lines and it moved the count by **42 faces of 50,967** — nothing. Reverted rather than kept: it is a plausible latent fault, it is not this one, and shipping it with a rationale that does not hold is how a decision log stops being worth reading. What remains of the original three leads is the dirty pass's chain walk, which stops at the first unbuilt node and so never refreshes the ancestors below one |

| # | Decision | Source | Notes |
|---|---|---|---|
| D346 | **The served requests are for cells `world_has` says are empty — and refusing them makes it worse, which is the finding** | measurement | Logging the first served requests after an edit: **`request 0,8,-80 level 3: world_has 0, found 0`**, and its six face neighbours, all with `world_has` false, all re-requested every frame for ever. That looks conclusive — until the obvious fix is tried. Refusing a request the world says is empty is one line, and it **starves the tree**: leaves 17,461 → 7,625 after the edit, ignorance 50,967 → 57,366, and on the control, which had been at zero, 0 → 2,696 with fully-lit faces 14,531 → 6,746. A guard that only skipped genuinely empty cells could not do that. **So the reading flips: `world_has` returning false is not evidence the cell is empty, it is evidence `world_has` is wrong about it** — and every child mask in the tree is derived from that same function, which is where a descent's WANTED over empty world would come from. Reverted |
| D347 | **Two dead ends recorded in one place, because the third would otherwise be tried next** | — | Of D343's three leads: the stale chunk index moved 42 faces of 50,967 (D345); refusing empty requests made every counter worse (D346); and re-requesting invalidated bricks changed nothing to the digit (D340). All three are reverted and none of them is the fault. What all three have in common is that each assumed `world_has` was the authority and asked how its answer was being *used*. The measurement above says the answer itself is the thing to check, at a level where it can be tested directly — `world_has` against a walk of the actual voxels, over a region an edit has just emptied, in a headless test with no renderer in the way |

| # | Decision | Source | Notes |
|---|---|---|---|
| D348 | **The root cause: `world_has` asks whether a brick is allocated, not whether it holds anything** | headless test | Found by asking the pool directly instead of the renderer. `tests/test_node_pool.cpp`, *a region emptied by an edit stops being wanted*: fill a box, build it, empty the top half, invalidate exactly as the edit path does, serve one frame — and the emptied region still answers **wanted** at level 3 while level 4 is still there claiming it. The line is `chunk->brick(...) != nullptr`. **A brick is not freed when its last voxel goes**, so that tests whether somebody once put matter there and keeps answering yes for ever. Every child mask in the tree is derived from it, which is why the descent says WANTED over open air, why occlusion — which reads WANTED as opaque — keeps a shadow after its caster is deleted, and why the request is unservable: the pool is asked for the cell, finds nothing to build, and is asked again. `!= nullptr && !brick->empty()` makes the test pass at every level |
| D349 | **And the fix costs more than the bug, so it is not in** | measurement | With the answer corrected, **500 frames of the edited camera did not finish in seven minutes** — about one frame a second, reported by the player before the measurement finished. The old answer was fast *because* it was wrong: a gathering ray crossing an emptied region used to stop at the first unbuilt cell, and with the region correctly empty it marches on, through the hole to the far plane, 512 steps a ray, for every face in the store. The resident tree grows **4.5×** with it — 41,882 leaves to 187,377 — because the pool then builds what those longer rays ask for. Reverted, and the test is left in the file **skipped**, with the reason on it: it is the gate for the work that unblocks it, not a failure to be silenced |

| # | Decision | Source | Notes |
|---|---|---|---|
| D350 | **Bounding the gathering rays works, and moves the wall to the CPU** | measurement | A `max_distance` on `node_march` — sixty metres for a shadow or ambient ray, the far plane for a primary one — took the corrected-`world_has` build from *not finishing 500 frames in seven minutes* to finishing them at **8.55 ms of GPU**. So the ray bound is right and cheap and should land on its own merits. But the same run reads **711 ms of node-pool CPU a frame, worst 1,508**, against 13.8 reverted: the frame rate the player saw was never the GPU. Both reverted together, because the second is what makes the pair unusable |
| D351 | **The CPU wall is the request-serving loop, not the builds** | measurement | `max_builds_per_frame` is 16,384 and is not the limit being hit; what costs is that every request is *walked* whether or not it has anything to do, at about a million a frame. Two things feed that volume and both are addressable without touching correctness: **the shadow-ray report of D341 is dilated to its six face neighbours** by `stream()`, exactly as a miss report is — but a miss report is a guess about where geometry might be and a shadow-ray report is the precise cell that stopped the ray, so the dilation is pure waste there, a factor of seven. And a request whose node is already built pays a full descent to discover it. Either alone may be enough; the counter to watch is `requests` against `hits`, which currently reads 634,769,296 to 13,175,913 |

| # | Decision | Source | Notes |
|---|---|---|---|
| D352 | **A report that is exact says so, and is not dilated** | measurement | Every accepted node report was grown to its six face neighbours, because a *miss* report is a guess about where geometry might be and only the cells some ray happened to land on would otherwise be built — which left notches along the edges of what had streamed. A shadow ray's report is not a guess: it is the one cell that stopped the ray. `kFeedbackExact` marks it and the consumer requests it alone. **Requests after an edit: 634,769,296 → 296,368,098**, node-pool CPU 13.8 → **11.5 ms**, and on the control 0.364 → **0.109 ms** with the tree 11,000 leaves smaller because the dilation had been building neighbours nobody asked for. D341's result is untouched — the control still reads **0 faces shadowed by a cell the pool has not built** — and `--validation` is clean with both mirrors matching |

| # | Decision | Source | Notes |
|---|---|---|---|
| D353 | **Thinning the shadow reports does nothing, so they are not the bulk** | measurement | D351 named two feeders of the request volume and D352 fixed the first. The obvious follow-up — report one cell in four a frame, keyed on the cell so every one gets its turn, on the argument that duplicates dominate — moves **CPU 11.543 → 11.589 ms and requests 296,368,098 → 294,318,340**, which is nothing. The feedback buffer is still at **124,297 entries of a 131,072 capacity** with those reports thinned to a quarter, so they were never what filled it. Reverted; it would have cost convergence for no return. **What is left to account for**: 124,297 entries a frame becoming about 1.18 million requests is roughly nine each, and the exact reports are now one each — so the remaining multiplier is the miss reports' dilation and the **proximity sweep**, which pushes bricks within twenty metres of the camera every frame whether or not anything has changed. That sweep is the next thing to count, and it has nothing to do with edits, which is consistent with the control's request figure being high too |

| # | Decision | Source | Notes |
|---|---|---|---|
| D354 | **D352 does not make D348 affordable either, and the correctness fix is now parked rather than iterated on** | measurement | With the request volume halved (D352) and the gathering rays bounded (D350), the corrected `world_has` **still does not finish 500 frames in five minutes**. So the cost is not the dilation, not the shadow-ray report count, and not the ray length — three feeders measured and eliminated — and the remaining explanation is the one D349 already gave: correcting the answer grows the resident tree about 4.5× and the pool pays for it every frame. **That is a residency-policy problem, not a bug to be squeezed out**, and it wants the sub-pixel rule R2b left half-done (a node finer than the pixel is never *requested* but may still be *stored*) rather than another pass at the request path |
| D355 | **Three 1 fps builds on the player's machine is a process failure, not a measurement** | user report | Each of the three attempts at D348 was compiled, run, and handed over before its own timing had been read; each time the report *"1 fps"* arrived while the measurement was still running. The engine has a `--settle` screenshot harness precisely so a change can be timed without anyone playing it, and it takes about four minutes. **A change that alters how far a ray travels or how much the pool builds is timed on the harness before it is built into anything anyone opens.** Written down because the failure was repeated, which makes it a habit rather than an accident |

| # | Decision | Source | Notes |
|---|---|---|---|
| D356 | **The gathering-ray bound is neutral on the build that exists, so it is not in it** | measurement | D350 measured it against the corrected `world_has` and it was the difference between not finishing and 8.55 ms — but that configuration is parked (D354). Measured on its own, against a same-commit control on the seven-camera grid: **nothing moved over 3% in either direction**, and the picture moved by 0.15 to 2.29 of 765 on three cameras against a run-to-run noise floor of about 7.5. It earns nothing today because rays already stop early on the unbuilt cells the bug leaves lying about — the bound only pays once those stop being there. It also caps shadow reach at sixty metres, which is a real restriction in a world meant to be large and would go unnoticed until someone built something big. **Parked with D348**, to land in the same change or not at all, on the same principle that removed the quadratic AO terms (D336): a change that cannot be measured is not carried |

## The renderer rewrite — one root cause behind three symptoms, and five failed fixes

**CLOSED by D357–D358 below.** The account is kept because the five dead ends in it are worth more
than the fix was: four of them assumed `world_has` was the authority and asked how its answer was
being *used*, and the sixth attempt — the one that asked whether the answer itself was true — took
one headless test. What the section gets wrong at the end is which half of the fix was expensive:
it says the reader has to change, and the reader never changed at all.

**The three symptoms are one fault.** The player reported them separately over several sessions:

1. delete part of the building and **its shadow stays**, for ever;
2. pull away from the deleted region and **it fades back in, entirely black**;
3. standing still, **bricks flicker to simpler geometry, often plain cubes**.

All three follow from one line. `NodePool::world_has` asks `chunk->brick(...) != nullptr`, which
tests whether a brick is **allocated**, and a brick is not freed when its last voxel goes. So an
emptied region answers "there is matter here" for ever. Every child mask in the render tree is
derived from that answer, and from there: the descent reports the cell as unbuilt-but-occupied,
which occlusion treats as opaque (D302, D324) — **symptom 1**; the ancestor above it folds a colour
from children that *have* been freed, giving nought, and draws that at any distance where a pixel
resolves the coarse level — **symptom 2**; and it appears and disappears as the level dither
crosses a boundary — **symptom 3**.

**The reproduction is headless** and in the tree: `tests/test_node_pool.cpp`, *a region emptied by
an edit stops being wanted*, currently **skipped** because it fails. Un-skip it as the gate.

| Attempt | Result |
|---|---|
| Re-request invalidated bricks (D340) | No change, to the digit |
| Re-index `occupied_` on every edit (D345) | Moved 42 faces of 50,967 |
| Refuse requests the world says are empty (D346) | **Worse**: starved the tree, control ignorance 0 → 2,696 |
| `world_has` tests emptiness (D348, D349) | **Correct** — ignorance 50,967 → 3,079, lit faces 13,575 → 41,878 — but 726 ms of CPU a frame after a deletion, because `!= nullptr` stopped at the first allocated brick while `!empty()` must scan past every emptied one. Free on an unedited world: 0.140 ms |
| Free the emptied bricks, so `!= nullptr` is both correct and cheap | The right shape and **not yet working**. `Chunk::compact()` is unusable on an edit frame — it walks the whole chunk and re-encodes every brick. A targeted `drop_brick_if_empty` over only the bricks the edit touched was written and also came out slow; it was reverted before its own measurement landed |

**What is almost certainly the answer**, and what the next session should do: free an emptied brick
**at the point the last voxel is cleared**, inside `Chunk::set`, rather than sweeping for empties
afterwards. `set` already finds the brick and already knows the write emptied it; the free is then
O(1) per edit and no scan of any kind is added. `Chunk::prune` shows exactly how to unlink one
(free the brick, clear the child slot, decrement `brick_count_`), and the only care needed is that
the parent nodes above it are unlinked too when they lose their last child, which `prune` also
shows. With that, `world_has` needs no change at all and all three symptoms go together.

**Do not measure this on the machine the game is being played on.** Five separate builds in this
investigation reached the player at about one frame a second, every time because the change was
handed over before its own timing had been read. `--max-seconds` now exists for exactly this: it
gives a scripted run a wall-clock deadline so a slow build reports itself instead of hanging.
Rebuild the last good commit before saying anything, because the binary on disk is the one being
played.

**What has to land first.** Bounding what a gathering ray may cost, which is owed anyway. R10b's
near-field falloff already gives the ambient ray a natural stop at about a metre and is currently
not used to end the march; a shadow ray wants a step budget of its own. Both are small, both are
measurable on their own, and together they turn D348 from a one-line change that costs the frame
rate into a one-line change that does not.

**Where to start next.** Superseded in part by D341, which did the first of these. The remainder: The counter in D339
is how each is judged, not the picture. **(1)** Find why the re-requested bricks are not rebuilt —
`world_has` may be answering false for them, in which case the parent's mask is cleared and the cell
should read EMPTY rather than WANTED, and the fault is upstream of occlusion entirely. **(2)** A
WANTED cell whose parent has a folded coverage should occlude *by that coverage* rather than
absolutely; the parent was just re-folded by the same dirty pass, so the number is to hand. **(3)**
Failing both, an edit could mark its region as "known thin" for a bounded number of frames, which is
a rule rather than a mechanism and should be the last resort, not the first.

## The renderer rewrite — the shadow that outlived its caster, closed

| # | Decision | Source | Notes |
|---|---|---|---|
| D357 | **The brick is freed where it empties, so the reader never had to change** | measurement | D348 found the fault and D349 priced the obvious fix at 726 ms of CPU a frame: `!= nullptr` stops at the first allocated brick and `!brick->empty()` must scan past every emptied one. So the reader is left exactly as it was and the *world* is made honest instead. `Chunk::set` already has the brick in hand and already knows the write emptied it, so it unlinks it there — one descent that was being walked anyway, an eight-word emptiness test, and the ancestors that lose their last child go with it. `Chunk::drop_brick_if_empty` is the same thing for the bulk writers in `op.cpp` that fill or assign a whole brick and that the chunk therefore cannot see. **O(1) per emptied brick, no scan of anything.** `compact()` stays, for the clip paths that still go around `set`, and is no longer on any edit path. The headless gate — *a region emptied by an edit stops being wanted* — is **un-skipped and passing** |
| D358 | **And the chunk goes with its last brick, which turns out to be most of the fault** | measurement | D357 alone was **not enough and read as 1 fps**: 1,042 ms of node-pool CPU a frame after the 36-million-voxel delete, with the GPU at 10.3. `world_has` answers level 8 and above out of `occupied_`, which is rebuilt only when the *set* of chunks changes — so about thirty chunks the edit had emptied went on claiming occupancy at every level above the chunk. The descent stops at a coarse node, reads a mask bit set over nothing, reports WANTED, is served, finds nothing to build, and reports again next frame for ever: D344's phantom exactly, now the only one left. `World::drop_chunk_if_empty` in `apply_op` is the counterpart of D357 one level up, and it is affordable only *because* of D357 — `empty()` is a counter now. **D345 tested this same idea and measured 42 faces of 50,967**, because at the time the answers below level 8 were wrong too and fixing the coarse half alone changed nothing. Two halves of one fault, and neither is worth anything without the other |
| D359 | **The numbers, against a same-commit control** | measurement | Close camera, 1280×800, quality 7, `--settle`, the D338 edit (everything above three metres, 36,163,624 voxels) at frame 400, read 260 frames later. Control is the same commit with both changes stashed out. **Faces shadowed by a cell the pool has not built: 62,756 → 0.** Mean sun visibility on faces that face the sun **0.1222 → 0.6350** (D338's "correct" reference for the terrace was 0.3608, and the stale reading was 0.0056). **Node-pool CPU 11.426 ms → 0.336 ms**, total GPU **4.610 → 3.749 ms**, feedback **160,559 reports with 29,487 dropped → 24,153 with none dropped**, requests over the run **334.4 M → 19.0 M**. The run itself went from not reaching its screenshot frame inside 120 s to finishing in **9.0 s**. On an **unedited** world it is free: the seven-camera grid at all three resolutions moved nothing over 3% except two cells at −4.4% and −4.5%, both on views whose worst-frame spread is larger than that |
| D360 | **A side effect worth naming: the world's content hash was wrong for an edited world** | — | `World::content_hash` skips empty chunks so that a world built here and the same world read back from the clip cache agree — and `Chunk::empty()` was `brick_count_ == 0`, which an emptied chunk failed. So a peer that carved a region and a peer that loaded the result computed different hashes for identical worlds, which is what `06-multiplayer.md` §4 reconciles on. Visible in the two runs above: same 91,034,757 solid voxels, hashes `d6ee9121…` and `e8006ac2…`. There is a test on it now |
| D361 | **The gathering-ray bound, measured a third time, is still not carried** | measurement | D350 measured it as the difference between not finishing and 8.55 ms, and D356 parked it as neutral "to land in the same change as D348 or not at all". It was built again on top of D357 and D358 and measured again: GPU **3.749 against 3.754** with the bound off, mean sun visibility **0.6350 against 0.6349**, node-pool CPU nil either way. The reason it does nothing is now clear and is worth keeping: **the 512-step cap binds long before sixty metres does**, because a ray near the camera marches at single-voxel granularity and spends its whole budget inside twenty. A distance bound on a step-bounded ray is not a bound. Reverted, and the parking is over — D350's result belonged to a build where the CPU, not the ray, was the wall |

| # | Decision | Source | Notes |
|---|---|---|---|
| D362 | **Every scripted run now ends on the clock, without being asked** | user request | `--max-seconds` existed (D355) and had to be remembered, which meant it was passed exactly when somebody already suspected a problem — never on the run that turned out to have one. It now defaults to **180 s** for any run that ends by itself (`--screenshot`, `--ticks`, `--stream-frames`, `--benchmark`), and `--max-seconds 0` is how to say no deadline. Three minutes because a cold clip cache is 133 s of sampling on its own (D241) and a settled run of any camera on the grid is under ten, so a run that reaches the deadline is either cold — in which case its figures were never comparable — or slow, which is the finding. Two holes closed with it: the deadline was only tested inside the screenshot branch, so a `--settle` run waited on **kSettleGiveUp, which is thirty thousand FRAMES** — eight hours at the one frame a second a bad build runs at — and the two headless audits ended on their own counters. All three read one clock now. Prompted by the fourth 1 fps hand-over of this investigation, and this time the run was killed by the person watching it rather than by the harness, which is the same failure D355 records |

## The tools — what a preview says, and two keys that were not saying it

| # | Decision | Source | Notes |
|---|---|---|---|
| D363 | **The cursor marker is a ring on each face of the voxel, and the constraint point is a cross** | user request | Both were cubes before: the voxel under the crosshair was drawn as a one-voxel preview box, and a dropped constraint point as a filled cell washed in the inverse of the backdrop. Neither said what it was. A filled cell is indistinguishable from a one-voxel preview *box* — one is where you are pointing and the other is what is about to happen — and an inverted backdrop is a different colour on every surface it lands on, so eight marks across a wall did not read as eight of anything. Now: a hollow ring for the cursor, an X for a constraint point (the key that drops it), both in the material's own colour, both **hollow** so the surface being marked is still visible through the middle of its own marker |
| D364 | **On the faces, in world space, bounded by the voxel — three things that were each tried the other way first** | user report | A billboarded shape facing the camera was built and rejected: it stays square-on from every angle, which reads as a screen overlay hovering near the voxel rather than a mark on it. A **minimum size in pixels** was built and rejected for the same reason and a worse one — at range it covers voxels it does not mean, and a marker that lies about which voxel it means is worse than one you cannot see. And an inscribed *sphere* for the cursor was built and rejected: it is one shape for a cube, where a ring on each face says which way the voxel is turned. What is measured in pixels is line WIDTH only, which is a different quantity: a line thinner than a sample flickers, and thickening it changes nothing about what the shape encloses. Faces seen closer than about 78° to edge-on are skipped, because a circle drawn on a face seen edge-on flattens into a line along the silhouette — three of those give the marker a partial box around it, which is exactly the "outline" that was reported after the first attempt |
| D365 | **Preview boxes fill their faces, and the polarity is the rule the outline already used** | user request | An outline says where a box is; the faces say which way it is turned, and a wireframe seen straight on is ambiguous about that. Every face the ray crosses is tinted, so the colour deepens where the box is thick, which is a depth cue for nothing. Placing takes the material's own colour where it can be seen and the inverse where it is buried; **carving is the other way round** — inverted in the open, and the colour of what is about to be removed where it is buried, so the shape inside the rock reads as the rock that is going to leave. The two halves of one box are then never the same shade and a carve is never mistaken for a placement. A clipboard *selection* fills at a quarter; the ghost it becomes does not fill at all, because a coloured pane over the thing you are lining up is the one thing a paste preview must not do |
| D366 | **A preview drawn exactly on a surface z-fights, and the fix is one biased comparison** | user report | The markers sit on the faces of a solid voxel, so the preview's distance and the world's distance to that face are the same number arrived at by two different pieces of arithmetic. Half the pixels of a ring then decided "in front" and half decided "behind", and the shape broke into crawling speckle — the classic z-fight, reached through a marcher rather than a depth buffer. `preview_behind` is now the single comparison every preview makes, biased by a thousandth of the distance: five millimetres at five metres, well inside a voxel and well outside the noise. Also fixed with it: the ring's radius was set to the half-width of the face, which puts half the line's thickness past the edge, so the circle read as slightly larger than the voxel it was marking |
| D367 | **Cost, on the worst case that exists** | measurement | The preview runs per pixel in `resolve.comp`, so the case to measure is a box filling the screen with the full complement of marks in it. Close camera, 1280×800, three runs each: **0.740–0.741 ms with no preview at all, 0.775–0.777 with a screen-filling box and eight marks** — 4.9%, inside the 0.80 ms budget. Two figures were thrown away getting there, both single samples taken while a previous run was still shutting down and holding the GPU: one read 1.013 and one read 2.182, and a third config that should have been identical to a known-good baseline read 1.407 against 0.700, which is what gave it away. **Trap 9 has a corollary: serialise the runs.** The harness now waits for the previous process to exit |
| D368 | **`--preview` state 6 and `--preview-mark`, because a shape nobody can photograph is a shape nobody notices has stopped being drawn** | — | The cursor marker and the constraint crosses had no scripted path onto the screen: both need a hand on the mouse or the keyboard, which made them the only preview elements that could not be checked from a screenshot. `--preview x0,..,z1,6` draws the cursor marker at a chosen voxel and `--preview-mark x,y,z` drops a cross, repeatable. Everything above was verified through them |

| # | Decision | Source | Notes |
|---|---|---|---|
| D369 | **Q and E were not broken: the palette had one entry in it** | user report, measurement | Reported as "I cannot change materials with E or Q". The keys, their mapping, their handlers and their uniqueness all checked out on paper. One log line settled it: **`palette: 1 materials from the cache`**. `materials_` is taken from the cached world, the cache on disk was written before that field existed, it reads back empty, and the fallback is `push_back(1)` — so E and Q cycled a list with nowhere to go. The palette now comes from the **script**, which is what declares the materials and has been parsed by that point regardless because the cache key is hashed from it: **550 materials** for the facility. That fixes every stale cache in existence rather than the next one written, and the count is logged at load, because a palette of one is indistinguishable from a key that does nothing and the two have different fixes |
| D370 | **Undo changed the world and never told the renderer** | user report, measurement | Reported twice as "Ctrl+Z does nothing", and both times the world went back correctly. An edit takes two steps — refresh the coarse grids, then tell the renderer which region changed — and undo took only the first. The node pool is what the game marches (D224) and nothing had told it, so the world returned and the picture did not. It could not be caught by any test in the suite, because every test asks the World what it holds and the fault was entirely in what the renderer had been told: **the seam D225 describes, reached through the one edit path that was not carrying its ops with it.** `undo`/`redo` now report the ops they applied — a bool return was what made it impossible to do the right thing at the call site — and go through the same two steps a chisel stroke does |
| D371 | **X was bound to redo as well as to dropping a constraint point** | — | Found while reading D370's neighbourhood. `redo_down` was `is_down(X) \|\| is_down(Y) \|\| (ctrl && is_down(Y))`, and X is the constraint-point key (`chisel.hpp`). So every point dropped also redid a step of history — silent when there was nothing to redo, and when there *was* something it put back an edit the player had deliberately undone. One key with two meanings, where the meaning nobody asked for only fires in the case where it does harm. Redo is Y or Ctrl+Y now |
| D372 | **`--undo-frame`, and what it immediately found** | measurement | Undo was reported broken twice, so it is now scriptable on the same code path the key takes rather than a second implementation beside it. Edit, undo, screenshot: **the world comes back to the same content hash as a run that never edited at all** (`766f2fd6…`, 74 chunks, 127,198,381 solid voxels, against the edited `e8006ac2…`). The *picture* does not, and that is the next thing to find: 400 frames after undoing a 36-million-voxel delete, geometry is fully restored and **16.6% of pixels still differ from the never-edited frame at a mean of 20.9/255** — against 45.3% and 72.7 for the edited frame, so most of it did come back. What is left is light: the terrace reads as full sun where the restored roof should be shadowing it, and there are lit slashes across the entablature. The relight window is not obviously to blame — `kEditShadowReach` is 512 voxels and covers the terrace — so the suspect is `kShadowRefreshFrames`, which is 120 and had closed 280 frames before the shot. **Pre-existing and not caused by the fix**: before it, undo did nothing at all, so there was nothing to be stale |

| # | Decision | Source | Notes |
|---|---|---|---|
| D373 | **A face is TOLD the world changed, rather than left to infer it, and D372's residual goes** | user report, measurement | Reported as *"the shadows do come back after redoing, just extremely slowly"*, which is what turned D372's open item into a diagnosis. `face_accumulate` throws a history away only when a sample contradicts a **unanimous** one (D319) — the right conservative test for a sample that might be noise, and **no test at all for a face that was mid-transition when the next edit landed**. Delete the roof and a terrace face starts climbing from black towards white; undo before it arrives and it is unanimous about nothing, so it has nothing to throw away and simply averages back down inside a 256-sample window. The signature is unmistakable once counted: **fully shadowed faces flat at ~42,000 across four hundred frames against the 105,848 the same camera has when it was never edited**, with the mean drifting 0.4427 → 0.3465 → 0.3113 rather than converging. The host does not have to infer any of this — it knows the world changed and knows exactly where. `edit_min.w` is 2 on the one frame an edited region opens, and faces inside the box drop their history outright. Exact information instead of a guess, and **D319's rule is untouched** for the case it was written for |
| D374 | **What it moved** | measurement | Close camera, undo of the 36-million-voxel delete, against a run that never edited (mean sun visibility 0.1304, 105,848 fully shadowed). Before: **0.4427 at undo+60 and still 0.3113 at undo+400**. After: **0.1278 at undo+20**, 0.1287 at +60, 0.1303 at +400. In pixels against the never-edited frame, where two never-edited runs of this camera differ by **1.493/255 and 1.42%** — that is the floor: before, undo+400 read 20.907 and 16.59%; after, undo+400 reads **1.916 and 2.81%**, undo+60 reads 2.447 and 4.42%. **Converged inside a second, and at the noise floor by four hundred frames.** GPU 4.701–4.718 ms never edited against 4.893–5.087 with the edit and the undo inside the window, which is the edit's own cost more than the reset's: the faces in the box were already exempt from the shading stride, so what was added is one store |

| # | Decision | Source | Notes |
|---|---|---|---|
| D375 | **Eight constraint points was a display limit that read as a tool limit, and it is sixty-four** | user report | Reported as "there seems to be a visual limit to how many extra point Xs there are", which is exactly what it was: `kMaxPreviewMarks` was 8 and the note beside it shrugged that "more than this can exist; they simply stop being marked, which is a display limit rather than a tool limit". That shrug is trap 7 in the interface — the ninth point was dropped, the box grew to reach it, and nothing said where it was, so the tool worked and looked broken. What made eight defensible was cost: each mark is tested against the ray per pixel. So the host now hands over **the box every live mark fits in**, one slab test rejects the whole loop for any pixel that cannot be looking at one, and the ceiling moves to sixty-four. Overflow past that is **logged** rather than silently dropped. Measured, grid close camera, three runs each: **0.697–0.707 ms with no preview, 0.736–0.739 with sixty-four marks and a screen-filling box**, against a 0.80 budget. At arm's length from a wall, where all sixty-four fill the frame, it is 0.775 against 0.822 — the one configuration that exceeds the budget, and it needs sixty-four points on screen at 0.75 m to reach it |
| D376 | **A face fades as it turns edge-on; it is not cut off at a threshold** | user report | D364 skipped faces closer than about 78° to edge-on, because a circle drawn on one flattens into a line along the silhouette and three of those give the marker an outline. Reported back: *"sometimes faces of the circle or Xs are invisible when they shouldn't be culled, especially when seen from their profile only slightly off."* Correct, and the fault is the *shape* of the rule rather than its value — a hard threshold makes a face vanish outright while it is still perfectly legible, so turning slowly past a marked voxel loses a face with a snap. What was actually wanted is that a shape too foreshortened to be a shape stops shouting, not that it stops existing. It is a `smoothstep(0.03, 0.30, …)` now: full strength by about 17° off the face, gone only within two degrees of exactly edge-on, where the face is a hairline and there is nothing left to draw. Verified at four yaws from square-on to strongly oblique; the cost did not move |

| # | Decision | Source | Notes |
|---|---|---|---|
| D377 | **A preview box asks "does this ray touch me" before it asks anything else, and an ordinary one is then free** | user request, measurement | Asked for outright: *"optimize these types of ui elements, they should be basically free."* `draw_one_box` ran its six-face loop first — reconstructing a point and running two range tests per face — so a pixel nowhere near the box paid for six of those before discovering it was nowhere near the box. The slab test was already being computed at the bottom for the volume wash; moving it to the top and returning early is the whole change. Grid close camera, three runs each: **0.714–0.730 ms with no preview, 0.704–0.714 with an ordinary chisel box and a constraint mark** — free, and below the baseline's own spread. The screen-filling box with sixty-four marks is 0.733–0.741, up 3%, and it is a case that cannot arise from the tool. **One trap on the way, and it is worth the entry**: the first version early-returned on depth as well as on the miss, which deleted the box from every pixel where it was buried — the exact case the tool exists for, and the thing the comment three lines above it warns about. A box is drawn THROUGH geometry on purpose; only the wash is depth-gated. It rendered as a preview that simply was not there |
| D378 | **O acts on the voxel you are looking at, by default, and it moves carving too** | user request | Two changes to one setting. It defaulted to placing against the face — the convention a block game teaches, where you point at a surface and the block lands on top of it. This is not that game: the crosshair is on a 3 cm voxel, the cursor marker now draws a ring around exactly the one it means, and having the tool act on a *different* voxel from the one being marked is a contradiction the player carries on every stroke. Off by default; O is there for when a placement genuinely has to go in front of a surface. And it now applies to **carving as well as placing**, which it never did — `resolve_point` consulted it for placing only, on the argument that "carving the air in front of it is not a thing anyone wants". True of the air and false of the toggle: a setting that says where the tool acts and then applies to half of it is one nobody can predict from, and the two modes disagreed about which voxel the crosshair meant. The `mode` parameter is gone from `resolve_point` rather than ignored, so there is nothing left to get wrong |

| # | Decision | Source | Notes |
|---|---|---|---|
| D379 | **Constraint points are uncapped, and what had to be solved was cost rather than storage** | user request, measurement | Third attempt at the same question. Eight was a cap with a shrug next to it; sixty-four (D375) was a bigger number and the same shape of answer; *"make it infinite"* is the right ask. Storage was never the hard part — the points moved out of the parameter block and into the tail of the clip cell buffer, which is already bound to both shaders that draw previews, so no pipeline gained a binding to carry twelve bytes a point. **Cost is the hard part**, because each point is tested against the ray per pixel. Two bounds, neither of them a limit: the whole set has one box, so a pixel that cannot be looking at any point pays one slab test; and the set is **sorted into spatial cells and split into groups of thirty-two, each with its own box**, so a pixel inside the outer box opens only the groups it crosses. Sorting is what makes the second one work — a group of thirty-two scattered across a building has a bounding box the size of the building and rejects nothing. Measured at 0.75 m from a wall, three runs each, on **the worst arrangement the hierarchy can face** (one dense cluster, so every group box covers the same pixels): **0.783–0.791 ms with none, 0.789–0.796 with sixty-four, 0.814–0.816 with three hundred and six.** Scattered points — the case a player actually makes — is where the groups earn their keep and this measurement does not show it |
| D380 | **X repeats while held, and refuses to mark the voxel it just marked** | user request | Dropping a line of constraint points was one tap per point. It auto-repeats now, on the same `KeyRepeat` the clipboard's counters use, so a line can be swept out with the mouse. That makes the same voxel arrive fourteen times a second whenever the crosshair is still, so a repeat landing where the last one did is dropped: the list is a set of *places*, and one recorded fourteen times a second is fourteen times the cost for nothing. Only the immediately previous point is compared, which is all a swept line can produce — deliberately crossing an earlier point still marks it again, and that is harmless |

## Ambient occlusion — the blocky wall, and how much of it was one bug

| # | Decision | Source | Notes |
|---|---|---|---|
| D381 | **An ambient ray that grazes its own wall counts as contact, and a flat wall comes out as a patchwork** | user report, measurement | Reported as *"ambient occlusion looks as if the sub-voxel smoothness of the shadowing of the faces was misaligned or rotated instead of being smooth."* Debug view 18 — the near field on its own — showed it plainly: **a flat facade at 0.75 m ran 129 to 246 of 255 with a standard deviation of 13**, in blocks, on a surface whose correct answer is one number. A cosine-weighted ray leaving a flat wall cannot geometrically hit that wall, but it can *graze*: at a shallow angle it travels many voxels sideways while rising almost nothing, and the DDA lets it clip the shared edge of two cells in the surface layer — the same leak the sun's jitter comment describes from the other side. `contact` then reads nearly one a few centimetres out and the face goes dark, and which faces get unlucky depends on their jitter. The gate is that a hit must have **risen clear of the face's own plane**, three quarters of a voxel, and it is free to compute: `sky_dir` is built with `up * sqrt(1 - hemi.x)`, so the rise is that square root times `t` and nothing has to be dotted. **sd 13.08 → 10.55**, and the horizontal banding across the facade goes. GPU 3.426–3.438 ms against a same-commit control's 3.417–3.439 — unmeasurable, three runs each |
| D382 | **What is left is variance, not a bug, and two candidates were eliminated to establish that** | measurement | The gate did not fix everything and the rest is worth naming precisely rather than chasing. **Face-to-face roughness** — the mean second difference along a scanline, which kills any smooth trend and leaves the steps — is **2.5/255 on a flat wall**, against the renderer's own run-to-run noise of 2.50/255 (D336). It is at the floor. So the residual is per-face Monte Carlo variance in a near field estimated from a couple of hundred samples, and what makes it *visible* is that a dark interior is lit by this term alone and the exposure pushes it hard. **The R10c gradient was suspected and is not the cause**: turning it off moves a lit interior by 1.619/255 and a dark one by **0.529/255**, and it adds 0.44/255 of roughness. **Coarse stand-in faces were suspected and are not the cause either**: the level histogram at the camera in question is 49,108 faces at level 0 against 1,301 at level 3. The answer to what remains is **R5, the face denoise**, which the plan has always owned and which is not built — filtering across neighbouring faces is exactly the shape of it |

## Open items carried forward

- **O21.** Link to the deprecated WorldShaper repository (UI style reference only).
- Round-1 and round-2 questions are otherwise closed. Round-3 questions will be raised per stage as they arise.
