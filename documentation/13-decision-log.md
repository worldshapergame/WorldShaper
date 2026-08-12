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

| # | Decision | Source | Notes |
|---|---|---|---|
| D383 | **A face's ambient samples are one stratified sequence, not two hundred independent draws** | measurement | D382 named what was left of the ambient occlusion's look — per-face Monte Carlo variance — and handed it to R5, the face denoise. Most of it did not need a denoiser. Each face was drawing a fresh pair of white-noise numbers every time it was shaded, so its samples clumped and gapped and the error fell as one over the square root of them. They are one **R2 low-discrepancy sequence** now, indexed by how many samples that face has already taken, which spreads them evenly over the disc for the same ray and the same cost. Face-to-face roughness on the terrace, mean second difference along a scanline: **8.975 → 5.130 of 255**, with the mean unmoved at 219.1 → 219.2 — the answer is the same, the noise around it is not. GPU 3.961 → 3.940 ms on that camera and 3.430 enclosed, inside D382's own control range |
| D384 | **The rotation is per face and must not include the frame** | — | Two ways to get this wrong and both were considered. Every face walking the identical sequence lines their errors up into something structured, which is worse than noise because the eye finds a pattern and cannot find grain — so each face is offset by a Cranley-Patterson rotation of its own. And that offset is hashed from the **slot alone**: put the frame in it and the rotation changes every sample, which throws the stratification away and returns white noise wearing a low-discrepancy costume. A sequence has to be a sequence, so the offset is fixed for the life of the face |

| # | Decision | Source | Notes |
|---|---|---|---|
| D385 | **The ambient window was the noise floor, and it is eight times longer because ambient occlusion depends only on geometry** | measurement | The arithmetic says it outright: a contact fraction near a half, averaged over N samples, has a standard deviation of √(0.25/N), and at the 256-sample window that is **8 of 255 — which is the face-to-face roughness that was measured on a flat wall**. The cap *was* the floor. The sun needs a short window because the sun moves and a face has to be able to forget where it used to be. **Geometry does not move on its own**, and since an edit now tells the faces inside it to start again from nothing rather than leaving them to average their way back (cebf015), the ambient window is not a forgetting mechanism at all — it only stops the counters overflowing. `kSkyWindow` is 2,048, which the sixteen-bit counts and the full-word sums hold with room to spare. Roughness **5.130 → 3.752 of 255**, GPU unchanged at 3.940 ms, `--validation` clean and both mirrors matching |

| # | Decision | Source | Notes |
|---|---|---|---|
| D386 | **The sub-voxel gradient was three quarters of the remaining noise, and it is believed only as far as it clears its own error** | user report, measurement | The player separated the two terms by eye before any instrument did — *"it's the sub-voxel AO, not the per-voxel face AO"* — and turning the gradient off measured it: roughness **3.752 → 1.076 of 255**, so **71%** of everything left belonged to that one term. It is the arithmetic one should expect. The mean of N samples has a standard error of √(0.25/N); a first moment against a coordinate running over [−1, 1] has about √(1/3N), is then multiplied by three, and lands as a **tilt across the face** rather than a level. On a flat wall the true gradient is nought, so that tilt is invention: every face leans a different way and neighbours disagree at their shared edge. Each coefficient is now scaled by `c²/(c² + noise)`, which keeps a crease's gradient nearly whole and a flat wall's hardly at all. **3.752 → 1.881**, against a floor of 1.076 for having no sub-voxel term at all — so three quarters of the term's noise goes and the detail stays |
| D387 | **A hard shrink is worse than no shrink, and the reason generalises** | measurement | The first attempt used the positive-part estimator, `max(0, 1 − noise/signal)`, which is the textbook form and made the picture **four times rougher than leaving the gradient alone** — 15.469 against 3.752. It is a threshold: two neighbouring faces whose coefficients straddle it keep wildly different amounts and disagree at their shared edge by far more than the noise ever did. **A hard decision taken per face is a new discontinuity per face**, and this renderer stores its answers per face, so any per-face nonlinearity has to be continuous in whatever it is deciding on |

**A measurement hazard, recorded because it cost a run.** These figures come from `--screenshot`
runs, and those open a window on the desktop. The player was using it. One run at frame 7,000 came
back with the face store full at 1,048,576 and the frame at 2.363 ms — a different scene entirely —
and it was discarded rather than reported. **`--headless` does not help: it runs a world audit and
renders nothing.** There is no offscreen render path, so anything measured this way is measured on
somebody's screen, and the honest options are to say so, or to build one.

**Where the ambient look now stands.** Face-to-face roughness on the same flat surface, over the
three changes: **8.975 → 5.130 → 3.752 of 255**, at no cost in either — 3.940 ms against 3.961
before any of it. What is left is honest and is stated rather than claimed away: the long window is
a *ceiling*, not an instant improvement. A face reaches 2,048 samples after some thousands of
frames, so the picture keeps getting smoother while a player stands still and a face that has just
come into view is exactly as noisy as it ever was. Converging the first two hundred samples faster
is what R5's spatial filter is for, and it is the only thing left that can help a moving camera.
The *origin* jitter across the face is also still white noise, and stratifying it against the same
sequence is the same four lines.

## Ambient occlusion converges instead of trickling — stage R10d

*The user asked for ambient occlusion to "load 100× faster" and to "refine itself 100× faster", at
"no performance drop, or negligible", and for it not to tank the frame rate when voxels are placed,
removed or streamed in. D383–D387 took every bit of variance out of a sample; this takes the samples
out of the calendar. **All figures are same-commit A/B, `--settle`, 1280×800, quality 7, RTX 5060
Ti, scene content hash `766f2fd63f1a01c4` on both sides.***

| # | Decision | Source | Notes |
|---|---|---|---|
| D388 | **The near field and the far field stopped sharing a ray, and that is what made the term affordable to converge** | measurement | One ray answered both halves of the ambient integral, which looks like the obvious economy and priced the cheap half at the dear half's rate. The **near field** is a falloff over `kContactMetres`: every answer past a metre is the same answer, so its ray is now **bounded at 32 voxels**. The **far field** asks whether a direction reaches sky, which is a question about the other end of the world and cannot be bounded at all. `node_march` therefore takes a `max_t`, the light record grew from four words to five so the two counts are kept apart, and `resolve.comp` gates them separately. Trap 7 is why they cannot share a count: a bounded ray that runs out of length and a ray that reached open sky both return `hit == false`, and only one of them means "sky" |
| D389 | **A face measures its ambient term hard, once, and then stops for ever** | measurement | Ambient occlusion is a function of **geometry**, and the host already says on the exact frame when geometry changes (`edit_min.w == 2`, D373). So the steady-state cost of this term is **zero rays** — which `21-renderer-rewrite.md` §8 R10 claimed from the start and which nothing implemented. Every ray a converged face was casting re-derived a constant it already held, one a visit, per face, for the rest of the run. It stops at `kSkyConverged` now, and the rays it saves are what the burst is spent out of: **total rays over the life of a face go down**, 2,048 and then silence against 2,048 and then one every stride for ever. Enclosed camera, faces pass **1.238 → 0.790 ms** (−36%), total GPU **3.348 → 2.936 ms** (−12%), with every live face's ambient term finished by measured frame 120 |
| D390 | **"Converged" has to be answerable from the record the pass has already read** | measurement | The first version asked the face-light array whether a face was finished, which is a scattered four-byte read per face per frame across a 21 MB buffer — half a million cache lines fetched to learn there is no work. The pass sat at **2.13 ms with every visible face converged and casting not one ray**. The answer is a bit in `photons`, which is GPU-owned and inside the thirty-two bytes the invocation reads anyway, coalesced across the workgroup. And it is **two** bits: `kFaceAmbientDone` (no rays ever again) and `kFaceAmbientIdle` (nothing to do on a frame the sun is not due). One bit for both meanings made the audit report the store as finished while the pass was plainly still paying — trap 7, in the instrument this time |
| D391 | **The far field stops when it runs out of variance, not when it runs out of budget** | measurement | Sky visibility is a binomial proportion, so its error is p(1−p)/N — **nought on every surface in a sealed room and on every roof under open sky**, which is most of a building. It stops when its own measured error falls under `kSkyFarEps`, floored at 32 samples so the estimate of p is not itself noise and capped at 512 so a face at exactly a half still terminates. A plain unanimity test was tried first and stopped only **8,752 faces of 498,179** on the facade camera, because an exterior face is rarely unanimous and very often 0.9 and settled. Indoors the rule fires almost everywhere: at the enclosed camera the whole store is silent by frame 120 |
| D392 | **A newly claimed face inherits the coarse face standing over it** | — | D308–D311's stand-in, applied to the other half of the ambient integral: a face with no samples reads as fully unoccluded, and indoors that is the worst answer available rather than a cautious one. The face three levels up is already claimed by the same host code, is converged long before its children are found because five hundred and twelve share it, and costs one hash to look up. Eight samples' worth, so the face's own first burst outvotes it. The **gradients are not inherited** — they are a fit over this face's extent and the ancestor's is over one eight times wider, and D386 measured what an invented tilt costs |
| D393 | **The samples are one four-dimensional stratified sequence, not two two-dimensional ones** | — | D383 made the ray *direction* a stratified sequence and left the *position on the face* as white noise. Both come from one R₄ sequence now — the four-dimensional generalisation of the plastic constant D383 uses in two — because two R₂ sequences advanced by the same index are both affine in it, so where a sample stands on the face would predict which way it leaves. That pair is exactly what the gradient is a fit over, and it would have been drawn along a line through a 4D space instead of spread over it |
| D394 | **What costs in this pass is the face, not the ray — so three ways of metering the burst all made it worse** | measurement | The burst is sixteen bounded rays in the visit where a face used to get one, so the obvious worry is a population that starts measuring all at once: a camera turned round, a region streaming in, a chisel. Three meters were built and measured against no meter at all, at cut+240 after a 180° turn: **fewer rays each, budget divided by the population — 3.17 ms**; **whole burst, one face in N per frame — 5.61 ms**, which also wrecks coherence, four bursting lanes in a workgroup of sixty-four and the other sixty waiting on them; **whole burst, one workgroup in N — 3.80 ms**; **no meter at all — 1.21 ms, everything converged.** Ninety thousand faces at thirty-two rays and two hundred and forty thousand at two cost about the same, because an unconverged face reads its record, builds its sampling frame and walks the branchy half of this shader whether it then casts thirty-two rays or two. **The cheapest thing to do with a face that has to measure is to let it measure, all at once, and be finished with it.** All the metering was removed, host side included |
| D395 | **An edit resets only what an edit can change, and the two halves of the term have very different reaches** | measurement | `kEditShadowReach` is 512 voxels because a *shadow* cast by what you placed can land sixteen metres away. The **near field is blind past one metre**, so resetting it over sixteen metres made placing a single voxel cost every face within sixteen metres its full burst. The near field now resets within `kEditAmbientReach` — 64 voxels, the falloff and as much again — and the far field keeps the full reach, because opening a roof really does change how much sky a floor across the room can see, and the far field costs one ray a visit to put right. Measured, small carve at the enclosed camera: faces **1.279 → 0.827 ms**, which is *faster than the build it replaces* rather than a cost at all. The 36-million-voxel delete, which resets fifty thousand faces' near field by construction, is 1.690 against 1.242 for forty frames and back to 0.79 after |
| D396 | **Padding the light record to a 32-byte stride bought nothing, and it was tried because it should have** | measurement | Five words is a 20-byte stride: unaligned, unvectorisable, and straddling a cache line three times in four — which is the reason `GpuFace` is thirty-two bytes. Padded to eight words it measured **2.185 ms against 2.185**, and 1.256 against 1.053 once converged, for 34 MB against 21. This record is touched a handful of times per *face*, not per *step*, so its stride is not where this pass spends anything. Not carried |

**Measured, all A/B against a same-commit control.** Enclosed camera, settled, two runs each: faces
**1.238/1.234 → 0.790/0.796 ms**, total GPU **3.348/3.344 → 2.936/2.962**. Facade camera once
converged: faces **1.774 → 0.86**, total GPU **4.560 → 2.32**. Near-field error against the
converged answer at the same frame after a hard 180° cut: cut+8 **21.4 → 9.7** of 255, cut+30
**13.1 → 4.4**, cut+240 **3.50 → 0.95**. A face takes **sixteen ambient samples a frame instead of
one every `face_stride` frames** — 32× to 96× the sampling rate depending on the camera — and then
stops casting altogether. 458 tests, 18.0 M assertions, passing.

**What it costs, stated rather than claimed away.** A screen that turns over completely puts ninety
thousand faces into the burst at once, and for about a second the faces pass runs at **2.0 ms mean
and 4.6 ms worst against the 1.14 and 2.03 of the build it replaces**, before dropping below it for
good. `kSkyBurst` is the dial and the trade is exactly linear: thirty-two halves the time and
doubles the peak, eight the reverse. Sixteen is where it is set, and the peak is bounded by that
constant rather than by anything adaptive, for the reason D394 measures.

**New instrument.** `--debug-mode 19` paints how far each face is through its ambient rays — green
converged and silent, red held short of it by unbuilt geometry, grey the progress between, blue no
samples, magenta no face. The audit line `ambient on the card: N of M live faces cast no more rays
at all` is the same question over the whole store rather than over what is on screen, and it is
counted over **live** faces: the first version counted dead slots below the watermark and reported
21,679 faces still casting when the true number was three.

## The world sharpens and nobody is told — stage R10d, the other half

*R10d made a face measure its ambient term hard and then stop casting rays for ever, on the grounds
that ambient occlusion is a function of geometry and **the host says on the exact frame when
geometry changes**. That premise was true of edits and false of everything else. The clip ladder
sharpens the building region by region in the background and pastes each box straight into the
world's bricks, and for the whole life of the ladder that path told nothing downstream anything.
All figures are same-commit A/B at the enclosed camera, 1280×800, quality 7, RTX 5060 Ti, both sides
on scene content hash `1f4710eee4ee2585` — the same world, reached two ways.*

| # | Decision | Source | Notes |
|---|---|---|---|
| D397 | **A region paste makes the same announcement an edit makes, because it is the same event** | measurement | `pump_refinement` rewrites a box of the world at four times the detail it held a moment ago. Nothing downstream can notice on its own, and the two that most needed to could not for opposite reasons: the node pool's feedback reports what a ray could not **find**, and a brick that is resident but out of date is found every time; a face's ambient term, since D389, has stopped taking the samples that would have told it. So the paste now calls `announce_world_change`, which is `invalidate_edited_chunks` with the ops taken out of it — the light window, the edited box, residency, the summary tree, the thumbnail tiers and the pool's bricks. **Measured, one run watching the world sharpen against one loading the identical world whole: the near field went from a mean of 19.10 of 255 over 547,411 pixels apart to 2.43 over 42,096**, against a run-to-run floor of about 1.5 and 14,000 for this camera. Sun visibility over the same pair, 0.1076 against the loaded run's 0.1282, and **20 faces in full sun against 14,506** |
| D398 | **The pool was holding 7,497 of its 17,344 leaves in a shape the world had given up, and nothing anywhere said so** | measurement | The geometry half of the same fault, and the more alarming half: a leaf is a copy taken at build time, eviction is only under memory pressure (D247), so a brick built before a paste holds the blocky version for the rest of the run. `Resident::revision` has been sitting in `node_pool.hpp` unread since R1a — residency does exactly this check at `residency.cpp:660` and has consequently never had this bug. **It is an instrument that was missing, not a mechanism**: `node_buffers_.audit` asks whether the card agrees with the pool, and both agree perfectly about a brick neither has looked at since the world rewrote it. `NodePool::stale_leaves` asks the question that was not being asked — how many built leaves hold an occupancy the world no longer has — and the screenshot audit now prints either that count with the first offending brick's coordinate, or *the node pool agrees with the world, leaf for leaf*. Two headless tests pin it |
| D399 | **The cost is charged where the change is, which is a paste that already measures in seconds** | measurement | Invalidating a region's bricks and reopening the light inside it is not free, and the honest place to look for it is the sharpening period, which `--settle` discards *by construction* (trap 14). Over 450 frames mid-ladder: faces **2.114 → 2.382 ms** mean and **4.160 → 4.590** worst, CPU node pool **0.065 → 1.002 ms** mean. The two runs are not on identical scenes — one got three regions further than the other before the shot — so these are the shape of the cost rather than a clean A/B. On a **settled** world, where no paste happens, there is nothing to charge and nothing is charged: CPU node pool **0.035 ms**, the same 0.025 the session opened with, and the pool agrees with the world leaf for leaf |
| D400 | **`invalidate_edited_chunks` now unions its group and hands over one box** | — | It used to grow the light window from the union of the group's ops and then iterate chunks and bricks per op. Two shapes for one question, and only one of them could be handed to a writer that is not made of ops. The union is what the window already used; for the single-op cases that matter — a scripted `--edit`, the 36-million-voxel delete — it is the identical box. Checked on the delete: mean sun visibility **0.6405** against the 0.6350 §4b records, nought shadowed by ignorance, and the pool agreeing with the world afterwards |

**What is left here, measured and not fixed.** A face whose ambient rays were stopped by geometry
the pool has not built is held one sample short of converged and comes back at one bounded ray a
visit (D389). **On a settled world that is 50,578 of 848,622 surface pixels — 6.0% — at
`--debug-mode 19`**, and it is a real population rather than a corner case. The hold releases on the
first ray that comes back clean, which takes the count to `kSkyConverged` and **freezes the mean
with 2,047 samples in it that were measured through the shell**. That is a bound rather than
garbage — `occlude_unknown` stops the ray at the shell's near face, so the sample is the earliest
possible contact and errs dark — and the error it can carry has **not been measured**, because
nothing counts ambient ignorance by level the way the sun's line does. Two things not to do without
that number: collapsing the history on a clean ray oscillates for ever on a face that permanently
borders unbuilt geometry, and dropping the tainted samples outright makes the face wrong-bright,
which R10e is explicit about.

## Lamps on the face — stage R3c, the second half

*R3c was "sun and lamps in the face pass" and only the sun had landed. The real-time path had no
lamp light at all: an emissive voxel drew as a pale block and lit nothing, and the constant
`kIndirectFloor` in the composite stood in for every fitting in the building at once. This is the
lamps, on the same one-invocation-per-face footing the sun and the ambient term already use — and
the responsiveness the user asked for by name: **placed, deleted or tweaked, everything reacts at
once.** All figures are A/B against a same-commit control built with `kLampConverged = 0`, which
turns the term off and changes nothing else, at the enclosed camera, 1280×800, quality 7, RTX 5060
Ti, `--settle`, both sides on scene content hash `766f2fd63f1a01c4`.*

| # | Decision | Source | Notes |
|---|---|---|---|
| D401 | **A lamp is sampled once per FACE per frame, never once per pixel, and a face never loops over lights** | design | The estimator is the path tracer's own `pick_light`, moved off the pixel: `kLampCandidates` fittings are scored by what each would deliver here unshadowed — radiance × cosine × the solid angle of the sphere that *contains* the fitting — one is kept in proportion to that score, a direction is drawn inside its cone, and the sample is corrected by the density that produced it. Resampled importance sampling is not a refinement here: `clips/many_lamps.clip` holds eighty-four fittings and a face in one quarter of that hall is walled off from thirty of them, so a uniform draw makes one sample in eighty-four useful and gives it eighty-four times the weight. **A hall with a thousand sconces costs a face exactly what a hall with one costs**, which is R9g's promise arriving early. The same expression as the reference tracer's, deliberately — two renderers computing this differently would light the room one way and aim the rays another (D204's rule, in a third place) |
| D402 | **What a face stores is irradiance, as a float sum, and not a fraction** | design | The sun and the ambient term store two counts and divide on read, because both are fractions in [0,1] and D293 is the standing measurement of what a narrow running mean does to one. A lamp's contribution is not a fraction: it is radiance over a solid angle divided by a density, unbounded above, and there is no scale a fixed-point record could be defined against. Three float words hold the running SUM and a fourth holds the count, so the mean is exact at any magnitude and readable from the first sample. `kFaceLightWords` goes 5 → 9 and the face-light buffer **21 MB → 38 MB** for a million slots; padding to sixteen was not done, for the reason D396 measured at five |
| D403 | **The host says the lamps changed; the face does not infer it** | design | This is the whole of "instantly", and it is D373's lesson applied to a second accumulator. A converged face stops reading its own record, so nothing it could measure would ever tell it a lamp had gone out. So `light_list_hash` gives the emitter list an identity, a change bumps a version, and `light_reset` is 1 on the one frame it changes — which reopens every face in the store for that frame. Each face then compares the version its own samples were taken under against the current one and decides for itself, so the flag is the gate and the stamp is the decision. **Six headless tests pin the identity**: placing, deleting, dimming and carving a fitting all change it; an edit twenty metres from the nearest sconce does not, and that half decides what this costs |
| D404 | **A changed list drops a face's CONFIDENCE, not its answer** | measurement | Throwing the accumulated mean away on a lamp edit is exact and it flashes: every surface in the frame would read one or two rays' worth of an unbiased estimator on the next frame, which is the variance of a coin toss applied to the whole picture at once. The mean is kept and the count dropped to `kLampSeedSamples` = 8, so the face's own next burst of 8 outvotes the prior on the very next frame and has buried it within three. **The one exception is the last lamp going out** — with no emitters there is nothing to measure and a kept mean would go on lighting the room for ever, so that case alone goes straight to nought |
| D405 | **Measured: three quarters of a lamp change is on screen on the next frame, and it is at the noise floor by frame fifty** | measurement | The portico's fittings carved away at a measured frame (17.2 M voxels, 21 emitters → 17, list version 2 → 3), against the settled answer 150 frames later. **Lamp term alone (`--debug-mode 20`), against a total change of 32.965 of 255**: edit+1 **8.979 (73% arrived)**, edit+2 7.926, edit+5 4.927 (85%), edit+15 **0.819 (97.5%)**, edit+50 **0.220 (99.3%)**. The whole shaded frame, against a total change of 63.571 and a run-to-run floor of 1.16: edit+1 11.514, edit+15 2.087, edit+50 1.250. **Nothing here waits for the feedback round trip**, because these faces already exist — the two frames R3e could not remove are the cost of a face being *new*, not of its light being wrong |
| D406 | **A converged face touches nothing — kept on the argument, because the number came out inside the noise and the first number was wrong** | measurement | The lamp block read the sample count and wrote it back on every visit, which is half a million scattered four-byte read-modify-writes a frame on a term whose answer has stopped changing. Gating it on the flag already in `photons` first measured as **0.89 ms**, and *that figure was an artefact*: the two builds were compared on runs whose stores had converged to different degrees, which is trap 8 in the instrument this session was building. Re-measured properly — three rounds, rebuilding between every run so drift hits both arms equally, both with lamps on — the gate reads **3.031 ms mean against 2.964 without it**, which is nothing, and **5.06 ms worst against 5.50**, which favours it by less than its own spread. **It is not measurable on this camera.** It is kept because it is the rule `kFaceAmbientDone` already states in full and because a removed load cannot become a cost, and it is written down here because the way it was first "measured" is a trap this project has now fallen into twice |
| D407 | **What it costs, measured by interleaving the two builds because the machine drifts more than the effect** | measurement | The same build read **2.41 ms and 3.75 ms** on the faces pass across one session, on one scene, with the store converged in both — so nothing here is quoted from adjacent batches. Three rounds, rebuilding between every run, settled, 300 measured frames: faces **2.613 → 3.075 ms mean** (2.599/2.611/2.628 against 2.859/3.133/3.232) and 4.54 → 5.21 worst, total GPU **5.706 → 6.217** (+9%). So the settled term is **+0.46 ms, +18% of the pass**, inside its 4.40 ms budget — not free, as the first pass of this claimed, and not scaling with resolution either. The control's own spread is 0.03 ms and the lamp arm's is 0.37, which is the lamp arm's population of still-measuring faces rather than the machine. **The transient is the larger number and it is charged where the change is**: across the 80 frames after a 17.2-million-voxel carve, two interleaved rounds give faces **17.08 → 19.80 ms mean** and **24.25 → 28.83 worst**, so lamps add ~2.7/4.6 on top of a re-burst that is already 17 ms and is the ambient term reopening every face inside `kEditShadowReach`. `kLampConverged` = 512 at `kLampBurst` = 8 is sixty-four frames, and it is the dial; D394 says not to meter it |
| D408 | **The picture: 21.78 of 255 over a quarter of the frame, and the portico stops being a black hole** | measurement | Against the control, the enclosed camera moves by a mean of **21.78 of 255 over 261,393 pixels of 1,024,000**. That is not a tint — the deep shade under the portico is the one place in this building the sun never reaches, and it was lit by a constant. It now has six flames in it casting real pools of light with real falloff, the columns cast lamp shadows across the floor, and the fittings themselves glow rather than reading as pale blocks (emission is added after the albedo multiply, because a lamp is bright whether or not anything shines on it). **The cost is reproducibility**: two runs of this build differ by 1.157 of 255 over 23,066 pixels where two runs of the control differ by 0.569 over 2,513, which is the Monte Carlo residual at 512 samples and is R5's to take, exactly as the ambient term's is. Anything gating on an image diff from here on has that floor |
| D409 | **Open, and found by building this: the real-time composite's exposure is fixed, and a lamp-lit interior now says so** | measurement | `clips/many_lamps.clip` is a sealed hall lit by thirty-six sconces and nothing else. The face pass gets it right — the reference path tracer on the same camera draws the same distribution of light and is correctly exposed — and the real-time picture is **blown white**, because `kPreviewExposure` in `resolve.comp` is the constant 3.2 and says in its own comment that real exposure control is a later stage's job. Nothing was wrong with it while every interior was lit by a fraction of a sky constant. It is wrong now, it is the next thing the lamps make necessary, and it is deliberately **not** fixed here: adding auto-exposure changes the brightness of every screenshot in the project at once and would make every figure above incomparable with every figure before it |

**Instrument.** `--debug-mode 20` is the lamp term on its own — tone mapped, because a lamp's
contribution spans orders of magnitude between standing under a sconce and standing across a hall
from one, and a linear ramp shows one end and clips the other. Magenta is a pixel with no face, blue
a face that has not measured yet, green no geometry: black is a legitimate answer here and must not
be shared with any of the three (trap 10). The audit line `lamps on the card: N of M live faces cast
no more rays at all` is the same question over the whole store, and it is the number to read before
believing any cost figure here — D406 is what happens when it is not.

**The measurement lesson, because it is the more useful half of D406 and D407.** Two builds of this
pass cannot be compared from adjacent batches. The faces pass is a function of how much of the store
is still measuring, that state is not reproducible frame for frame, and the machine drifted by 55%
of the pass over one session. Every figure above is from **interleaved rounds with a rebuild between
every single run**, and the arms are checked against each other's `still bursting` and live-face
counts before the numbers are read. Trap 8 says a measurement is against a scene; this adds that it
is also against a *convergence state*, and the second is invisible in the `scene:` line.

**Two rules kept rather than re-argued.** A lamp ray reports nothing and asks for nothing (D292,
widened by R9h: no light path may cause streaming), and it stops short of the fitting it is aimed
at by `kLampSurfaceMargin`, or every sample would be occluded by the thing it is sampling. A ray
stopped by a cell the world claims and the pool has not built holds the face one short of converged
and it comes back at one ray a visit, which is the ambient term's hold with the same bound and for
the same reason.

**One hazard written down rather than fixed.** `light_buffer_` is a single host-visible mapping
written by `update_lights` while a frame in flight may be reading it. It predates this stage — the
path tracer has always read it the same way — and the exposure is one frame of possibly torn lamp
records on the frame a lamp changes, on a term that restarts its samples on that exact frame anyway.
Double-buffering it is a descriptor change and belongs with whatever else next needs one.

## The light pass while moving — the case the settled grid discards by construction

*The user asked for the lights to be faster and for more frames while MOVING. The first thing that
had to exist was a measurement of that, because there was not one: every figure in this file above
this line is settled, and `--settle` discards transients on purpose. Nothing was changed in the
engine here. What landed is the instrument and the diagnosis it produced, in that order, because a
change measured against the wrong case is worse than no change at all.*

| # | Decision | Source | Notes |
|---|---|---|---|
| D410 | **A moving camera is a measurement, and `baseline.ps1` structurally cannot take it** | design | `tools/_flybench.ps1` drives `--fly` along a fixed path at the fixed 1/60 step the flight already uses, and prints the pass table. It is not a refinement of the grid — the grid starts its window at refinement's fixed point, so by construction it measures the state where every face has converged and nothing is bursting. That is the right thing for comparing marchers and the wrong thing for comparing light. This is trap 14 again with the instrument on the other foot: the settled grid said the face pass costs 1.11 ms and the player was watching it cost eleven |
| D411 | **Measured: the light pass is 11.75 ms of an 18.6 ms frame while moving, and 1.11 ms of a 3.3 ms frame standing still** | measurement | Close camera, `--fly 0,0,3,15`, 2560×1440, quality 7, `--settle`, 200 measured frames, RTX 5060 Ti. Three runs: faces **11.588 / 12.090 / 11.581 ms** mean, worst 15.3–17.4, against a **4.40 ms budget**; visibility 3.30, resolve 2.78, total GPU **18.6 ms**. The same camera standing still reads faces **1.110 ms** and total GPU 3.345. So the pass is **63% of a moving frame and ten times its own settled cost**, and every claim in `21-renderer-rewrite.md` §6 about lighting not scaling was measured on the standing-still half. **The spread is 4% and the convergence state is reproducible to 0.1%** — 280,551 / 280,887 / 280,695 faces still bursting across the three runs — so D407's warning is satisfied and two builds can be compared on this case |
| D412 | **The cause, named and deliberately not fixed in this change: every ray throws the descent cache away** | measurement | A bursting face casts `kSkyBurst` 16 near rays + 1 far + `kLampBurst` 8 lamp rays + the sun's, and at 280k bursting faces that is about **6.3 M rays a frame**. `node_march` opens with `node_walk_reset()`, so each of those ~25 rays pays a hash probe and a full eleven-level descent from the 512 m root before its first step — for a near ray that is *bounded at one metre* and then takes a handful of steps. The cache is safe to keep: node.glsl's own argument for it says the pool is immutable for the whole dispatch and `node_locate` block-checks before trusting an entry, so a cached slot is a function of the tree and not of the path taken to it. Rays from one face share an origin cell, so rays 2..25 would enter at level 5 and descend two levels instead of eleven, **bit-identically**. It is left for the next change because it has not been measured yet, and R1h is the standing reminder of what happens when a descent cost is reasoned about rather than read |

**The ranked plan this produced**, biggest first, each to be measured on its own against
`_flybench.ps1` and interleaved per D407:

1. **Drop `node_walk_reset()` from `node_march`.** The globals already carry their initialisers per
   invocation, so nothing else has to change. Output identical.
2. **A dedicated `node_occlude()`** returning `{hit, unknown, t, level, steps}` — everything the
   three ray sites in `shade_faces.comp` read and nothing else. It drops `leaf_voxel_type`,
   `node_face_coverage`, the normal tracking, `node_face_hit` and the whole `stand_in` branch, and
   it keeps the `kFeedbackExact` report R9i needs. Three copies of the full marcher are inlined into
   that shader today, and D181 is the standing measurement of what code size costs there.
3. **Keep `photons` and `counters` in registers** through `shade_faces.comp`'s `main`. There are up
   to five global read-modify-writes per face per frame on two words the invocation already holds —
   which is the fault D406 fixed for the lamp count, in the same function, one term over.
4. **A hierarchical skip inside a brick**, from the `mip_cell2` and `mip_cell4` the `LeafHeader`
   already carries. The inner walk steps up to twenty-four single voxels with no mip test at all.
   Same first hit, exactly.
5. **The store churns while moving** — 545,310 evictions over 400 frames at 763,829 live of
   1,048,576, so converged faces are being given up and re-bursting from nothing. A separate lever
   with its own measurement, and it is R2's question rather than R3's.

## The light pass stops lighting what nobody is looking at — and the case that actually hurts

*D410–D412 measured the light pass while moving and named a cause. This is the change, and the
cause named there turned out not to be it. The player also named the case that was missing: not
flying, and not editing, but **flying while editing**, which is what holds the button down feels
like and which no instrument in this repository could reach. All figures are interleaved
same-build A/B — the two arms are two FLAGS, not two builds, for the reason D407 records — at
2560×1440, quality 7, `--settle`, 200 measured frames, RTX 5060 Ti, two rounds each.*

| # | Decision | Source | Notes |
|---|---|---|---|
| D413 | **A chisel that flies with you, because "while moving" and "while editing" are one case and were being measured as two** | user report, design | `--chisel EVERY,RADIUS` carves and fills alternately, every `EVERY` frames, through the same `apply_group` and the same `invalidate_edited_chunks` the mouse button uses. It is not a fixed box: it is aimed with the same `raycast` the Chisel tool aims with, at the surface the camera is looking at. **Both halves of that were got wrong first and both were caught by looking rather than by counting.** The first version put the box three metres along the forward vector; on this flight path that is open air, so every carve changed nothing and every fill hung a cube in the sky — and the counters read "1,437,480 voxels changed" while the facade on screen was untouched, which is trap 1 in a new place. The second scaled `Camera::position_*` by `kVoxelsPerMetre`, and that position is **already in voxels**, so the ray started thirty-two times too far out: ninety-four chunks of the world were created a kilometre away and the `scene:` line went 74 chunks to 168. Aimed properly it changes 1,680,157 voxels over seventy-nine edits and visibly demolishes the building |
| D414 | **The light pass was lighting six hundred frames of scenery, and the frame is made of a fifth of it** | measurement | The store keeps a face for `cold_frames` after anything last asked for it. Every one of those faces was casting its full burst on every frame it owed a ray: **763,800 live faces, of which the frame in front of you reads 218,000**, with 281,244 still bursting sixteen ambient rays, eight lamp rays and a sun ray apiece. That is the rule the whole rewrite was asked for under — *if you cannot see it, it is not processed* — and the light pass was the one place still ignoring it. `node_visibility.comp` already looks a face up for **every** pixel, so it knows exactly, this frame, with no lattice and no round trip; it now stamps the slot it read (one conditional word, and after the first pixel of a face the rest are cache hits). `shade_faces.comp` skips a face no pixel has read for `seen_window` frames. **Residency does not change** — the face keeps its slot, its key and its accumulated answer, so the composite reads what it always read and a face coming back into view carries on rather than starting again. Flying: faces **10.43 → 4.94 ms**, total GPU 16.95 → 12.26 |
| D415 | **An edit reaches every face; only a face somebody is looking at re-measures NOW** | measurement | The first version of D414 let an edit through the gate entirely, because it has to: `edit_min.w == 2` reopens every face within `kEditShadowReach`, which is sixteen metres and indoors is most of the store, and a wall deleted behind your back must not keep its shadow. But "reopen" also meant "burst now", and with the chisel running the face pass hit **75.0 ms on every edit frame**. The two questions are separated: the RESET still reaches every face in the box, visible or not, and is a handful of word writes; the RAYS wait until a pixel reads the face. Worst frame **75.0 → 19.8 ms** with the mean 9.16 → 7.80. The same split covers `light_reset`, so a lamp change still reopens the whole store on its one frame and still costs nothing until the faces are looked at |
| D416 | **The shading dispatch is sized by the work, not by the store** | measurement | Once light stopped at what a pixel had read, 220,000 faces of work were spread over 772,000 invocations — about eighteen busy lanes in a workgroup of sixty-four, and a workgroup runs for as long as its slowest lane. `face_worklist.comp` packs the slots that owe work and the shading pass is dispatched **indirectly** over the count, so every workgroup it launches is full. It costs **0.096 ms** and it took faces 6.47 → 4.99 flying and 10.46 → 8.32 with the chisel. Two things to know: the decision is made by ONE function (`face_work_of` in node.glsl) that both passes call, because a worklist that is the stricter of the two would silently stop lighting faces and nothing anywhere would say so; and the indirect header is `{0, 1, 1, 0}` and not four zeroes, because a dispatch of `(n, 0, 0)` launches nothing at all and reads exactly like the pass suddenly costing a tenth of what it did |
| D417 | **Two of the ranked causes from D412 measured as nothing, and one of them was ranked first** | measurement | **Dropping `node_walk_reset()`** — the top item, on the grounds that 6.3 M rays a frame each paid an eleven-level descent from the root — measured **11.85 against 11.77**, inside the 4% spread. It is kept, on D406's argument that a removed load cannot become a cost, and it is written down because the reasoning behind it was sound and the answer was still no: the descent is cache-resident, and what the pass is short of is not instructions. **Prolongation** — a face subdivided out of a converged one inheriting its parent's fit rather than measuring the same occlusion again — is built, is correct, and measured **4.92 against 5.05** flying and **8.25 against 8.26** chiselling. The reason is in one log line: `faces by level 0:695913 3:27466 6:566`. **There are no level-1 or level-2 faces on this scene at all**, so nothing is ever subdivided — the two thousand faces claimed a frame are newly *revealed* voxels with no finer-scale parent to inherit from. It is kept behind `--no-face-prolong` (off by default) because the case it is for is real in a scene with mid-distance geometry and because the prior it computes is the correct one: the parent's linear fit evaluated at the child's centre with the gradient rescaled by the change of extent, shrunk first exactly as resolve.comp shrinks it on read (D386, D387) |
| D418 | **What the pass actually spends on, by removing one thing at a time** | measurement | Four probes, each one constant, each measured against the same build: **lamps off** (`kLampConverged = 0`, D407's control) 11.85 → 8.95, so eight of the twenty-five rays a bursting face casts are a quarter of the pass; **the ambient target 2048 → 512**, which is four times fewer near-ray frames, 5.05 → 3.55; **the near ray bounded at 8 voxels instead of 32**, 5.05 → 3.45; **the ignorance hold removed**, 3.55 → 3.69, nothing; **the far field switched off entirely**, 3.55 → 3.28, and it takes the "still owe ambient rays" count from 199,432 to 71,759, so the far field is what keeps a face *counted* as unfinished and is not what it costs. The shape that leaves: there is no single remaining lever worth a factor. The cost is the sum of the rays, roughly in proportion to how many there are and how far each goes, and the next real reduction is either **R5's spatial filter** (which is what would let `kSkyConverged` fall from 2,048) or **spatial coherence between the lanes of a workgroup**, which the work list has now made possible to arrange and which nothing has measured |
| D419 | **The measured result, and the one number that did not move** | measurement | Interleaved, two rounds, one build, two flags. **Flying**: faces 10.483/10.372 → 5.043/4.840, total GPU 16.95 → 12.26, CPU 18.96 → 16.27. **Flying while chiselling**: faces 48.340/48.791 → **8.278/8.317**, worst 61.0 → 24.0, total GPU 55.52 → **15.02**, CPU 57.13 → 23.45 — eighteen frames a second to sixty-seven. **The picture does not move**: settled, at the enclosed, close and outdoor cameras, on-against-off is 2.836 / 1.286 / 0.077 of 255 against a run-to-run floor of 2.728 / 1.161 / 0.096, with speckle identical to two decimals (19.30 → 19.27, 55.33 → 55.40, 12.10 → 12.10). **The number that did not move is the edit's own CPU cost**: apply and undo 0.24 ms, the coarse grids **3.55 ms**, invalidation downstream 0.01 ms, per edit. `rebuild_coarse_grids` is O(world) and is called once per edit from the same path the mouse button takes. It is now the largest single thing an edit costs and it is untouched |
| D420 | **A harness bug invalidated eight runs, and it is trap 4 word for word** | measurement | The A/B was run once with a `-Extra` parameter added to `tools\_flybench.ps1` and a local named `$extra` built beside it. PowerShell variable names are case-insensitive, so those are ONE variable, and a parameter declared `[string]` keeps that constraint for life: assigning the argument array to it joined it into a single space-separated string, and the whole lot reached the exe as one argument it ignored. Neither `--chisel` nor the arm flags were passed in any of the eight runs, and **the two arms came out equal to three digits** — which is exactly what a working change with no effect looks like. It was caught because the player said the chisel was not doing anything. The local is `$runArgs` now |

## An edit flashed a slab of the wrong colour, and the pool was throwing away an answer it had

| # | Decision | Kind | Why |
|---|---|---|---|
| D421 | **The reported fault, photographed rather than argued about** | measurement | Reported from playing, in two halves: *"sometimes when I stay still I see bricks flashing with different geometry and with the colours the facility is made of, they even cast shadows"*, and *"whenever I modify a voxel, either place it or carve it, I can see for a brief moment how the brick I placed that voxel on becomes a grey cube whatever material it might be"*. The second half is deterministic and is therefore the one to photograph: `--chisel 300,8` fires exactly one 4,913-voxel edit, and a screenshot at edit+1 against one at edit+60, cropped to the edited cell, shows the whole neighbourhood painted in **flat, hard-edged rectangles of black, cream, sky-blue and tan** where edit+60 draws a clean stone cube. Those rectangles are `node.glsl`'s R2d stand-in — `result.colour = nodes.items[found.slot].colour`, an ANCESTOR's folded colour drawn as one flat cube face over the whole cell. The colours are arbitrary because a fold is only ever over the children that happen to exist, so a node whose only built child is an outer wall paints the wall's colour over an interior, and one folded while a sky-facing child was the only thing built paints sky. **The stand-in was not the bug**; drawing the parent while waiting is right and the alternative is a hole. What is wrong is that anything was waiting at all |
| D422 | **A brick an edit touched is re-derived in place, not dropped and re-requested** | change | `NodePool::update` walked to the brick `dirty_` named, called `release_contents` on it and left it at level nought — the pool's spelling of *"the world has this and I have not built it"*. That is the right answer for a node nobody has asked about and the wrong one for a node that was resident a microsecond ago, because the only route back is the feedback round trip: a ray has to *miss* it, report on frame N, the host reads that on N+2 because that is when N retires, then builds and uploads. Three frames of stand-in. **Nothing had to be discovered**: `dirty_` already names the exact brick and the world already holds the answer, so the edit path now calls `build_leaf` for it immediately and moves the record into the slot it already occupies — the same build-into-a-scratch-slot-and-move that `refine` does, with both ends marked dirty for the reason D-of-byte-18,240 gives. It repairs the ancestors for free: the fold that runs just below now averages a child that is THERE rather than averaging around a hole, which is where half the wrong colours came from. Bounded by the frame's build budget, so a carve of a hundred thousand bricks degrades to exactly the old behaviour on whatever it cannot afford; an emptied brick costs almost nothing because `build_leaf` finds no occupancy and returns before it encodes. **Measured**, same camera and same single edit: edit+1 draws the correct stone block at the correct shape instead of four wrong-coloured slabs. **It is cheaper, not dearer** — 4K with `--chisel 8,16` on scene `43cd76bbc2e2552b`, node-pool CPU **0.196 / 0.195 / 0.219 ms → 0.141 ms**, because the brick no longer has to be missed, reported, re-descended from the root and rebuilt two frames later; total GPU 23.42 / 24.10 / 23.42 → 24.46, at the top of the control's own 2.9% spread. Mirror matches, leaf-for-leaf agreement holds, 468 tests |
| D423 | **The test asserted absence, and absence was standing in for correctness** | test | *an edit drops the brick it touched* checked `find(...) == kNoNode` after an edit, and its own comment said what it was really for: *"what matters is that the stale copy is gone — a resident-but-wrong node is the one thing feedback can never discover"* (D131). Absence is the weaker claim of the two AND it is the visible fault. It is *an edit refreshes the brick it touched* now, and it asserts the property itself through `mirror_voxel`, which reads the way the shader reads: the node is present, the edited voxel reads air, its neighbour still reads stone. One test added beside it for the case the refresh must not answer by keeping a node — an edit that empties a brick outright leaves none behind, because a bit left set over nothing is D133 |
| D424 | **`request` did not mean wanted, and fixing that measured nothing here** | change | `node_last_read_` — the array the erode sweep tests first — was written in exactly one place a ray could reach: `touch_slot`, from the slot a ray STOPPED on. So a node wanted by anything other than a stopping ray aged out on a six-hundred-frame timer while it was being asked for every frame. The proximity radius makes it plain: twenty metres of world is pushed through `request` every frame precisely so collision, physics and editing can touch what is behind you (D199), every one of those lands in `refine`, and every one of those nodes was evicted anyway and rebuilt on the next sweep. `refine` now stamps the chain it walks, on the pass it already makes back up it to fold. **It measured nothing on the camera it was measured on** — close camera, settled, frame 2400: `built 14 evicted 8` against `built 14 evicted 17` and `built 22 evicted 2` across two control runs, and detail-level flicker between consecutive frames 92 pixels against 75, both inside the run-to-run spread. That camera is sixty metres from the building, so its twenty-metre proximity set holds almost nothing, which is why. Kept on the argument rather than the number, exactly as D417 was: a store on a path already in hand cannot become a cost, and a node the pool has been *told* to keep must not be a candidate for eviction. Memory unmoved: 118,424 nodes and 7,245,312 bytes against 118,392 and 7,262,848 |
| D425 | **The node and face staging rings were one region against two frames in flight** | change | Found while looking for D421 and **not** the fault that was being looked for; recorded because it is a real race and because the search is the useful part. `NodeBuffers::upload` and `FaceBuffers::upload` memcpy into a host-mapped ring on the CPU *while recording*, and the `vkCmdCopyBuffer` that reads those bytes runs when the card reaches that command buffer. `kFramesInFlight` is 2 and the loop waits on the slot it is about to record, so when frame N writes, frame N-1's copies may not have executed — and both reset the cursor to offset zero, so frame N-1's copy would source frame N's bytes into frame N-1's destination offsets, which is another array entirely. `WorldBuffers` has partitioned per frame in flight since the chunk path was written and says why in as many words; the two arrays the rewrite added did not. Split rather than doubled, so the allocation is unchanged and a frame stages half of what it could — still far more than any frame produces, and an overrun keeps its ranges marked for next frame, which is what `staging_exhausted` is for. Both audits now bound themselves by the whole ring instead of one region, since they stall the device before they look and measuring against the per-frame capacity would quietly halve the world they cover. **It is not proven to be what the player saw**: the arms were built and run three times each at 1280×800 and 3840×2160 under `--chisel 8,16` and the control reported `GPU mirror matches` every time. The reason is that the copies are the first commands in the frame, so the window is only open when the card is a whole frame behind, and on this machine the CPU wakes out of `wait_for_slot` at the same instant the card starts the next frame and then spends `node_pool_.update()` before it reaches the memcpy. The window is real, it is machine-dependent, and it fails as arbitrary bytes reinterpreted as node records — the facility's own palette when the source was payload, greys and whites when it was occupancy bitmasks |

**What is left here, measured and not fixed.** The *standing still* half of D421 is **not closed**. On a
settled, static, un-edited camera at frame 2400 the pool still reports `built 14 evicted 8` in a
single frame, and a converged tree should report neither; consecutive frames of the detail-level
view differ on 92 pixels of 3,686,400 at 1440p and 2,112 of 8,294,400 at 4K, worst 153 of 255, which
is a whole detail level moving. Every one of those is the same absent-node state D421 photographed,
arriving through eviction instead of through an edit, and it will paint the same wrong-coloured
slab. The remaining hole is named and not yet measured: `touch_slot` stamps the node a ray **stopped
on**, so a brick a ray passes THROUGH — mostly-air bricks at the edges of geometry, which is most of
a facade seen at a grazing angle — is read every frame and stamped never. `NodePool::touch_slot`'s
own comment says *"a ray READ this node"*, and that is not what the code does. The instrument to
build first is a count of evictions of nodes that were on screen that frame, because the two
candidate fixes — reporting every node a ray touches, and refusing to evict anything inside the view
frustum — cost very differently and nothing has measured which is needed.

**And the stand-in's colour is worth its own look.** D421 is only visible because a fold is over the
children that happen to exist, so an ancestor standing in for a cell can be painted sky-blue over an
interior. `fold_children` already skips children at level nought; what it does not do is record how
much of the node the colour it kept actually speaks for, so the composite cannot tell a colour
folded from eight children from one folded from one. That is the same distinction trap 6 draws
between a volumetric and a projected coverage, arriving in the colour rather than in the alpha.

**The face pass** is **8.30 ms against a 4.40 ms budget**
while flying and chiselling, and 4.94 ms while only flying. Its worst frame is 24 ms and that is the
frame an edit lands on: every face within sixteen metres does its reset there, visible or not, and
that is by construction. The three things that would take it further, in the order they look
worth doing:

1. **Spatial coherence in the work list.** Sixty-four lanes of a workgroup currently walk sixty-four
   unrelated neighbourhoods of the tree, so the working set is sixty-four times what it needs to be.
   Sorting the compacted list by the Morton code of the face's coordinate — a histogram, a scan over
   32,768 buckets and a scatter, all of which the list already makes possible — **cannot change a
   single pixel**, because it only changes the order the same invocations run in. Nothing has
   measured it, and the two items above that were reasoned about and measured as nothing are the
   standing warning about that.
2. **R5, the face denoise.** `kSkyConverged` is 2,048 because that is where Monte Carlo noise falls
   under the floor (D385), and it is the largest multiplier in the pass: 512 measured 5.05 → 3.55.
   A spatial filter across neighbouring faces is what buys that legitimately.
3. **`rebuild_coarse_grids` per edit**, which is 3.55 ms of CPU an edit and is O(world) for a change
   that is one metre across.

## Residency only ever heard about the brick a ray stopped on

| # | Decision | Kind | Why |
|---|---|---|---|
| D426 | **The instrument D425 named, and the two things it had to be independent of** | measurement | D425 left the standing-still half of D421 measured and unexplained and said what to build first: *a count of evictions of nodes that were on screen that frame*, because the two candidate fixes cost very differently and nothing had measured which was needed. It cannot be built out of `node_last_read_`: that array decides what is cold, it is stamped from feedback, and feedback was the thing under suspicion — a count taken from it would agree with the policy however wrong the policy was. So there are two independent witnesses, and neither is read by anything that decides anything. **The frustum**, built from the same four vectors that go into the parameter block the marcher reads, so it cannot disagree with where the rays went; it over-counts, since it does not ask about occlusion, and that is the right direction for an instrument about wrongly-evicted nodes. And **churn** — a node requested again within `kChurnWindow` of this pool evicting it — which is the harm itself rather than a proxy and needs no theory about why the signal was lost. Three discriminators hang off the second: the level, the brick's fill, and whether any ray had EVER reported reading it, which is what tells "a node that was read and went quiet" (a sampling problem) from "a node no ray ever mentioned" (a reporting problem). Close camera, 1280×800, `--settle`, static, un-edited, frame 2400: **249,454 evictions over the run, 228,964 of them inside the frustum, 37,213 requested again within two seconds — and 249,414 of the 249,454 were nodes no ray had ever reported reading.** Every churned node was a leaf. Resolution changes it barely at all (37,247 at 800p, 43,147 at 1440p, 49,852 at 4K, while the reporting lattice's period goes 64 → 256 → 1,024 frames against a 600-frame cold window), which **eliminates sampling density as the cause** and was the first thing the instrument was pointed at |
| D427 | **A ray reports the bricks it passes THROUGH, not only the one it stops on** | change | `node_flush_read` is called at the three places the march returns a hit, so residency only ever heard about the last brick of a path. Every brick in front of it — the ones a ray has to walk voxel by voxel to get past, which is most of a facade seen at a grazing angle and every window reveal, cornice and step — was read every frame and stamped never, aged out on the six-hundred-frame timer, and came straight back as a miss. `touch_slot`'s own comment says *"Read by a ray, so it is wanted"*; that was not what the code did. One call after the inner walk falls out without hitting solid, on the same lattice the stop report already uses, so the traffic follows the same screen area it always did. **Measured**, close camera at 1280×800 against a same-build control (`--no-node-crossings`, D407's rule): churn **37,213 → 29,077**, and the part of it a primary ray asks for **1,177 → 0**; churn through a dilated neighbour **6,313 → 313**; resident leaves 21,747 → 20,382. The reported symptom is D425's own instrument — consecutive frames of `--debug-mode 3` on a settled, static, un-edited camera at 1440p, which differ only where a detail level is moving: **13, 0, 3, 50, 0, 2 and 59 pixels of 3,686,400 across seven pairs on the control, worst 153 every time it fires, against nought on all seven with the report on.** Five of the seven control pairs flicker and two do not, which is what a fault arriving in erosion slices looks like and is why one pair is not a measurement. On the outdoor camera the same change takes the renderer's *reproducibility* with it: two runs of the control differ on **12,484 pixels of 1,024,000 at a mean of 0.480**, two runs with the report on differ on **674 at 0.081**, and resident bytes fall 7.08 MB → 6.24 MB with lifetime evictions halved. It costs nothing measurable: interleaved on `_flybench.ps1` at 1440p, visibility **3.700 against 3.643 ms** and total GPU **11.36 against 11.65**, both inside a control that spans 3.456–3.777 by itself; node-pool CPU 2.06 against 2.08. 470 tests |
| D428 | **The harness said "no flicker at all", and it was measuring one frame twice** | measurement | The consecutive-frame pair was written `@($Frame, $Frame + 1)`, and PowerShell binds the comma tighter than the plus: that is `(2400, 2400) + 1`, a THREE-element array whose first two entries are the same frame. Both arms duly reported nought pixels of difference, which is exactly what a working fix looks like. Caught because the eviction counters printed beside it read nought as well, on a run that had certainly evicted a quarter of a million nodes. This is trap 4 and D420 for the third time, in a third disguise — the first was a `[string]` parameter swallowing an array, the second was a local shadowing a parameter by case. **The standing lesson is not about PowerShell**: a measurement that comes back clean has to be checked against something that should NOT be clean in the same output, or it is indistinguishable from a measurement that never ran |
| D429 | **The half that was left, measured, and named before it was fixed** | measurement | With D427 in, **28,764 of the 29,077 remaining returns are asked for by a light ray stopped by ignorance** — zero by a primary ray. That is a closed loop with a period of exactly `cold_frames`: a shadow, ambient or lamp ray is stopped by a cell the pool has not built and reports it (D292's narrowing, R9i), the pool builds it, **no primary ray ever reads it** because it is an occluder and not a visible surface, six hundred frames later the erosion sweep takes it, and the next light ray to reach it reports it again. The bricks are solid — 283 of 512 against 253 for the average resident brick — and 92% of them are inside the frustum, which is where the "and they even cast shadows" half of D421 comes from: an unbuilt cell is opaque to occlusion (D302), so the same loop that flickers a slab also flickers a shadow. It is very likely what holds D400's open item open — *a face held short of convergence by unbuilt geometry is 6.0% of surface pixels on a settled world* — since those faces are held by bricks the pool keeps throwing away. **It was measured before it was fixed and fixed in its own change**, because it touches a rule rather than a bug: D292 forbids a light path from dragging residency towards what it crosses, so the honest change is narrower than "keep what the light reads" and had to be argued rather than assumed. D430 is that change. **The link to R10's open item did not survive contact**: on this camera at frame 2400 the held-short count reads nought in every arm, including one with both rules off, so the 6.0% D400 recorded is not reproducible here and this is not evidence about it either way |

## A light ray keeps the cell that stopped it

| # | Decision | Kind | Why |
|---|---|---|---|
| D430 | **The rule, which is the second half of a sentence D292 had only written the first half of** | change | D292 says a light path may name **the one cell that STOPPED it** and nothing it merely crossed, and R9i narrowed the request rule to exactly that. What was missing is that naming a cell as *missing* was the only thing a light ray had ever been able to say about it: once built, it went silent, and the pool heard nothing again until six hundred frames later it evicted the thing and the next ray reported it missing all over again. So the keep rule is now the same set as the ask rule — a light ray stopped by **built** matter says it is using it, on the same one cell, and asks for nothing. Stamping, not streaming. **Measured**, close camera 1280×800 settled, against a same-build control (`--no-light-keeps-geometry`): rebuilds within two seconds of an eviction **29,017 → 4,660**, evictions **242,794 → 204,973**, resident 20,368 → 20,746 leaves (+1.9%), feedback 56,008 → 56,288 reports with none dropped. Cost is nothing measurable in the case that hurts: flying and chiselling at 1440p, interleaved, the faces pass reads **7.88 ms mean over five runs (7.81–7.99) against a control's 7.78 (7.46–8.01)**, and total GPU 16.77 against 17.00. 470 tests |
| D431 | **One entry per RAY is not a throttle, and the flood took the streaming down with it** | measurement | The first version reported the occluder from every ray that was stopped by it, throttled by a per-slot schedule so that a node was eligible one frame in sixteen. That bounds the rate per node and says nothing at all about the total, because thousands of rays are stopped by the same brick: at 4K while flying and chiselling it measured **1,538,219 feedback reports against a 131,072 capacity, of which 1,407,147 were dropped**. What gets dropped is not only the new stamps — the buffer carries the *miss* reports that stream geometry, so the cure starves the thing it was meant to help. It reads as a settled-camera success (churn fell) and a moving-camera disaster, which is trap 14's shape from the other direction. The fix is a card-owned word a slot, `node_seen`, exactly as `face_seen` is to the pixel's reads (D414): a node produces at most one entry per window however many rays hit it, and the same 4K run reports **65,505 with none dropped**, against 61,736 for the control |
| D432 | **Two things built, measured and taken out again** | measurement | **A slot-hash pre-gate in front of the seen array**, so the load lands on a sixteenth of the reads rather than all of them. It works and it costs most of the benefit: settled churn **8,651 against 1,028**, because a brick is then only reported if a light ray happens to read it on the one frame it is eligible, and a converged face casts nothing on most frames. It was chasing a +6% on the faces pass that **did not exist** — two interleaved rounds said 8.14 against 7.63, four rounds put the arms inside each other's spread. Not carried. **And stamping what a light ray CROSSES as well as what stops it**, which is the more tempting of the two because a crossed brick that goes unbuilt becomes an occluder itself and re-enters the loop: churn 4,606 → **1,028**, and the faces pass 7.88 → **8.25 ms** flying and chiselling, which is outside a spread that stop-only sits inside. Not carried, and the deciding argument is not the 0.36 ms — it is that "keep everything a shadow ray crosses" is the thing D292 exists to forbid, and the rule that was actually missing was the symmetric one |

**Where that leaves it.** 4,660 rebuilds remain over a settled run against 29,017, and they are the residue of faces that genuinely reopen — a face that stops being seen, is dropped from the gate, and comes back. Nothing about them moves a pixel between consecutive frames on any camera measured. The instrument that would price the next step is already in the audit: `what asked for them back`.

**What the shape of this was.** Four fixes were reasoned about before the instrument existed and three of them were wrong. The lattice was too sparse (it is not: the churn barely moves across three resolutions whose sampling periods differ sixteenfold). The bricks were mostly air (they are not: 276 of 512 against a resident average of 258). The staging ring was racing (D425 built it, measured it, and said so). What was actually true was the thing `touch_slot`'s comment had claimed all along and the code did not do, and **the number that found it was "how many of these had any ray ever mentioned at all" — 99.9%, which is not a hint, it is an answer.** Every earlier attempt had been asking how a report was *used*; none had asked whether one was ever *made*. That is D345's lesson arriving from a third direction, and the class is: **when a signal is suspect, do not measure it against itself — count the events that produced it.**

## An edit relit the whole room, and the lamps never converged again

| # | Decision | Kind | Why |
|---|---|---|---|
| D433 | **What flickers is measurable term by term, and three of the four terms were innocent** | measurement | Reported as *"after playing for a short while the light becomes like squares and flickers rapidly"*, alongside two others about editing and undo. The instrument is the one the debug views already are: photograph two CONSECUTIVE frames of a static camera under `--chisel 60,16` — one edit a second, which is what building feels like — and diff them in each term separately. Enclosed camera, 1280×800, `--settle`, ten frames after an edit: the **sun** (`--debug-mode 16`) is **bit-identical**; sky visibility (17) moves 791 pixels; the ambient near field (18) moves 6,253; **the lamps (20) move 442,227 pixels of 1,024,000 at a mean of 10.59 and a worst of 152**. Never edited, the same pair of the same camera moves 29,469 at 2.19. The audit line settles it without a picture at all: never edited, `lamps on the card: 111,372 of 111,373 live faces cast no more rays at all`; with one edit a second, **`0 of 121,013`** — not one face in the room held a settled lamp term for the whole run. **Three wrong theories died on the way and all three were mine**: that the face store was full (it is not — 813k live against a 1,048,576 budget at the worst camera measured, and `out_of_room` never fires); that the composite was falling back to the coarse stand-in, which would explain the *squares* exactly (it is not — `--debug-mode 16` at edit+1 shows no magenta and no blue anywhere, because the visibility pass reads a face's counters before the shading pass touches them); and that geometry was being left behind (D436). The lesson is trap 16's, one more time: the question that answered it in one line was *which term moves*, and it took four screenshots |
| D434 | **An edit reopens a face's lamp term only where it can stand in a lamp's way** | change | An edit is announced for the box a SHADOW can reach — sixteen metres, `kEditShadowReach` — and every term inside it was reopened on that reach. The near field already had its own much smaller reach for exactly this reason (`kEditAmbientReach`, two metres, because the ray is bounded at one). The lamps had none, so a one-metre chisel stroke reopened the lamp estimator of every face in the room: 121,000 faces restarting from `kLampSeedSamples` and taking `kLampConverged / kLampBurst` = 64 frames to settle, by which time the next stroke had landed. Per face, so it reads as squares; every frame, so it flickers. **The reach a lamp actually has is a geometric question, and it is cheap to ask exactly**: a face's lamp light can only change if the moved geometry stands BETWEEN it and a fitting, so `lamp_path_crosses_edit` slab-tests the segment from the face to each fitting against the edited box — the box itself, recovered by shrinking the announced one, the same way the near field recovers its own. R9g's rule that *a face never loops over lights* is not broken by it: that rule is about the per-SAMPLE cost, which is what makes a hall of a thousand fittings cost what a hall of one costs on every frame for ever. This loop runs on the one frame an edit is announced, over the faces inside it, and is capped at `kLampEditProbe` fittings, past which the answer is the old conservative one. **Measured against a same-build control** (`--no-lamp-edit-scope`; two flags, never two builds, D407), enclosed camera, 1280×800, `--settle`, `--chisel 60,16`: faces holding a settled lamp term **0 of 120,833 → 89,408 of 121,026**; the lamp term between two consecutive frames **242,842 pixels at 4.86 → 36,747 at 2.15**, against a never-edited floor of 27,072 at 1.97; **the shaded picture between two consecutive frames 302,797 at 5.84 → 79,949 at 3.03**, against the same camera's never-edited floor of 70,649 at 2.92. Two frames after a 35,937-voxel placement, against the same scene 120 frames later: **578,934 pixels at 11.42 → 84,023 at 3.33**. **It is also the largest single saving the face pass has had since D414**: 2560×1440 with one edit a second, the faces pass **11.116 → 7.560 ms mean and 17.468 → 11.073 worst**, total GPU **18.788 → 15.332 mean and 25.902 → 18.438 worst**. **And nothing is lost**: 440 frames after a 2,146,689-voxel placement, when both arms have converged, the lamp pictures differ by 28,718 pixels at 2.23 against that 27,072 floor and both report the identical `104,444` converged faces; a never-edited settled run moves 64,851 pixels at 2.729 against a run-to-run floor of 67,348 at 2.807. 470 tests, 18.0 M assertions |
| D435 | **An announcement lowers a face's confidence; it does not erase its answer** | change, measured neutral | `face_accumulate`'s own comment has said since D319 that a contradicted face keeps its ratio and drops to two samples, *"so the composite keeps reading it throughout and no pixel ever falls back to full sun on account of an edit"*. D373's announcement then set `counters = 0` outright, which is the one thing that sentence promises never happens — and a face at nought samples is not answerable, so every pixel on it reads the coarse face three levels up, which indoors is inside the same box and has been zeroed by the same line. That was the mechanism I expected to find behind the squares, and **it is not what a player sees**, because the visibility pass reads the counters before the shading pass writes them: by the time the composite looks, the face already has its first new sample. Measured both ways with `--face-edit-seed 0` as the control arm: two frames after a placement, **571,168 pixels at 11.17 against 578,934 at 11.42** — inside each other. It is kept anyway, at eight samples, for the same three terms, because the invariant is right and because the case it is exactly wrong in is one no screenshot reaches: a face reopened while nobody is looking at it takes no sample at all, so it is still at nought when the camera comes back to it. **D373's own gate is not regressed**: sixty frames after an undo, `--debug-mode 16` against a never-edited run of the same camera is **bit-identical** |
| D436 | **What an undo leaves behind is light, not geometry** | measurement | Reported as *"undo doesn't delete all voxels, only parts of what was there, and chunks of them remain only visually"*. The second half is the testable one and it is **not reproducible**: place 2,146,689 voxels with `--chisel 700,64`, undo at raw frame 800, and the world comes back to the byte — the same content hash as a run that never edited — with `the node pool agrees with the world, leaf for leaf` at the screenshot, no stale leaf, and a worst pixel difference of **35 of 255** against the never-edited control with no block-shaped residue anywhere in the difference image. What is actually left is the lamp term: at undo+60 it differs by **232,516 pixels at a mean of 4.78** against a 27,072-pixel floor, spread as a per-face mosaic over every surface in the room rather than as a ghost of what was removed. An undo of a large box legitimately reopens most of the room under D434's test — a four-metre block really does stand between most faces and most fittings — so the transient is the honest cost of the answer being re-measured, and it is `kLampConverged / kLampBurst` = **64 frames**. **Open, and deliberately not tuned here**: nothing has measured what a larger `kLampBurst` costs, and D394's table is the standing warning that metering a transient makes it worse rather than better. What is NOT explained is the first half of the report — an undo that leaves voxels behind in the world — which no run of mine reproduces, and a player pressing undo once after a stroke that produced several history records would see exactly that |

**The shape of this one.** Three symptoms, one cause, and the cause was in the term I had eliminated first: the lamps converge and go silent, so they were the least likely thing to be moving every frame. What made it findable in four screenshots rather than four sessions is that **each term has a debug view of its own** — D433 is really a measurement of the instruments D310, D330 and D408 built, being used together for the first time. The generalisable rule is the one `kEditAmbientReach` already stated and only the near field had obeyed: **an announcement has a different reach for every term it reopens, and using the largest for all of them is not conservative, it is a permanent transient.**

## A letter is matter, so it is priced in voxels

| # | Decision | Kind | Why |
|---|---|---|---|
| D437 | **The cell is three by five, not five by seven** | change | The typeface was five wide and seven tall for a capital, on the claim that this was the smallest unambiguous Latin alphabet. That claim was about *reading*, and reading was never the binding constraint here: a letter in this game is voxels, so its size is what somebody has to build. **Three wide, five tall for a capital, three for an x-height, one row below the baseline** — six rows and three columns is the whole cell. A capital is **15 cells against 35** and an `o` is **9 against 25**, so a sign is well under half the wall it was. Three by five is the floor for an alphabet carrying BOTH cases, and the floor is where the shapes stop being *distinct* rather than where they get harder to read: at three by four the capitals lose the row that carries B's, E's and S's middle bar; at two wide there is no interior left, so nothing tells an `o` from an `n`. **Four letters are allowed past it and the rule is stateable**: `M`, `W`, `m` and `w` are five wide and `N` is four, because three strokes need two gaps between them and a diagonal with one column to fall through is a horizontal bar — drawn at three, `M` is `H` with a heavier middle and `N` is `H` with the bar moved a row, which is a pair of letters told apart by one pixel in the two words a player is most likely to put on a door. A test holds the list, so it cannot grow by accident |
| D438 | **"Unmistakable" was measured pairwise, and seven letters lost the argument** | measurement | The whole case for three by five is that the shapes stay distinct, and that is a claim about *pairs*, not about any one drawing. So every pair of the ninety-five printable characters was compared cell by cell. **Fourteen pairs came out one pixel apart**, and seven of them were faults rather than conventions: `0`/`U` (a squared ring is the `U` with a lid), `2`/`Z`, `&`/`8`, `H`/`K`, `=`/`z`, `l`/`!` and `u`/`v`. One pixel is not a difference at this size — it is a smudge, a bad screenshot, or a voxel somebody has not placed yet. All seven were redrawn; the zero became the **barred ring**, which is the slashed zero with the slash lying down because a diagonal needs a column it does not have. **What is left one pixel apart is the set where the one pixel IS the meaning**: `.` against a space, `,` against `;`, `.` against `:`, `O` against `0`, and `O` against `Q`. The weaker half of this lives in the suite as *no two characters are the same drawing*; the pairwise distance itself was a throwaway script, because the answer it gives is a redrawing and not a regression |
| D439 | **The face reads markdown, and an emphasis is a property of the glyph** | change | Three pixels across leaves no room to say "this is a heading" by choosing a different face, so structure has to be said with size, weight and marks — which is the vocabulary markdown already carries and what people already type when they want it. Read: headings, bullets nested by indent, numbered items, task boxes, quotations, fenced code, rules, `**bold**`, `*italic*`, `` `code` ``, `~~struck~~`, `[label](target)` and `\` escapes. Each emphasis is drawn rather than substituted: **bold** is the drawing smeared a pixel sideways and one wider (there is no second weight to draw at three across), *italic* leans the rows above the x-height one pixel right (two turns a letter into a diagonal), an underline hangs a row BELOW the descender so it never touches a `p`, a strike runs along the middle of the x-height where the hyphen already is, and `code` is monospaced because a box around a word costs more pixels than the word. Headings spend size while there is size to spend and weight when there is not (`#` ×3, `##` ×2, `###` and deeper bold). **Not read, each on purpose**: tables (a column rule is a third of a letter), images (nowhere to fetch a picture from inside a world), raw HTML, setext headings, reference links. `_` inside a word is not emphasis, so `snake_case_names` survive being written down — that one rule is most of what CommonMark's flanking machinery buys |
| D440 | **The card's copy of the typeface is generated from the font, and checked against it** | change | The interface draws text in a compute shader, so `shaders/ui.glsl` carries the glyphs as well — and a second copy of a drawing is a second place for it to be wrong, in the way that survives a release because it is wrong *only on screen*. The cell is five by six, which is thirty bits, so a glyph is one `uint`: `tools/font-table.ps1` prints the table from `assets/font/00-basic-latin.txt` and a test reads it back out of the shader and checks every glyph and every advance against that source. Two consequences fell out of the same change: the shader's font is **proportional** now, so its text walks the string instead of dividing by a fixed advance (an `i` is one pixel wide and an `m` is five); and the loading screen **stopped upper-casing its labels**, which it only ever did because the shader's face had no lowercase. Its words are set at two interface pixels a glyph pixel, so a capital is ten interface pixels against the seven the old face gave — bigger than before, in two thirds of the ink. 481 tests |

**What the shape of this one was.** The old size was justified in the font's own header, in a sentence that was true about a different question: *"five by seven is the smallest size at which the Latin alphabet is unambiguous"*. It is a reasonable claim about a screen, and the file it was written in draws letters that become **matter** — where the same pixel costs a voxel a player places, and where "buys nothing a reader can use" has to be weighed against thirty-five voxels a letter. The generalisable part is that **an inherited constraint should be re-derived against the constraint you actually have**, and the way to tell them apart is to price it: nobody argues about 5×7 against 3×5 until somebody writes down 35 voxels against 15.

## The shell — a title, docked windows, libraries, and one editor seen twice

*The user's specification of 2026-08-11, written up in full as `23-shell-and-libraries.md`. Two
points in it were ambiguous and were put back to them before anything was written down; both
answers are recorded as decisions here rather than as interpretations there.*

| # | Decision | Kind | Why |
|---|---|---|---|
| D441 | **The game opens on a title, not in a world** | change | `Application::run` builds the facility before the first frame exists and there is no way to not do that. That was right while the only thing to look at was the renderer and it is wrong the moment there is more than one world: `09-performance-budgets.md` §8 has always said *cold start to main menu ≤3 s* and *enter a world ≤5 s*, and those are two numbers about one event until a menu sits between them. It also creates the path `02-architecture-overview.md` requires and nothing has ever exercised — **a world is torn down on the way out, never shared** — because today there is exactly one world per process |
| D442 | **Two buttons on it: worlds, and settings. Nothing else** | user | A title screen is the easiest place in a game to break the first constraint in `14-ui-style.md`, which is *as little of everything as possible while staying legible*. No news, no store, no social, and no "continue" that guesses which world was meant. Leaving is the window's close button and Escape, because a third button spends a third of the interface on the one action every player already knows |
| D443 | **Two families of window — parameters on the left, libraries on the right — and every one of them is docked, resizable and re-dockable to any edge** | user | The side is not decoration: *changing a number* and *choosing a thing* are different tasks, and a consistent side means the eye knows where to go without reading a title bar — the same argument the icons are made of. Docking rules out four failures at once: nothing floats (a game that makes a player manage overlapping windows has handed them a desktop), a window never covers what it is about, two windows on one edge split it rather than stacking, and there is never a "reset layout" to find. Sizes are remembered **per edge**, because the useful width of a library docked left is not its useful height docked top |
| D444 | **Every numeric value is a slider, double-click types into it, and a typed value is not capped** | user | The slider's range is the *useful* range, not the legal one — it is where the handle can go, not where the value can. Asked directly which "no caps" meant, the user answered **no limits on values**. Two boundaries are stated with it so the rule is not applied where it has nothing to say: a value that would genuinely break the game is still refused **and says in one line what it would have done**, because a refusal that does not explain itself is indistinguishable from a bug; and a value that is not a number is not a slider — a choice is icons, a switch is a switch, a name is a name |
| D445 | **A library is a file manager over the real folder in `%LOCALAPPDATA%\WorldShaper\`, not a database with a folder underneath it** | user | The folders are folders and the files are files, so a clip dragged in from Explorer appears and a backup is a copied folder. The consequence is a rule: **nothing in the game may hold state about a file that the file does not carry**, because the file can move, be renamed or be deleted without the game watching. One shelf per kind — worlds, clips, materials, mods, characters, scripts — and the set of kinds is open, because a mod that adds a kind of thing needs somewhere to put it |
| D446 | **Delete goes to `trash\`; bulk select is a dragged box, shift for a range, ctrl to add** | user | A delete a player cannot undo is not the thing they pressed, and this is a library of work somebody made. The selection gesture is Explorer's on purpose: it is the one file manager every player on this platform already knows, and an interface that spends its novelty budget on *selecting things* has spent it in the worst possible place |
| D447 | **Every file carries the name of whoever made it, and the interface cannot edit that field** | user | It travels with every copy: a clip downloaded from somebody says who made it, for ever, in your library and in the library of anyone you pass it to. Not editable from inside the game — not because it cannot be forged from outside, but because a name the interface lets you overwrite is a name that means nothing. It is the same chosen username the invite codes carry (answer J2), so a player has one identity and not two |
| D448 | **The community tab is a live index over the players who are reachable right now, gossiped over the multiplayer ladder, with no infrastructure of ours** | user | Asked how a search over "any player's files" could exist against answer M2, the user answered that it uses the same laws as the multiplayer — free, no hosting, no port forwarding — and browses what any **online** player has **at that moment**. So a peer answers three questions about itself (what have you, what does one look like, give me that one) over IPv6 / LAN / STUN / relay, and the index is gossiped rather than hosted. **Newest first is the default order because it is the only one that is true without counting anything.** Trending — day, week, month, all time — is by downloads, and is honestly an *ordering* rather than a scoreboard: with no server, a download count is what peers claim about themselves |
| D449 | **A file is browsable while its author is online; deleting the original removes it from the browser and never from the copies** | user | Publishing here is not a transfer to somewhere — it is *being reachable while you are running*. A copy keeps working and keeps saying who made it after the author has taken theirs down, which is the only behaviour that does not quietly delete somebody's downloaded work when a stranger tidies their folder |
| D450 | **Everything in a library is offered while you are online, with one switch in settings and a per-folder private flag, both defaulting to shared** | user, with a reservation | This is what was asked for and it is what is built. The reservation is recorded because it is real: *online* and *browsable by strangers* being the same state means a player who has not thought about it is sharing. One switch and one flag is the smallest thing that gives a player who **has** thought about it somewhere to go, without turning the feature into an opt-in nobody uses |
| D451 | **The reach of the community browser is bounded by who you can reach, and the DHT that would widen it is its own decision** | honesty | With no rendezvous of any kind a fresh install knows nobody, so the browser bootstraps from the connectivity ladder and the people you have played with. Answer M1's option (b) — a public DHT — is explicitly *not* infrastructure of ours and is what turns the village into a city; Steam's matchmaking widens it again once the game ships there. Named as its own sub-step rather than assumed, because "browse everyone" and "browse who you can reach" are different products and only one of them is free |
| D452 | **The visual editor and the script editor are two views of one document, and editing either updates the other live** | user | Not an import and an export. `20-clip-forge.md` §8 already states the representation this rests on — *the node array is the real representation; text and wires are two views of it, and either can be saved as either* — so both views read and write that array and neither is the master. The script view is the more powerful one deliberately (expressions, loops, its own functions, and nothing to arrange on a canvas), and the visual view is not a toy under it |
| D453 | **A script that does not parse is not an error; the visual view holds the last graph that parsed and greys itself** | — | The text is re-parsed on every keystroke, so half of every word typed is a syntax error. A parser that interrupts at that moment is a parser a player fights. Nothing empties and nothing pops up |
| D454 | **Anything the visual view cannot draw is an opaque text node carrying its own source, never something dropped** | — | The two views may disagree about how much of a document is drawn as wires. They may never disagree about what the document **is**. A view that silently discards what it cannot represent deletes a player's work the first time they open it, and that is the single failure this arrangement exists to make impossible |
| D455 | **The editor asks for a file before it opens** — it sends you to the library tab with *new* under the cursor | user | There is no editing without something to edit, and an editor that opens on an untitled nothing has to invent a place to keep it, which is how a library grows files nobody chose to make |
| D456 | **The visual view waits for the node editor in Stage 20 rather than getting one of its own in Stage 15** | O19, D66 | There is exactly one node editor in this project, with different node sets for world generation, logic, materials and clips, because a player should learn it once. Building a second one in Stage 15 to throw away at Stage 20 is precisely what the roadmap's ordering rationale exists to prevent. Stage 15's editor tab therefore ships the **script view**, and the tab says in one line what the other half is waiting for |

## Building it — what the first slice found

*The shell as specified above, built. Four things came out of building it that the specification
could not have said, and each is here because it changed the code rather than because it was
discovered.*

| # | Decision | Kind | Why |
|---|---|---|---|
| D457 | **Bold is a whole cell in the world and half a cell on a panel, and the panel's half is drawn at seven tenths of the ink** | correction | D439 said *the drawing smeared one pixel sideways, one wider*, which is right about a letter made of voxels and wrong about a letter on glass. At three columns across a counter is exactly one cell, so a whole-cell smear fills the middle of `o`, `e`, `a`, `g` and `y` and a bold word comes out as a row of blocks. It was legible in the document and unreadable on the screen, which is where it was reported from. On a panel a glyph pixel is four device pixels, so half a cell exists to smear into: the stroke gains half its width, the counter keeps half of itself, and the added half at less than full ink reads as weight on the stroke rather than as a second column of the letter. It degrades to the original rule at one device pixel per glyph pixel, which is the case the world is always in — so the world keeps D439 for a reason rather than by omission. Two further consequences: an emphasis costs no WIDTH on a panel, so `**this**` does not move the rest of a line as it is typed; and a heading drawn *fainter* than the rows under it cancels its own weight, which is what the first attempt did and why the fix looked like no fix at all |
| D458 | **The process owns the window, the card and the interface; an Application owns exactly one world** | change | D441 asks for a world to be torn down on the way out and never shared, and the only way to make that structural rather than remembered is to make a world an OBJECT with a lifetime. So `run_windowed` holds the window, the device, the swapchain, the profiler, the shell and the sound; `Application` holds the world, the pools, the pipelines and the residency, is built when a world is opened and destroyed when it is left. Every pool in a second world is new because there was nowhere for an old one to survive. It also moves the loading screen's own tear-down onto the path where the window is closed mid-build, which previously leaked because the process was ending anyway and now is not |
| D459 | **Escape in a world opens the shell; it does not end the game** | change | It used to be two steps — give the mouse back, then quit — which was right while there was one world per process and nothing to go back to. With a title behind it, a key that ends the game outright is a key that loses a world nobody meant to leave. Three steps now: the mouse, then the shell, then the shell again. Leaving is a button in the window that lists worlds, because leaving is a thing you do TO a world and that is where a world is a thing you can point at |
| D462 | **A cached world is keyed on the source with the author tag taken out, and a world put on the shelf brings its built world with it** | trap | Reported as *worlds do not use the new loading system once you are inside them*, and the report was right about the symptom and could not have named the cause. D447 stamps every file with who made it as it is created — including the copies that seed the worlds shelf — and a built world is cached under a key hashed from the source that produced it. So one comment line changed the key, every world on the shelf missed the cache it already had beside it, and each one rebuilt from cold: **coarse first, at an eighth of the detail, then re-sampled region by region over minutes.** From inside, that is a world made of blocks that slowly resolves, which looks exactly like the view-driven streaming being broken and is nothing of the kind — the node pool was marching correctly the whole time, over a world that genuinely was coarse. The rule that fixes it is the one that should always have been there: **who made a file is not part of what the file builds.** Seeding also copies the built world beside the clip when there is one, so the shelf arrives with the sampling already paid for. Measured on the facility: **8.5 s and eighteen regions still to sharpen, against 0.84 s and no ladder at all** |
| D461 | **Anything a world installs on something that outlives it comes off when the world does, and the tear-down path has a flag that walks it** | trap | Leaving a world crashed, every time, and the report named `ImGui_ImplSDL3_ProcessEvent` and `Window::pump` — neither of which is where the fault was. The developer HUD sets an SDL event hook on the *window*; the window outlives a world now (D458), so tearing the HUD down left a hook pointing into a shut-down ImGui context and the first mouse move on the title afterwards read address 8. It is the general shape rather than the specific bug that matters: **a pointer that used to be immortal because the process ended immediately after it is a pointer with a lifetime now**, and there were three more of them on the same line — the captured mouse, the crash report's camera context, and the platform's text composition, none of which crash and all of which lie. What makes it a decision and not a fix: `--cycle N` plays N frames, leaves the world, shows the title again and exits, so the one path nothing could reach without a hand on the keyboard is now walked by a command. It found this crash on its first run |
| D460 | **The title is photographable from the command line** | instrument | Every measurement in this project is taken by a flag that walks straight past the title (§0), so the one screen the game opens on was the one screen no automated run would ever look at. That is exactly the failure `14-ui-style.md` names for the tool previews: *a shape nobody can photograph is a shape nobody notices has stopped being drawn.* `--title-shot FILE` after `--title-frames N`, and `--title-open worlds\|settings\|both` to put a docked window up first, in the world as well as on the title. Two things fell out of it: `save_image_png` could not read a sixteen-bit float surface and produced a magenta tartan — so neither the loading screen nor the interface had ever been photographable — and a scripted screenshot taken with a window open now comes from the shell's surface rather than the render target, or an interface over a world would be the picture that was never captured |

**What the shape of the first one was.** D457 is the only entry in this document so far that
*corrects* a decision rather than adding one, and it is worth being exact about what was wrong.
D439 was not a guess: it reasoned from a real constraint — at three across there is no second weight
to draw — to the only move available, and it was right that smearing is the only move. What it did
not do was price the move against the size of a counter, which is the same omission the five-by-seven
face was carrying before D437 priced a capital in voxels. **A rule derived for one medium has to be
re-derived against the resolution it is actually applied at**, and the way to tell whether it has
been is to look at the thing rather than at the rule.

---

**What the shape of the specification was.** Two of the sixteen entries above are answers to questions rather
than to a specification, and both were worth the interruption: "any value with no caps" reads as a
typography rule and as a clamping rule with equal force, and "auto published" collided head-on with
answer M2's *no infrastructure of ours, ever*. The rest of the specification needed no interpretation
at all — which is the useful observation, because the parts that did are exactly the two where a
wrong guess would have been discovered late and by a player rather than early and by a question.

## The mark — a picture rather than furniture, and two faults it uncovered

*The logo. It was one drawing with one animation; it is now a seeded combination of animations,
patterns, palettes, arrangements and a live sorting algorithm, with no bounding box. Everything here
was measured or photographed rather than argued.*

| # | Decision | Kind | Why |
|---|---|---|---|
| D463 | **The mark is drawn from a seed, and every choice in it is salted separately** | user | It is the one surface allowed a hue of its own (`14-ui-style.md`), so it is the one place a palette costs nothing — and a picture that is identical on every launch is furniture. A seed is chosen on a press and again after `kLogoIdleSeconds` with nobody touching anything. Salting each choice rather than consuming bits in order means adding a twenty-fifth animation does not move which palette every existing seed picks, and a seed is a thing a player can arrive at twice. What it cannot protect is the choice whose bound changed, and that is stated where the salts are listed rather than left to be discovered |
| D464 | **Nothing about it is ever one choice: two animations and two patterns, blended on their own clocks** | user | The difference between a mark that loops and a mark that keeps becoming something. Counted, the discrete space is **3,981,312,000** — but the number is not the point and the header of `shaders/logo.glsl` says so: the sort's cycle number is part of its own hash, so no seed has a final state at all |
| D465 | **A change is drawn, not cut — twenty-four transitions, and the arriving seed picks which** | user | A press is answered by *a different change* as well as by a different mark. Each transition warps where the outgoing and arriving pictures are READ rather than fading two finished pictures, which is what lets a change be a movement — a slide, a fall, a card turning, a squeeze through a line. The combine is premultiplied: mixing the colours where only one of the two has a mark averages it against a black that is not there, and the change reads as the mark going dark in the middle of itself |
| D466 | **The mark has no bounding box** | user | Reported as *it still seems to be cut at the edges, especially when it stretches*. The command's rectangle is `kLogoReach` times the square the drawing occupies and the card divides that back out, so the drawing keeps its size and everything thrown wide of it is still evaluated. It is paid for in pixels — four times across is sixteen times the area — which was the reason to keep it small while an arrangement was a loop over 144 blocks, and is not a reason any more (D468) |
| D467 | **The sorting runs on the host, and that is not an implementation detail** | change | It was in the shader first, over sixteen slices. A pixel cannot ask what another pixel decided, so a shader can only sort an array it can hold in registers and re-sort from scratch for every pixel — which bounds the slice count to about sixteen and bounds the algorithms to the ones whose comparisons are local, because the state of an insertion sort at step *k* depends on the whole array. On the host it is 160 slices, ten real algorithms, once a frame instead of once a pixel, and **testable**: `tests/test_logo.cpp` runs every algorithm at four hundred seeds, checks each one reports its keys in order when its run completes, checks a partial run never exceeds a whole one, and checks that what comes out is always a permutation. None of that can be asserted about a shader. The answer reaches the card in the buffer the text already travels in |
| D468 | **Every arrangement is written backwards, so it works at the drawing's own resolution** | user | Reported as *make all sorting algorithms and things be based on the actual voxel resolution of the actual 1 and 0 and not on dividing the logo into blocky sections*. The first version cut the drawing into 144 blocks and asked every pixel which of the 144 covered it — which works, and 144 blocks is a resolution, and a coarse one: a block was thirteen voxels of drawing moved as one lump. Each arrangement is now a map from where a pixel IS to where in the drawing it reads, so every voxel goes to its own place and the loop disappears. The solids are ray casts against a turning cube, sphere or cylinder, so their occlusion is exact for free; the plane folds — including the **four-dimensional** one — are a projective map of the drawing's plane and therefore one `inverse` of a three by three matrix. What a backward map cannot do is draw two pieces overlapping, and that is the whole of the trade |
| D469 | **The exchange count is normalised out of the pacing** | — | Selection sort exchanges 159 times and an odd-even network six thousand, so pacing them by exchanges a second would make one a flicker and the other a wait. Every run takes `kLogoSortSeconds` scaled by the seed, and what differs between the ten is the SHAPE of the run, which is the only reason to have ten. The run's length is measured by running it rather than guessed, because it depends on the shuffle as well as on the algorithm |
| D470 | **Both of the things that take the mark apart hold at nought for most of their cycles, and the two are half a turn out of phase** | correction | The first attempt had each up about half the time and both up a fifth of it, and the mark was unrecognisable in the photographs — which is the one thing a title owes a player. It is now measured rather than promised: a test walks five minutes of clock at five seeds and requires the drawing to be exactly the drawing for more than half of it and both stages busy at once for less than a fifth. It is also what makes the two expensive paths free most of the time, because each is skipped while its weight is nought |
| D471 | **A control that writes to a caller's value must say so on the frame it writes, not on the frame the hand comes off** | trap | Reported as *whenever you slide a value on sliders they are reset to the value it had previously* and *settings don't seem to have an effect*, and it was one line. `Ui::number` returned true only on the release — and on the release frame the drag arithmetic no longer ran, so what it handed back was the value it had been given. **Every drag of every slider in the game was discarded**, and the handle springing back was the interface honestly reporting that nothing had changed. The general shape is worth more than the fix: an immediate-mode control whose result the caller copies has to report on every frame it wrote, or the copy is of a number that never moved. `tests/test_widgets.cpp` exists for this and holds the contract at four claims |
| D472 | **A settings window is a picture of the state before it is a way to change it** | trap | The other half of *settings don't work*. `Knobs` opened on its own defaults rather than on what the game was doing, so a player read numbers that were not this machine's and the first row they touched snapped the game to whatever it happened to say. `seed_knobs` fills them from the quality controller, the camera, the swapchain and the overlay at world start, and the level follows the controller while the controller owns it — otherwise the one number on the panel that moves by itself is the one number on it that is always stale |
| D473 | **A scripted title steps its clock by a fixed sixtieth** | instrument | Everything on that screen that moves is a function of the clock, and on the wall clock a photograph of any of it was a photograph of whatever the frame rate happened to be — two runs of one command could not be compared. `--title-frames N` now means N/60 seconds exactly, which is the same rule `--fly` already uses for the camera. With `--logo-seed N` and `--logo-change N` beside it, the mark's animations, its arrangements, its sort in progress and the transition between two combinations are all photographable and all reproducible; without it none of them were |

## The vocabulary — the twenty-four drawings, and the input they sit in front of

*O22's second half, taken off the shelf. The interface icons arrived with the shell and none of them
had been revisited since; the same pass found that what the shell does to the game's input had never
been decided at all, only accumulated.*

| # | Decision | Kind | Why |
|---|---|---|---|
| D474 | **A drawing has no resolution: every icon is a signed distance field, not a grid of cells** | user | Reported as *remake all the UI icons and make them more legible*, and then as *you can make the icons higher res than what they already are and more detailed*. They were thirteen by thirteen booleans, and thirteen by thirteen is a resolution — at the size a row sets an icon on the desktop this is developed on, sixteen device pixels, a cell is one and a quarter pixels. So every diagonal was a staircase, every circle was a lump, nothing could be drawn finer than a cell however good the screen was, and there was **no antialiasing anywhere**, because a boolean has no edge to soften. A distance field is stated in continuous coordinates and sampled at whatever the screen has: one source is a clean shape at sixteen pixels and a detailed one at ninety-six, and an edge's coverage is its distance in device pixels — the one number the grid could never know. Two rules came out of doing it and both are legibility rather than taste: **a stroke is a letter's stroke** (`kIconStroke` is a shade under the eight units a glyph pixel works out to, because an icon lighter or heavier than the word beside it reads as a different interface), and **the stroke has a floor against the sampling**, so an icon shrunk to 0.55 of nominal gets a *heavier* drawing rather than a fainter one |
| D475 | **A press is eased, and the ease overshoots** | user | The countdown a press keeps is linear, and handing it straight to a drawing was the whole of the animation: it snapped to its pressed pose and slid home at one speed. The eye reads acceleration, not position — a thing that stops dead did not move, it was moved. So it leaves fast, comes home slowing, goes about a tenth past home and settles. The overshoot costs nothing and does all the work, because it is the only part of the motion that says something elastic happened. It also has a consequence in the shader: `phase` now runs a little past 1, so every drawing is written to take a small negative motion as a **counter-swing** rather than as a shape turning inside out |
| D476 | **Escape is one press, and what it opens is both windows** | user | Reported as *esc ingame not bringing menus straight up and having to press it twice*, with *esc should bring the settings and the library window at the same time*. It was three states walked through in order — mouse captured, mouse free, window open — so the key everybody presses to reach the settings had to be pressed twice before anything appeared, and what appeared was the library on its own. Both halves are wrong for one reason. Giving the mouse back is not a state anybody asked to be in; it is what opening the menu **costs**, so it belongs to the same press. And the two families are one state, not two: what a player wants when they press Escape is *the menu*, and the interface cannot know which side of it they meant, which is the whole argument D443 makes for the sides existing. So there are two states and no third, and Escape swaps them — closing recaptures the mouse, because a menu you close is a game you are back in. A press in the middle of the screen does the same, because an interface a child who cannot read can use may not require a key |
| D477 | **While the menu is up the game is given an empty input, not a set of exceptions** | user | Reported as *conflicts with controlling settings and moving or doing things ingame*. The windows are a mode, and only the **mouse** was ever asked about — so dragging a slider on the left of the screen also flew the camera, the wheel over a value also changed the flight speed, a digit typed into a field also swapped the tool in your hand, and Z was undo while you were reading the settings. Eight symptoms, one cause, and the shape of the fix matters more than the fix: the game is handed a *zeroed* `InputState` for those frames rather than being asked to remember, binding by binding, who each key was for. A rule stated once cannot be forgotten by the ninth tool. What is deliberately outside it is the developer keys, which are bound to nothing in the world |
| D478 | **A world is a file, a folder of parts and a built copy, and the library has to know that** | trap | Reported as *i deleted the facility folder in worlds and now the facility world has no facility*. A library is a file manager over the real folder (§4), and the real folder holds all three — so the pieces of a building look exactly like an ordinary folder sitting next to an ordinary file, and deleting them leaves a world that opens as an empty sky with nothing anywhere saying the two were connected. Three changes, and each closes a different half of it: deleting a folder a world is built out of is **refused** and the refusal names the world; deleting a world **takes its parts and its built copy with it**, under the trashed name, so putting it back is a rename of things that still agree; and a missing piece **refuses the build in one line** instead of being a warning. That last one is the general lesson: the document was parsed with a hole in it and produced forty cascading complaints about names declared in the file that was gone, so the one line that said what had happened was the first of forty — which is the same as not saying it |
| D479 | **There is no characters shelf** | user | Reported as *characters double as clips and playermodels and are presented as clips you can place in your world*. It held `.wsclip` files — the same format on a different shelf — and what made it a shelf was the idea that a character is a different **kind** of thing. It is not: a character is a clip you can wear as well as stamp, and which of those you do with it is a decision made when you use it rather than a fact about the file. A shelf per use is a decision forced on the way *in*, before the player knows: a figure saved to `clips\` could not be worn and one saved to `characters\` could not be placed, and neither was a rule anybody wrote down. It also cost twice — a duplicate landed in one of them, a search found half of what was there, every operation had two places to look. Anything on the old shelf moves to `clips\` once, because a shelf that stops existing must not take a player's files off the screen with it |
| D480 | **The whole icon vocabulary is photographable** | instrument | The same argument `--title-shot` is made of (D473), one level down, and it is what found four of the six drawings that were wrong. An icon is only ever on screen where some window happens to put it, so most of the twenty-four were drawn by nothing any automated run looked at — and sixteen device pixels, the size that decides whether a drawing survives, was the size nobody had ever checked them at. `--icon-sheet --title-shot FILE` draws every one at four sizes and across five steps of its own animation. What it caught in its first frame: three figures that came out as three dots, five books that came out as a bar chart, two eyes drawn with the wrong half of each arc kept, and — oldest of all — that `DrawList::icon` defaulted `phase` to **0**, which is the fully pressed pose, so every icon *not* on a control had been drawn mid-animation since the shell existed. The header's bin had its lid open and the tick was a tick that had not been drawn yet, and none of it looked like a bug because nothing had ever shown the two poses side by side |
| D481 | **A shelf that nothing is showing is not re-read** | trap | `Shell::frame` called `library_.refresh()` every frame, in the world as well as on the title, with no window open and nothing to draw from it — one `last_write_time` on `%LOCALAPPDATA%\WorldShaper\worlds` per frame, for ever. A stat is cheap; a stat several hundred times a second on a directory a virus scanner is watching is a synchronous call into the file system on the render thread whose cost this process neither controls nor can predict. The frames it was being spent on were every frame of playing |

## The second pass over the shell — a menu, a fold, and a readout in the wrong layer

*All seven reported by the user in one message, and none of them a new feature so much as a place
where the shell had stopped short of what it already implied.*

| # | Decision | Kind | Why |
|---|---|---|---|
| D482 | **A right-click on a selection is the toolbar, at the pointer** | user | Reported as *right clicking files or folders will bring their appropriate actions ... and make it work for multiple files whatever is appropriate like delete instead of having to press separate buttons*. The same items in the same order as the toolbar, so the two cannot disagree about what a library can do — the toolbar is the one a child who cannot read can find, and this is the one anybody who has used a computer tries first. Three rules came out of building it and each rules out something worse: on something already selected it **keeps the selection**, so *delete* over four files is one gesture, and on anything else it selects that one first, because a menu about a thing you did not point at acts on the wrong file; each item **carries the count**, because *delete 4* and *delete* are different decisions and only one needs thinking about; and an item that cannot act is drawn **faint rather than left out**, because a menu whose length changes with the selection is a menu whose items are never twice in the same place |
| D483 | **A world can be entered from inside a world, and entering one closes the interface** | user | Reported as *whenever you click a world even if you're already inside a world you enter it without having to be on the main menu* and *joining a world closes all menus*. The library is the same window in a world as on the title and lists the same worlds, so opening one from inside another is the obvious thing to try and what happened was nothing — going out to the title to come back in was never a step anybody wanted, it was a step the code needed, because the only thing that could open a world was the loop the title runs in. The tear-down is unchanged and is the whole point: the world goes with every pool it owned (`02-architecture-overview.md`), so this is exactly the fresh start opening one from the title is, minus a screen nobody asked for. Closing the windows is the same rule D476's *playing* state is made of: a departure is not a setting, and the windows that chose where to go are over the thing you went to look at |
| D484 | **The performance overlay belongs to the shell, not to the developer HUD** | user | Reported as *the performance overlay renders on top of the windows when it should render behind them*, and the cause is structural rather than a z-order: everything ImGui draws is rendered onto the swapchain **after** the interface has been composited, so nothing in that file can ever be behind a panel. That is right for the developer panel — a tool being used is in front — and backwards for a readout about the world, because a window is in front of the world. Moving the numbers into the shell's own draw list puts it in the interface's compositing order for free, and it now inverts against what is behind it and is lettered in the game's own face like every other mark. The cost is honest and worth writing down: with a window shut and the overlay on, the shell pass now runs where it used to be skipped |
| D485 | **A parameters window folds, and a section can hold sections** | user | Reported as *every parameter and value in settings is organized inside different cascading things like sound, graphics etc, so that it won't show all settings at the same time*. This is the first constraint of `14-ui-style.md` — *as little of everything as possible while staying legible* — applied to a panel that had outgrown it: a fold is how a control can exist without being on the screen. They open **open**, which is the part that needed deciding: a panel that opens folded makes finding one setting a search, and open-by-default with the folds remembered is neither that nor the wall it replaces. One consequence worth stating because it changed the code: the content height is now **measured last frame rather than counted**, since counting it correctly would mean walking the structure twice and two walks of one list is one place for them to disagree |
| D486 | **Every value has a reset, drawn only when that value is off its default** | user | Reported as *make every parameter and value have a reset to default button next to it, if a cascading section has a value inside it that isn't on default it will also have that button ... make space for those buttons*. The space is reserved on **every** row whether or not the button is drawn, because a column that moved when a value changed would jump as it was used. Drawing it only when something has changed is what turns it from furniture into a second statement: an empty gutter down a panel says *all of these are as they shipped*, and a heading carrying one says *something under here is not* — which is the only thing that makes a changed value findable while it is folded away. Defaults are the in-class initialisers of `Preferences` and `Knobs` and nothing else, so there is no second copy to drift; the one number that disagreed with the game was `field_of_view`, which said 70 while the camera shipped at 90, and a reset to a value the game never had is a button that breaks a setting |
| D488 | **The library's toolbar is gone; a right-click is the only way to reach an operation** | user | Reported as *remove the buttons that right click replaces now*, one message after the menu was built. Six of its seven drawings — new folder, new, rename, duplicate, delete, sort — are what the menu now puts under the pointer, on the thing pointed at, with the count in the label, and two ways to do one thing is two places for it to be wrong. It also gives a row of the listing back. **Up stays**, because it is not an operation on a selection: it is *where you are*, it belongs beside the breadcrumb that says so, and there is nothing to right-click to reach it — a folder you are inside is not an entry in its own listing. The cost is named rather than waved away: this trades a control a child who cannot read can *see* for one they have to be shown, which is a real step away from `14-ui-style.md`'s second constraint, and the mitigation is that the breadcrumb row answers a right-click too, so the folder's own menu has a target that exists even when the listing has no empty space left in it |
| D489 | **A drag with no button held is not a drag** | trap | Reported as *whenever you open the windows on a world for the first time it registers as if you were dragging a selection box in the right window*, and it was D483 biting back within the hour. Every control that takes `active_` gives it back when it sees the release — and a control that stops being **drawn** never sees one. Opening a world from the library closes the windows on the press, so the release of that very double-click arrived with the listing gone, and the rubber band that press had started stayed active for the rest of the process: the next time the windows came up, a selection box was already being dragged by a mouse with nothing held down. The fix is one line in `Ui::begin` and it is a general invariant rather than a patch for the band — any immediate-mode interface where a control can vanish mid-gesture has this hole, and the only thing that closes it is checking the button rather than trusting the control to tidy up after itself. Closing the windows now also puts down the keyboard and any open menu, for the same reason: under D477 a caret that outlives its own row leaves the game deaf to every key with nothing on screen to explain why |
| D490 | **A world that comes up empty says so on the screen, and a shelf says what it found in the log** | trap | *My facility world is still empty* and *I don't have the worlds* were reported three times across two sessions, and every time the answer was somewhere a player has no reason to look. "The world is empty" was a log line, so what the player got was a sky with nothing in it and no way to tell that from the renderer having broken; it is now one sentence at the bottom of the screen carrying the **first** error, because the rest of any list of them is what the first one caused. And the library logged nothing at all about itself, so the first question every one of those reports needed — *is the game looking where I am looking, and did it see what I see* — could not be answered without a directory listing taken by hand. It now logs the shelf, its **full path** and its count, once per shelf opened. The path is not decoration: `%LOCALAPPDATA%` is `AppData\Local` and `%APPDATA%` is `AppData\Roaming`, a player checking the wrong one finds nothing, and concluding from that that the game has lost their files is correct reasoning from what they were shown |
| D487 | **A missing include ends the document, and the rule lives where the splice does** | correction | D478 put it in `load_clip_script`, and that is not the only caller: the application splices a world's source itself so that the cache key covers the whole assembly, so the world-loading path — the only path a player is ever on — still parsed a document with a hole in it. `expand_includes` now returns nothing when any include failed, which is the one place both callers go through. The general shape is the lesson and it is the second time this document has recorded it: **a rule stated at one of two call sites is a rule the other one does not have** |

## Where a player's files actually live — the pass that took four folders away

*Six of these delete something. That is the shape of the whole round: almost every one is a place
where the game had built its own version of a thing the player's computer already had, or kept two
of something that should have been one.*

| # | Decision | Kind | Why |
|---|---|---|---|
| D491 | **A delete goes to the system's recycle bin, and there is no `trash\` of our own** | user | D446's requirement was right — a delete a player cannot undo is not what they pressed — and its implementation was a second recycle bin. Every player already has one: they know where it is, they know how to empty it, it is where every other file on their machine goes, and it survives the game not running. Ours was a second place to look, a second thing to explain, and a shelf that silently filled with things somebody thought they had thrown away. A delete that **cannot** be recycled is refused rather than done, because "could not put it where you can get it back" and "threw it away" are different answers and only one of them was asked for |
| D492 | **There is no scripts shelf; loose Lua is a mod that is not finished** | user | Asked as *what exactly is a script and why is there a library of that*, which was the right question: `.wslua` appeared exactly once in the entire codebase, in the line declaring the shelf, and `mods\` was already specified as *Lua and native packages*. Two shelves, one format, told apart by how finished the file was rather than by what kind of thing it is — the same mistake as characters and clips (D479), found the same way. The mods shelf lists both spellings and the old folder's contents move once. The word "script" meaning two unrelated things in one project is the thing that hid it: a **clip script** is the declarative description a world IS, and it lives inside `.wsworld` and `.wsclip` on other shelves entirely |
| D493 | **A world is one file, and what this machine built from it lives under `cache\`** | user | Reported as *why are there multiple files for the same world, a world should be just one file*, with a screenshot of a shelf showing three. A library is a file manager over a real folder, so everything in that folder is on the screen — and the `.world` and `.load` beside each world are not the player's work, are worthless on any other machine, and were nineteen megabytes sitting next to a five-kilobyte file. They are keyed by a hash of the world's full path now, so two worlds called `house` in two folders are two caches rather than one they take turns evicting |
| D494 | **What the game ships with stays with the game and is never copied** | user | Reported as *have a section on the game files (not appdata) for built in clips ... these clips always show on the clips library without having to be on the appdata folder*, and it is the answer to a bug reported three separate times. The facility's twenty-two pieces were copied into the player's worlds folder, where they looked like an ordinary folder, and were deleted — three times — each time leaving a world that opened as an empty sky. Copying them there was never necessary: the shelf lists the shipped folder alongside the player's own, and an `include` that is not beside its own file falls back to the shipped clips. **Beside always wins**, so a player who copies the parts in and edits them gets their edits. Built-in entries can be opened and duplicated and not renamed or deleted — a duplicate lands on the player's shelf, which is what makes *duplicate* the way to edit something the game shipped |
| D495 | **There is no switch for offering your library** | user | Reported as *make the offer my library while online option be nonexistent, all players must offer their libraries*. D450 kept it for the player who had thought about it; what it actually was is a switch that ships on, that nobody turns off, and that every peer has to be asked about — whose only real effect is a community browser that is sometimes mysteriously empty. *Online* and *browsable* being one state is what makes §5b work without a server at all: being reachable **is** the publishing, and a flag that can contradict that is a second source of truth about who is offering what |
| D496 | **One settings file, written whenever it changes** | user | Reported as *make the game save the settings you have on an always single changing settings file*. There were two of the shell's own — `shell.txt` and `layout.txt` — beside the renderer's `settings.txt`, so "my settings" was three files a player had to know the names of and a reset had to remember all of. And all of them were written **on the way out**, which in a game that ships a crash handler means the settings of any session that crashed were thrown away. It is one file now, compared rather than flagged, at most once a second — so dragging a slider is one write when the hand comes off rather than sixty a second while it moves. The old two are read once and swept up |
| D497 | **Settings has a data section: where your files are, and one button that takes them all away** | user | Both belong in the interface rather than in a document. A folder somebody has to be told the path of is a folder they cannot find — and this project has now spent three exchanges on `AppData\Local` versus `AppData\Roaming` — and a reset somebody has to do by deleting files by hand is a reset they will do wrong. *Reset everything* takes **two presses**, and the second one is the decision: the button changes into a different button that says what it is about to destroy and goes back on its own after a few seconds, which cannot be pressed by accident and cannot be pressed by habit either. The files go to the recycle bin, by D491, because a "reset everything" that is genuinely unrecoverable is a button nobody should be offered |
| D498 | **Text that does not fit travels to its end, waits, and jumps back** | user | The one thing in this interface that moves without a press, and the rule's own reason rather than an exception to it: motion means *this happened*, and what has happened is that the name does not fit. It does **not** loop — a loop has no beginning, so a name you glance at is one you have to watch a whole cycle of before you know where it started. A hold at each end and a cut between them means what is on screen is either the start of the name or the end of it, and both of those can be read on their own. Only the row under the pointer moves, which makes it the same *asking* a tooltip is, answered without words |
| D499 | **The world you are standing in is bold in the listing** | user | The library looks the same from inside a world as it does from the title, so the one question it could not answer was the one most likely to be asked from in there. Weight rather than a wash or a mark, because it is a property of the row's own name rather than something that has happened to the row |

**Three traps closed in the same pass, none of them worth a decision of their own.** A menu is drawn
last so that it sits over what it is about, which means the listing underneath had already
hit-tested the very click that was choosing an item — so an item over a row re-selected that row and
an item over empty space cleared the selection, and the item then ran on nothing or on somebody
else. The menu now owns the pointer while it is up and holds what it was opened on. **It took two
goes**: gating the rows left the *rubber band* still listening, and a press on a menu item is a
press inside the listing, so the band started on it, cleared the selection and re-picked whichever
row its own zero-height box happened to touch. Reported the second time as *the right click menu
affects the last thing on the list*, which cost a player two files. The lesson is not about bands:
**when something is drawn over a region, every reader of that region has to be told, and finding
one of them is not finding them all.** A row typed into
stayed a field until enter or escape, so a slider quietly stopped being a slider; a press anywhere
else now puts it back, as escape rather than as enter. And crash reports went in the data root
beside the six things a player has reason to open, a hundred and fifty of them; they have a folder,
and what was already there was swept into it.

**One correction.** D485 said sections open **open**, reasoning that a panel that opens folded makes
finding one setting a search. Reported as *make all setting cascading things be closed whenever you
open the settings window*, and looking at it settles it: what a settings panel opens as is its answer
to *what can I change here*, and five words with five triangles is that answer where four screens of
rows is the question asked again in more detail. It folds them every time it opens, not only the
first.

## Open items carried forward

- **O21.** Link to the deprecated WorldShaper repository (UI style reference only).
- **O22's first half is still open.** The application icon — `assets/icon.png` and the `.ico` built
  from it — is still a hand-drawn copy of the same 160×160 drawing the mark comes from, and the
  obvious move is still unbuilt: render one *pinned* combination of the real mark to a PNG and build
  the `.ico` from that, so the two cannot drift apart. The twenty-four interface icons, O22's second
  half, are D474–D475 and D480 above.
- **The mark's slices are one voxel wide, which at the title's size is under two device pixels.** A
  heavy scramble therefore reads as fine streaks rather than as bars sliding past each other. That is
  the direct consequence of D468 being what was asked for; `kLogoLanes` is the one lever, and nothing
  has measured whether a player prefers thicker slices to full resolution.
- **The two songs in `songs/` are committed and unplayed**, with no licence recorded. See
  `08-tech-stack-and-licensing.md` §4 for what has to happen before they can ship.
- Round-1 and round-2 questions are otherwise closed. Round-3 questions are raised per stage as they arise; **Q1 and Q2 (the shell) are asked and answered** in `01-open-questions.md` and carried here as D444 and D448–D451.
- **Open, and named rather than assumed:** whether the community browser gets a **DHT rendezvous** so it reaches strangers rather than only the peers you can already get to (D451). It is answer M1's option (b), it is not infrastructure of ours, and it is the difference between a village and a city.

## The light that turned into flickering squares — the door D434 did not close

*Reported as* **after playing for a while the light turns blocky and starts flickering rapidly and
randomly**, *with a photograph of the enclosed room. It is D433's symptom word for word, and D433 is
closed; this is the other way into the same state, and it needs no edit near a lamp — only a player
who moved between two edits.*

| # | Decision | Kind | Why |
|---|---|---|---|
| D500 | **A list of lamps is identified by the set, not by the order the camera happens to rank it in** | change | `build_light_list` ranks fittings by what each would deliver **at the camera**, so walking two paces returns the same lamps permuted. `light_list_hash` ran over the records in that order, `update_lights` treats a changed hash as *the lamps changed*, and the host answers that by setting `light_reset` for one frame — which reopens the lamp term of **every face in the store**, not one face. So any announced world change made after the player had moved relit the whole room: every face dropped to `kLampSeedSamples` and re-burst for `kLampConverged / kLampBurst` frames, and the next stroke landed first. Per face, so it reads as squares; every stroke, so it flickers. The hash now runs over a canonically ordered copy — by the record's bytes, which is a total order over a 28-byte POD with no padding and is the same bytes the hash then covers. Ordering was defended in the header as catching *a change that happens to be a permutation*; a permutation of the same records is the same set of lamps, and rank only decides which survive the `kMaxLights` cap, which changes the set and therefore still changes the hash |
| D501 | **The instrument was already printed and nothing had been pointed at it** | measurement | `lamps on the card: N of M live faces cast no more rays at all` is D403's audit line and it names this fault in one reading, with no picture and no theory. Facility, warm cache, 1280×800, `--chisel 60,16`, 600 frames, one build: **static camera — one version bump, `469,861 of 507,251` silent; the same nine strokes while flying — nine version bumps, `0 of 997,296`.** Nothing converges, so every face is re-measuring every frame. After the change the flying arm is **one bump and `264,456 of 995,684`**. Trap 16 again, from the other end: the question that answered it was not how the announcement was *used* but how often it was *made* |

**Two things found on the way to it, both measured, neither fixed here.**

- **Opening a world re-samples the clip; it does not load a built world, until one exists.** The
  facility's `.world` cache is written by `save_refined_world` at refinement's fixed point, and the
  facility had never reached one — eighteen regions, of which the late ones cost 7–26 s of sampling
  apiece. So every launch re-ran the ladder, and **every region paste blocks the main thread**:
  measured on this machine at **1.4, 6.5, 7.0, 13.0 and 14.1 s** in single frames. Once the cache is
  complete the same world **loads in 119 ms with no pastes at all**. This is §5's second item in
  `22-rewrite-handover.md` — *slice the paste across frames*, D74's Stage 16 — and it is what a
  player is reporting as massive hiccups. It is also half of why the light flickered, because every
  paste is an announced world change and therefore a light-list rebuild.
- **`--clip-coarse` is part of the world cache's key, so switching it deletes the other's cache.**
  The key is hashed from `script.settings.voxels_per_metre`, which line 2457 of `src/app/main.cpp`
  has already **divided by `coarse`** by the time the key is computed. A 608 MB full-detail cache
  written by `--clip-coarse 1` was discarded as stale by the very next default launch, which then
  rebuilt from cold. Harmless to a player, who never passes the flag, and a trap for anyone
  measuring loads: the arms of a `--clip-coarse` sweep destroy each other's caches.

## The blocky flicker itself: a full face store, and the coarse stand-in that fills the hole

*Reported again after D500 with two clarifications that closed it — **"the bug still happens even
when you haven't placed or erased a single voxel"** and that it takes a while to appear. Neither is
consistent with anything edit-driven, and both are exactly what a table filling up looks like.*

| # | Decision | Kind | Why |
|---|---|---|---|
| D502 | **Reproduced first, at a forced budget, because a table that fills after minutes of play cannot be argued about** | measurement | `--face-budget 120000` puts the store in the state a real session reaches, in ninety seconds. The picture is the reported one — the whole facade and steps in hard blocky patches — and the flicker is a number: **two consecutive frames of a STATIC camera with no edits differ on 231,409 pixels of 1,390,170 at a mean of 6.44**, against **56,284 at 1.55** with room to spare. The chain is all in existing comments and had never been put together: a refused claim leaves a pixel with no fine face *and* no host stand-in, so `node_visibility.comp` falls to the **card's provisional** coarse face — which by construction re-claims itself and takes **one fresh sample every frame**. One ray per 8³-voxel block per frame is a blocky picture that is completely different next frame |
| D503 | **The eviction floor was reasoned from convergence time and belongs to the request lattice** | trap | `kFaceMinCold` was 32, "far above the handful of frames a face needs to settle". True, and about the wrong quantity: `last_read_` is stamped by a CLAIM, and a claim comes from the one pixel in stride² the moving lattice is asking with — 64 frames at 1440p, **256 at 4K**. Below that, "nobody has asked for this face" means "the lattice has not got round to it". The emergency sweep halved its window down through 300, 150, 75, 37 to **18** and evicted the wall the camera was pointed at, which came back as an empty record. The floor is `2 × claim_period` now, set every frame from the render resolution, and `kFaceMinCold` is kept only as the note explaining why it was wrong |
| D504 | **The store gives history up before it is full, rather than refusing a claim when it is** | change | A refusal has no graceful form — there is no window short enough to make a surface with no face come out right — so the table must never reach one. Under an eighth free the window drops to a quarter of itself and under a sixteenth to the floor. **Measured, two flags of one build**: spinning in place at `--face-budget 600000`, **75,421 refusals → 0** with the live count unmoved (410,664 → 410,617); at the real budget, flying, **771 and 139 → 0 and 0** over two interleaved rounds |
| D505 | **Where it starts is a measured trade, and the first setting made the game slower** | measurement | Starting at *half* free removed every refusal and cost the faces pass **1.918 → 8.086 ms** flying at the default budget: a shorter window also gives up faces whose coverage is under a pixel — which the lattice samples rarely however plainly they are on screen — and each pays its whole ambient burst again on return. At an eighth the two arms interleave inside each other's spread (faces 1.913/2.771 off against 1.678/2.990 on; total GPU 5.641/6.504 against 5.450/6.845) with refusals still at nought |
| D506 | **A cursor is not a fact about pressure, and a sweep that follows one reports finding nothing** | trap | The rolling eviction sweep walks an eighth of the table a frame. While the table is still growing that cursor sits at the young end, so it sweeps only the faces just claimed — every one younger than any window — and gives up nothing while the table fills behind it. Measured in the headless case: window already at the floor, half the table swept per frame, **nought evictions until the cursor happened to wrap**. Under pressure the sweep starts at nought and covers everything |
| D507 | **A refusal is counted, and the audit says how full the table is rather than how many faces are in it** | instrument | `out_of_room()` is a state, so it answers "is it full *now*" — and a store that fills and recovers sixty times a second reads as fine at every moment anybody asks. `FaceStoreStats::refusals` is the harm itself: a claim that got no slot is a surface that got no light of its own on that frame. The line reads `N live of M … R REFUSED, cold window C frames (floor F)`, because a count with nothing to measure it against cannot show a store that is nearly full — which is the state that produces the picture and the one nothing was reporting. Trap 16, and D425's instrument argument reused rather than rebuilt |

**Open, and it is the proper fix rather than this one.** `last_read_` is stamped by the lattice's
claim. The card already stamps `face_seen` for every face every pixel resolves to, **every frame**
(D414) — and residency was deliberately left out of that change. That is the same correction D427
made for nodes and D430 for light rays, and it is what would let the window be genuinely short
without giving up anything anybody is looking at, which is the whole of D505's trade. The cheap
shape is D431's: gate the report on `face_seen` so a face reports itself down the feedback buffer at
most once per window, which is about eight hundred entries a frame against a capacity of 131,072 —
no readback, one entry per face per window however many pixels read it.

## Residency hears the screen, the trade it was supposed to unlock, and the instrument that had to exist

| # | Decision | Kind | Why |
|---|---|---|---|
| D508 | **A face a PIXEL read says so, once per face per window, and that is the store's residency clock** | change | `FaceStore::last_read_` was stamped by a CLAIM, and a claim comes from the one pixel in stride² the request lattice is asking with — so the store's idea of "somebody is looking at this" was really "the lattice got round to asking", which a face covering less than a pixel goes many periods without. `face_read` is a card-owned word a slot, exactly what `node_seen` is to the node pool (D431): a face reports itself down the feedback buffer at most once per `face_read_period` frames however many pixels are on it, and the consumer's whole answer is `FaceStore::touch`. Two gates, and both are needed — a **phase** so a cold store does not report half a million faces into a 131,072-entry buffer on one frame, and the **array** so the thousands of pixels sharing a face on its eligible frame produce one entry between them. The phase is not D432's mistake: the reporter there was a light ray from a face that casts nothing on most frames, and here it is the primary ray, which runs for every visible pixel on every frame |
| D509 | **It bought the correctness and not the trade, and the prediction was wrong** | measurement | It was built to pay for D505 — the idea being that squeezing the store cost 1.918 → 8.086 ms because eviction was blind to faces whose coverage is under a pixel, and that an exact signal would make a small store cheap. It does not. Flying at 1866×745 with the squeeze turned up (`--face-pressure-from 2`), **reads off: faces 7.108 ms, 512,725 live; reads on: 7.181 ms, 523,874 live.** The cost of holding less history is the re-burst of faces that genuinely left the screen and came back, and no residency signal can make that free. So D505's setting stands on its own measurement and this changes nothing about it. Kept anyway, and the reasons are worth separating from the number: "cold" now means what it says, the eviction floor stops scaling with resolution (128 frames rather than 512 at 4K), and the volume is nothing — 740 to 5,271 entries a frame, 63,822 of 131,072 with none dropped. `--no-face-reads` is the control arm |
| D510 | **The store says it is turning faces away while somebody is PLAYING, in the log, not at a screenshot** | instrument | Every number about this store was printed at the screenshot audit and nowhere else, so the one state that produces the reported picture was invisible in exactly the situation it gets reported from — and three rounds of this fault have now been diagnosed from repros of mine rather than from the session that was complaining. A player can play until it looks wrong, quit, and hand over `worldshaper.log`; that log has to already contain the answer. A rate-limited warning names the refusals and the fullness, and a heartbeat every six hundred frames prints the store's state whether or not anything is wrong — because "the log says nothing" and "the log says it was fine" are different answers and only the second is evidence. Trap 14, one level further out |

## The paste that was not pasting — a stall measured against the wrong suspect for two sessions

*`22-rewrite-handover.md` §5 has opened with* **slice the paste across frames** *since it was written,
on measured evidence: region pastes of 1.4, 6.5, 7.0, 13.0 and 14.1 s with the main thread blocked,
which is what a player reports as massive hiccups on a first load. The slicing was never built. It
is now not the fix, because the paste was not doing the work.*

| # | Decision | Kind | Why |
|---|---|---|---|
| D511 | **Split "pasted N ms" into its three parts before slicing anything, because it was three quantities in one number** | instrument | The one figure covered `paste_clip`, the op-log replay and `announce_world_change`, which scale with three different things — the box, what the player has done, and the box in bricks. Split, on a cold facility: **replay 0 ms and announce 0–2 ms in every one of twelve regions**, and all of it `paste_clip`. That alone would have justified the split, but the useful part was the column beside it: paste time **did not track the paste**. The same 991-brick region pasted in **146 ms and in 7,076 ms**; 5,359 bricks went in 80 ms while 4,258 took 1,453. No property of the output explains a 47× spread on identical output, and that is what said the cost was not in this function |
| D512 | **It was the job system, and paste time was the time of the SAMPLE running beside it** | measurement | Every row lined up against the *next* region's sample: paste 146 next-sample 503, paste **7,076** next-sample **7,213**, paste 6,527 next-sample 7,228 — and the last region, the only one with no sample running beside it, pasted in **75 ms**. The mechanism is in `JobSystem` and is exact. `parallel_for` does not queue a slice of the range, it queues a take-LOOP over it, so a worker that picks one up stays inside it until that submitter's whole range is consumed. `pump_refinement` starts the next sample *before* pasting (deliberately — D-era overlap, and it nearly halves the ladder), and both the sampler thread and the paste were handed `refine_jobs_`. So every worker was inside the sampler for the length of the sample, the paste's own entries sat behind them, and `wait()` — which helps with queued work so a waiting thread is never idle — handed the **main thread** the sampler's jobs to run. The paste was not slow. The main thread was doing the background sample |
| D513 | **Foreground work and background work do not share a queue** | change | `paste_jobs_`, a second pool, sized like any foreground pool rather than like the sampler — which is deliberately held to half the machine because it runs while somebody is playing. The paste is not background work: it is the frame the player is sitting inside, it lasts about a tenth of a second, and for that tenth of a second it is worth the brief oversubscription. **Measured, two flags of one build** (`--no-paste-pool` is the control), cold facility, `--no-clip-cache`, 1280×800, default camera: worst paste **7,282 → 92 ms**, the twelve pastes together **34,697 → 719 ms**, and frames drawn before the world settles **453 → 5,439**. The sample beside it pays **1–3%**. The world is untouched by it, which is checked rather than assumed: both arms settle on 108 chunks, 126,514,044 solid voxels, 12 of 18 regions and content hash **1f4710eee4ee2585** |
| D514 | **The pool counts the collision and says what it costs, because nothing anywhere said it was happening** | instrument | Two submitters on one pool is not illegal and reads as harmless; what it does is turn one thread's wait into another thread's whole workload. `JobSystem::submitter_collisions()` counts it and a one-shot warning names the consequence rather than the state. A **count** and not a flag, for D507's reason exactly: "are two threads in there now" is a state, and a pool that collides for four seconds of every seven reads as idle at whatever moment somebody asks. Checked the way trap 15 says to — the control arm **must** fire it and does, the fixed arm must not and does not. The headless gate is `tests/test_jobs.cpp`, *a pool with two submitters says so, and two pools do not collide*, whose overlap is arranged rather than raced for: the first submitter's own body refuses to finish until the collision is recorded, bounded so a broken detector fails the case instead of hanging the suite. 546 tests, 18.7 M assertions |

**What this does to the work item it came from.** §5's *slice the paste across frames* was sized
against 14 s stalls that no longer exist; what is left is **31–92 ms**, which is two to six frames
and not a hiccup anybody has reported. Slicing it is a large change with real hazards — a
half-pasted world is visible to `save_refined_world`, to `--settle` and therefore to every
measurement in this file — and it should not be built against a premise that has moved by 79×.
**It is not done and it is no longer the first thing.** D74's Stage 16 argument still stands for
the case it was written about, which is the edit: a 36-million-voxel chisel is still **one frame of
1,209 ms**, that is a different path with a different cause, and it is measurable on demand rather
than by watching a load.

**The general lesson, and it is the one worth carrying.** Four numbers in this file's §5 were
attributed to the paste for two sessions. Every one of them was true, none of them was about the
paste, and the thing that gave it away was not a profile — it was **printing the suspect's cost
beside the cost of what was running at the same time**. Trap 16 says that when a signal is suspect,
count the events that produced it; this is the same rule one step out. A cost that does not track
any property of its own output is not a cost, it is a wait, and the question to ask of a wait is
*who else is running*.

## An edit announced a volume to a tree that is not stored by volume

*The other half of §5's second item, and a different fault from D511 with the same shape one level
down: a cost that was O(what the edit COVERS) where the answer is O(what the pool HOLDS). Found by
splitting a frame the same way the paste was split.*

| # | Decision | Kind | Why |
|---|---|---|---|
| D515 | **The edit refresh descends the pool's own tree and prunes to the box, instead of being handed every brick in it** | change | `announce_world_change` enumerated every brick in the edited box and called `NodePool::invalidate` on each; `update` then deduplicated that list into a set and walked UP from each brick to find the ancestors to re-fold. Upwards means starting from bricks that mostly do not exist. Measured on the 36-million-voxel delete of §4b: **1,573,269 bricks announced**, of which the answer was **13,325 nodes to refold and no leaves rebuilt at all** — and finding that out cost **718 ms in one frame**, split as **gather 457 + descend 257 + fold 4**. Four milliseconds of purpose behind seven hundred and fourteen of bookkeeping. The pool holds the tree and the caller does not, so the caller now hands over the **box** and the pool descends from its own roots, pruning any child whose extent misses it. Post-order, which also removes the sort the flat version needed: a fold reads its children, and children are refreshed before their parent by construction. **718 → 7 ms**, and the edit frame stops being the worst frame in the run at all (node-pool CPU worst **890.7 ms on the edit frame → 26.2 ms on frame 15**, which is startup). A single-voxel chisel is untouched — one root, one chain, the same eleven nodes — because `invalidate` is now `invalidate_box` over one brick and there is one path rather than two |
| D516 | **A child mask is the field nothing compared, so it has an audit now** | instrument | This rewrite changes precisely *which* nodes get their mask re-derived, and nothing in the engine could have told a correct answer from a plausible one. `stale_leaves` (D400) compares what a ray SEES; a child mask decides where a ray is ALLOWED to look, and both ways of being wrong are silent — a bit set over nothing is a phantom request every frame for ever (D133), and a bit clear over something is geometry no feedback will ever ask for, because feedback reports only what a ray could not find. The GPU mirror cannot see either: it compares the pool against the card, and both agree perfectly about a bit that is wrong in each. `NodePool::stale_masks` walks every built node and compares its mask against the world, the screenshot audit prints `the node pool agrees with the world, mask for mask` beside the leaf line, and the headless gate is written the way trap 15 says — the arm where nobody tells the pool **must** report a stale mask, and does, or the clean arm proves nothing. `tests/test_node_pool.cpp`, *an edited box refreshes every mask under it, and an unannounced edit does not*, plus *announcing a large empty box costs what the pool holds, not what the box spans*, which asserts the cost claim as **work** rather than as wall clock so it cannot flake on a busy machine. 548 tests, 18.7 M assertions |

**Checked, not assumed.** Same camera, same scene, `--edit "-600,96,-600,600,640,600,0"`: content hash
**81d1ee684da20d7f** before and after, `the node pool agrees with the world, leaf for leaf` and
`mask for mask`, `GPU mirror matches: 282,001 nodes, 185,552 leaves`. On an unedited world the
default camera is unchanged at content **1f4710eee4ee2585** with both audits clean. One thing that
looks like a difference and is not: the `scene:` line reads 108 chunks under `--no-clip-cache` and
74 through the cache, on the identical content hash — a cache round trip drops 34 empty chunks, it
predates this change, and it is why the hash is the thing to compare rather than the chunk count.

**This is a two-build comparison and that is defensible here**, which D407 is otherwise the standing
warning against. The reason that rule exists is the faces pass, whose cost depends on a convergence
state no `scene:` line shows. This is deterministic CPU work on an identical world, the effect is a
hundredfold, and the arms were run minutes apart on one machine — but it is worth saying out loud
rather than leaving somebody to assume a control arm existed.

**What a large edit costs now, and what is left.** The 36-million-voxel delete was quoted in §5 at
one frame of 1,209 ms. Measured here it was 258 ms of op plus 718 of pool refresh; the refresh is
now 7. **The undo capture is what is left and it is now the largest part**: apply 62 ms, undo
capture **194 ms** into 538,169 inverse ops. That is the next thing in this frame, and it is D74's
Stage 16 argument arriving at its real target at last — an inverse op per changed run, captured
synchronously, for an edit nobody may ever undo.

## R3d — the per-pixel light path is deleted

*§9 of the plan has said since it was written that `pathtrace.comp` goes at R3. It has now gone.
3,297 lines deleted against 52 written.*

| # | Decision | Kind | Why |
|---|---|---|---|
| D517 | **The reference path tracer is deleted, and the descriptor set it owned is kept because the cloud pass was always sharing it** | change | Gone: `shaders/pathtrace.comp` (2,757 lines), `shaders/pt_normals.glsl` (293), the rgba32f accumulator and its barrier, the camera-moved test that existed only to reset it, the old world-space face cache's *use*, `--pathtrace`, F4, and the `path_trace_` branch in every place it reached. **Kept and renamed in comment only: `pathtrace_layout_` / `pathtrace_set_`**, because `clouds_.create(...)` is passed that layout and the comment at its construction always said so — *"the cloud pass needs the parameter block and the sun and nothing else, and a set of its own would be the same set with holes in it"*. Trimming it to what the cloud pass names is follow-on, not part of deleting the pass: the binding numbers are ones the shaders and `gpu/render_params.hpp` agree about. Same reason `face_cache_` is still allocated and bound while nothing reads it |
| D518 | **Validation caught the one real fault, which was a count and not a binding** | trap | Removing the accumulator's descriptor meant renumbering the `writes[]` entries after it — and the two `write_count` values a few lines below, `10` and `13`, are literals that do not renumber themselves. `vkUpdateDescriptorSets(): pDescriptorWrites[9].dstSet is VK_NULL_HANDLE`. It would have been a silent read of whatever the pool held on any machine not running `--validation`, which is every machine a player has. **Trap 1 says run `--validation` when a pipeline is new; it is equally the rule when a descriptor set loses a member**, and an array whose length is written as a literal beside it is where that bites |

**Measured.** Warm start is unchanged — 538 / 555 ms after against 551 before, same camera, same cache. The cost the tracer was carrying is on a **cold driver shader cache**: two runs of the *before* build gave **8,053 ms then 551**, which is the driver compiling that one large shader once per driver cache. That is now not compiled at all, and the pipeline stage reads **7 ms**. The picture is untouched: content hash `1f4710eee4ee2585`, GPU 3.994 ms, `--validation` clean, both node-pool audits clean, 548 tests.

**The gate is partly not R3d's to close, and saying so is the point.** §8 asks for *"no per-pixel random numbers remain"*. `shaders/resolve.comp` still carries `hash_u32` for the composite's ordered dither — and deleting that dither is **R5c**, in as many words. R3d removed every per-pixel random number in the *lighting*; the last one is in the composite and belongs to the stage chartered to replace it. The other two clauses (enclosed within 30% of outdoor, face pass within 10% between 1080p and 4K) are statements about the face architecture rather than about this deletion, and are unmeasured here.

**What R3d hands to R1e**: `shaders/world.glsl` now has exactly **one** includer, `shaders/visibility.comp`, which is `--chunk-marcher`. That was the whole reason for doing R3 before R1e (D278) — R1e no longer has a path tracer to port.

**Still owed from R3b, and not done here**: splitting `GpuFace` so the CPU's half and the card's half are never in one copy. The line is known — `x, y, z, packed` host, `irradiance, photons, counters, bins` card — and `src/gpu/face_light.*` already exists as the card-only home.

## R1e's gate, and the baseline it could not be measured against

| # | Decision | Kind | Why |
|---|---|---|---|
| D519 | **`r2-node-pool.csv` is superseded, and comparing against it measures the wrong thing** | trap | R1e's gate is *the grid table does not move*, so the grid was run against the recorded baseline. Its own first line refused: `realtime enclosed deck — was 0 solid voxels, now 127,198,381 — not comparable`. That row is honest about itself; the other twenty are not, because they are **comparable in format and not in subject**. `r2-node-pool.csv` was recorded at R2, before `shade_faces.comp` computed anything. Between then and now the renderer gained sun shadows (D290–D303), ambient occlusion (D325–D400) and lamps (D401–D409) — so the near cameras reading **+41% close, +72% mid, +40% outdoor** is not a regression, it is three lighting terms that did not exist when the baseline was taken. The distance cameras, which those terms barely touch, went the other way: **far −45%, distant −43%, sky −51%**. D243 and D244 already said figures either side of the world-cache change are not comparable; this is the same rule one stage further on, and the lesson is that a baseline file needs the reason it is invalid recorded *in it* rather than inferred later from a voxel count |
| D520 | **A fresh baseline is recorded now, at the point chunks are half gone** | instrument | `documentation/baselines/r1e-chunks-going.csv`, twenty-one realtime rows, seven cameras at three resolutions, on the build where `world.glsl`, the summary octree and the thumbnail tiers are deleted and `residency.*` is not. That is the comparison the rest of R1e has to hold against, and it is the first grid in the file taken with the face pass doing all four of its jobs. Pathtrace rows are excluded rather than kept empty, because the mode does not exist |

**What the grid does settle.** The distance cameras render and are *faster*, which is the outstanding question from the clip-box change: had the box been wrong, `far` and `distant` would have collapsed towards `sky`'s cost and `sky`'s speckle. They do not — `distant` is 0.900 ms at speckle 7.2 against `sky` at 0.919 and **0.7**. Different pictures at the same price, which is what a working clip box looks like.

**And a clause of R3d's gate, which was open.** §8 asks for *the enclosed room within 30% of outdoor at the same resolution*. Measured here: **1.00× at 4K, 1.26× at 1440p, 1.35× on the Deck**. Two of three pass; the Deck is five points over and is the resolution the budget is written for, so it stays open and is R5's to close.


## R1e is finished: chunk residency, its ten buffers and its descriptor set are deleted

*The fifth and last slice. What went: `src/world/residency.{hpp,cpp}` (1,329 lines),
`src/gpu/world_buffers.{hpp,cpp}`, `tests/test_residency.cpp`, the chunk marcher's orphaned
descriptor set, the reference tracer's descriptor set, its 256 MB face cache, its frame-statistics
buffer, `rebuild_coarse_grids`, `--stream-frames` and `--stream-log`. What arrived: 
`src/gpu/type_tables.{hpp,cpp}`, which is the two interned tables and nothing else.*

| # | Decision | Kind | Why |
|---|---|---|---|
| D521 | **A Vulkan layout's bindings need not be contiguous, so nothing had to be renumbered** | change | The handover said the world buffers could not go until the descriptor bindings were renumbered, and called that the risky edit of the stage. It was not needed at all. `clouds.comp` names bindings **13, 20 and 21** and it is the only pipeline left on the tracer's set, so the set is declared with those three and the seventeen descriptors no shader mentions are simply absent. The buffers behind them then have no reader and go with them. **The chunk marcher's own set went whole**: it outlived its pipeline by a stage, and was still being allocated, written on every resize and filled with eleven buffers no shader declared. `--validation` clean, and the cloud pass measures **0.227 ms against a same-day control's 0.224** |
| D522 | **The per-frame maintenance of the chunk system is gone, and what it was costing was not what this file said** | measurement | The handover has said "about 12 ms of CPU a frame" since R1g. The instrument disagrees and always did: `residency_.update` reads **0.003 ms mean with a 12–15 ms worst**, so the twelve milliseconds were one frame's spike quoted as a steady state. What the deletion is actually worth, all measured against a same-session control build: **device memory 970 MB → 112 MB** (world buffers 714, face cache 256, against 48 MB of tables and 64 of staging), **warm start 505 ms → 340 ms to ready**, and **3.86 ms → 0.00 ms per edit**, which is the one a player feels. The last is `rebuild_coarse_grids`: five wrapped occupancy grids rebuilt over the whole world for a change one metre across, on every chisel stroke, every undo and every region paste. What is left of it is the sweep for the world's extent, which is what the ray clip box is made of and is O(chunks) |
| D523 | **The picture and the tree are unmoved, and the grid is not the thing that says so** | measurement | Content hash **766f2fd63f1a01c4** before and after, `GPU mirror matches`, *leaf for leaf* and *mask for mask* clean, `--validation` clean, **505 tests** (548 less the 43 that went with `test_residency.cpp`). The settled enclosed picture differs by **1.266 of 255 over 25,395 pixels against a run-to-run floor of 1.283 over 25,764** — inside its own noise. The grid file could not settle it: `r1e-chunks-going.csv` and a fresh run differ by up to 16% on the close camera, and an **interleaved** same-session pair says the two builds are equal there (control 4.027 ms, this 4.018–4.037). The machine drifted about 10% over a session of measuring, which is D407's warning arriving through the grid rather than through the face pass |
| D524 | **The baseline harness was recording no scene at all, and comparing nought with nought** | trap | `baseline.ps1` gates every comparison on "were these two rows measured against the same world" (D231). It read the voxel count out of the `scene:` line with a regex that wanted the clip ladder's *"N of M regions"* — which a settled world does not print. So **twenty of the twenty-one rows** of the last baseline recorded a scene of nought voxels, every comparison against them passed the gate by comparing nought with nought, and the single row that did carry a count was the only one that ever objected. It reads exactly like a clean run. It now parses the **content hash** (D243), which is the scene's identity rather than a description of it, refuses a row that has none, and says loudly when the run it just took printed no scene line. Trap 10 — two different answers in one colour — in the harness rather than in the engine, and this is the second time a harness fault has been found by reading its output rather than by it failing |
| D525 | **`NodePool::stats()` walks the tree, so the frame must not call it — and the frame did** | trap | The overlay's report and the crash context both moved from chunk residency's counters to the pool's, and `stats()` sweeps every node for the level histogram and **popcounts every resident leaf** for the average fill: 1.5 million popcounts on the facility. Its own comment says it is read once at an audit. Two per-frame callers cost **1.76 ms of a 5 ms frame** — a change that only deleted work measured 7.27 ms against a control's 5.19. Split into `live_stats()`, which is counters only, and `stats()`, which keeps the name that sounds expensive. **How it was found is the reusable part**: every host cost this file already timed came out equal between the arms, so the frame was split into head, recording and present — head read **1.911 ms against 0.154** and named the line. Trap 17 exactly: a timing figure that covers more than one thing hides which thing it is. The split is kept as `CPU last frame: feedback / uploads / report` |

**What R1e leaves.** `visibility.comp` and `node.glsl` march one sparse octree; `node_buffers` mirrors
it; the composite reads a face store and two interned tables. Nothing in the renderer addresses a
chunk. Chunks remain what `03-voxel-data-model.md` says they are — a storage and networking unit —
and `World`, `Chunk`, `serialize` and `world_cache` are untouched.

**The one instrument that went with it** is `--stream-frames`, the chunk-mirror audit, which asserted
after every frame that the CPU's copy of each resident chunk still hashed to the world's. Its
successors are `NodePool::stale_leaves` and `stale_masks`, which ask the same question of the tree
the renderer actually walks, run headless in `ws_tests` and print at every screenshot. The release
workflow and `tools/package.ps1` drop the line rather than replacing it.

## R9a — the face set stops being what the camera can see

*The store holds a face because a primary ray stopped on it, so it holds exactly what the camera can
see and the light in it is a screen-space set wearing world-space clothes. This is the first half of
the stage that ends that: a light ray may now name the one face it LANDED on, the store keeps those
in a class of their own with a cap, and an instrument reports both classes. Nothing reads them yet —
that is the next change, and it is why the picture does not move.*

| # | Decision | Kind | Why |
|---|---|---|---|
| D526 | **A gathering ray names the one face it landed on, and the store holds it in a class with a cap** | change | R9a and R9b. The ambient far ray — the unbounded one, cosine-weighted, already cast — reports the face it stopped on down the feedback buffer with `kFeedbackSecondary` beside `kFeedbackFace`. It is the deliberate exception to D292: a ray names what it LANDED on, which is one face, never the geometry it crossed. The host's answer is a claim, which builds nothing and asks the world for nothing, so R9h's *no light path may cause streaming* is intact by construction and measured: node requests over the run **18,828,939 against 18,830,058**, built 5 against 1. The throttle is a phase on the slot and a period in frames (`--secondary-period`, 64), because there is no slot to stamp — the whole point is that the face is not in the store yet, so `node_seen`'s trick (D431) has nothing to key on. The cap is a quarter of the table and a claim past it is **DECLINED, not refused**: a refusal is a visible surface with no light of its own, which is D502's picture; a decline is one gathering ray reading a coarse stand-in. It is declined whenever the store is under pressure at all, so the class cannot push the table into refusing anything |
| D527 | **The sun's ray budget was being divided among faces nobody is looking at, and the regression came out looking like a win** | trap | `face_stride` — how often a settled face is refreshed — was `watermark / kFacesPerFrame`. Adding 262,144 off-screen faces therefore diluted the refresh rate of every face on screen by the ratio the new class had grown to, and the faces pass **got cheaper: 1.16 ms against 0.96**. The number that was supposed to go up went down, and the cost was hidden where no pass table looks: convergence, **84 sun samples a face against 72**. That is exactly what the plan says a per-class budget is for — one shared budget lets the off-screen set starve the on-screen one, and the on-screen one is what the player is looking at. The stride is divided by the on-screen population now, and both arms read **85 samples a face** with mean sun visibility **0.1290** to four decimals. The general shape is worth more than the fix: **a change that makes a pass cheaper by giving it less to do is a regression wearing an improvement's clothes**, and the only thing that tells them apart is a convergence number printed beside the time |
| D528 | **An instrument's own bookkeeping must never live in a field the card is writing to** | trap | The class started as bit 2 of `GpuFace::packed`, which is the obvious place — the flags byte has room. `packed` is mirrored to the card, `FaceBuffers::upload` sends whole records for every slot the store marks dirty, and the record has two owners: the host owns the key and the flags, the card owns `irradiance`, `photons` and `counters`. So `promote()` — a face a bounce ray found and a pixel then walked up to — marked the record dirty and sent the host's **zeroed counters over the light the card had accumulated**. That is D295 exactly, arriving through a door D295 did not name: not a coalesced range this time, but a host-side flag change on a live record. **29,882 faces a flight** would have lost their light and re-burst, with a right picture, a matching mirror, and only the cost moving. The class lives in a host-only array beside `last_read_` now, `promote` marks nothing dirty, and `FaceStore::validate` counts the classes present against the counter so a missed decrement cannot creep |
| D529 | **The face audit refused to say anything in the one case that costs** | instrument | `FaceBuffers::audit` returned at its first line whenever the store had anything still owed to the card — right for the mirror COMPARISON, which would otherwise read a host that has moved on against a card that has not, and wrong for everything else it prints. A moving camera always has an upload backlog, and the moving case is where this pass spends 63% of a frame, so every number about the card's state was unavailable in precisely the situation that needed it: two arms of an A/B differing by a factor of two, with nothing in either log to say why. What the card wrote into its own half of the record is a fact about the card and needs no host copy to agree with anything. The comparison is skipped and the statistics are printed, which is what made D530 and D531 answerable at all. Trap 15 from the other side — there, a measurement that never ran looked like a clean one; here, an audit that could not run looked like a quiet one |
| D530 | **The card's own stand-ins are counted now, and the count killed the obvious explanation in one reading** | instrument | Nothing had ever reported how many provisional faces the card claims for itself, and they are the most expensive face in the store: a provisional record is rewritten every frame it is needed, so it can never accumulate and it takes a fresh unbounded ray and a fresh lamp burst **every frame**, against half a million store faces that cost a few rays each and then stop. The first theory for D531 was that R9a's claims were covering stand-ins the card would otherwise have to invent. Counted: **8,254 claimed on the frame against 8,256** in the other arm. Not it, and it took one run rather than a session of reasoning. Trap 16 — when a cost is suspect, count the events that produce it |
| D531 | **The face pass while flying costs what the CARD holds, and the card runs up to 434,838 records behind the store** | measurement | The store evicts a face by zeroing its record and marking the slot dirty. `FaceBuffers::upload` sends dirty regions within one frame's staging budget and, when it runs out, **clears nothing and retries the whole set next frame** — so a moving camera leaves the card holding live records for faces the host gave up long ago. Measured flying at 1440p over 400 frames: the card is **434,838 records ahead of the store** (982,358 live there against 547,520 here) with the upload exhausted on **165 frames**. The faces pass tracks that number and not the store's: sweeping `--secondary-period` gives 3.103 ms at 426,196 ghost records, 2.634 at 416,000, **1.552 at 158,429**, and 4.342 at 47,749 — where the last is the period-16 arm, whose own on-screen set is 867,053 faces and which is therefore expensive for the ordinary reason. **R9a's flying win is that backlog moving, not the light getting cheaper**, and the backlog is a fault of its own which predates this change and is now instrumented |
| D532 | **Measured, flying, five interleaved rounds of one build** | measurement | Faces pass **2.835–3.156 ms (median 3.123) against 1.507–1.702 (median 1.621)**, total GPU **10.137–10.649 against 9.447–9.685**, with no overlap between the arms on either figure and **nought refusals in both**. The settled picture is unmoved — the arms are apart by 1.323 of 255 over 26,457 pixels against a run-to-run floor of 1.284 over 25,260 — and the flying picture likewise, 0.173 over 12,929 pixels against the control's own floor of 0.186 over 13,367. One side-result worth keeping: with the rule on, **two runs of the flight differ on 1,983 pixels against the control's 13,367**, which is D427's shape again — a change that makes a camera reproducible is worth more to every later measurement than the milliseconds are |

**What this does not do, and saying so is the point.** Nothing reads the off-screen set. `may_cast`
gates a face on a pixel having read it, so a face claimed by a light ray casts no rays, holds no
light, and costs the store a record and the feedback buffer about 1,200 entries a frame. That is
deliberate: the consumer is R9's bounce, where a gathering ray reads the face it lands on instead of
`kIndirectFloor` — the constant standing in for every bounce of indirect light in the building — and
it is the next change. Landing the supply first is what made D527 and D528 findable at all, because
each of them would otherwise have arrived inside a change that also moved every pixel in the picture.

**The levers**, all run-time so the arms of an A/B are one build (D407): `--no-secondary-faces` is the
control arm, `--secondary-period N` is the window in which a face may name one face a light ray of its
own landed on (a power of two; 0 is off), and `--secondary-share N` is the cap as a divisor of the
table.

## R9 — bounce: the light indoors stops being a constant

*The composite has lit every interior in this game with `kIndirectFloor = 0.5` — a number standing
in for all indirect light, with a comment saying R9's bounce would replace it. It does now. A
gathering ray reads the surface it lands on, and what used to be assumed is measured.*

| # | Decision | Kind | Why |
|---|---|---|---|
| D533 | **The bounce rides the ray that was already being cast, so the fourth term costs about a twentieth of a frame** | change | The ambient FAR ray is unbounded and cosine-weighted about the face's normal, which is exactly the ray an indirect estimator wants, and until now everything it found was thrown away except *did it reach sky* and *how far did it get*. It now also returns what it FOUND: `sky_radiance(dir)` where it escaped, and the outgoing radiance of the face it landed on where it did not — read out of that face's own record, which the same pass filled in. Three words of face light hold the running sum and the far field's own count divides it, because it is one ray answering two questions and a second count could only ever be a way for them to disagree. **Measured, enclosed camera, 1,200 frames, three interleaved rounds of one build**: faces pass **0.757/0.762/0.780 against 0.644/0.722/0.726**, total GPU **3.101/3.155/3.219 against 3.024/3.028/3.072** — about **+7% of the light pass and +5% of the frame** for the term that lights every interior. Flying at 1440p the arms overlap: **1.972/2.099/2.148 against 1.812/2.021/2.089** |
| D534 | **One layout constant declared twice drew the enclosed camera as salt and pepper, and it read exactly like an estimator with too few samples** | trap | `kFaceLightWords` was `const uint = 9u` in **both** `node.glsl` and `resolve.comp`, the second with a comment saying the layout is documented in the first. The record grew to twelve words for the bounce and only the writer's copy changed, so the composite indexed every face's record at the wrong stride: each pixel's sun visibility, sky visibility, near field and lamps came out of some *other* face's record. The picture was a black building under dense black-and-white speckle — which is precisely what a per-face Monte Carlo estimator with ten samples looks like, so the first two fixes went after the estimator. It is one declaration now, in `shaders/face_terms.glsl`, which both include. **The lesson is not "keep them in sync"**: a constant that describes a layout has to be declared where the layout is, once, and the comment pointing at the other copy is the smell — it proves somebody knew there were two |
| D535 | **A per-face estimator is believed in proportion to its samples, and the blend is smooth because a threshold is a discontinuity per face** | change | A bounce sample is a RADIANCE and has no bound to average itself against: one ray landing on a sunlit sill returns twenty times what the wall around it does. The first version believed the mean the moment a single ray existed and dropped `kIndirectFloor` outright, so neighbouring faces disagreed by the whole of that constant. Confidence is `far_n / kBounceBelieve` clamped to one, mixing the constant out and the measurement in together, and it is **smooth for D387's reason**: a hard per-face decision puts a new discontinuity on every face that straddles it, and neighbours either side of it disagree by more than the noise ever did |
| D536 | **The convergence rate was already right, and making it faster cost more than the term itself** | trap | `kSkyFarEager` gives a face its first thirty-two far samples every visit and the rest on the sun's schedule. The bounce needs `kBounceMin` = 128 samples to stop, and at one ray every `face_stride` frames that is **thirteen seconds** of a room visibly filling with light — so the eager phase was extended to the whole of it. It converged in about two seconds and it cost the faces pass **2.213–2.395 ms against 1.977–2.464 flying, up from 4.334–4.628** — over its 4.40 ms budget for the first time since D416. It was **not needed**: `kBounceBelieve` is thirty-two, so the face is believed outright at the end of the eager phase and everything after it only takes noise out. Fast to right, slow to quiet, and the expensive half is the one nobody is waiting on |
| D537 | **A host struct and a push block are not the same shape by default, and only the validation layers say so** | trap | `vec4` in a shader's push block aligns to sixteen bytes; `f32[4]` in a C++ struct aligns to four. Adding the sun's colour to `NodePush` therefore made the shader declare 128 bytes against a range of 124, and every field from `sun_colour` on was read four bytes early. `--validation` named it in one line — *"has a push constant buffer Block with range [0, 128] which outside the VkPushConstantRange of [0, 124]"* — and on a machine without the layers it is a wrong sun colour rather than an error. The pad is now declared in both structures so they are visibly the same shape rather than accidentally the same size, and the `static_assert` is an **equality** rather than a bound: a host struct SMALLER than the shader's block is the failure, and `<=` cannot see it. D168's std140 fault, in the push block instead of the uniform block, and D518's rule — run `--validation` when a layout changes — earning its keep for the second time in two stages |
| D538 | **The picture, and it moved the other way from the prediction** | measurement | The handover predicted that one measured bounce would be **dimmer** than a constant of 0.5 and that interiors would go dark until the exposure meter had a writer. Outdoors it is the opposite and by a wide margin: sunlit stone bounces far more than the constant ever stood in for, so the portico, the column shafts and the steps go from crushed black to legible. Indoors the constant was flat and the measurement has shape — the niche interiors, the dome and the pilaster shafts carry a gradient that was not there. **Enclosed camera: 16.998 of 255 over 763,794 pixels of 1,024,000**, against a run-to-run floor of 3.176 over 74,465. And it is quieter rather than noisier: **speckle 17.62 with 9 fireflies against 21.22 with 81**, because the surfaces that used to be lit by a constant plus a noisy shadow term are now lit by something with light in it. Two runs of the bounce arm differ by 3.176 against the control's own 3.142, so the term adds no reproducibility cost |

**What is left of `kIndirectFloor`.** It is the fallback for a face that has taken no far rays yet,
and nothing else. Where a measurement exists the constant is mixed out in proportion to it, so a
surface is never lit by an assumption *and* a measurement at once — which is the double-count
`face_sky_visibility` exists to prevent, one term along.

**It is multi-bounce for nothing.** A face's stored bounce is part of what it gives off, so a ray
reading that face reads the second bounce, and the face it read from carried the third. The series
is a progressive radiosity solve spread over frames and it converges for radiosity's own reason:
every step is multiplied by an albedo under one. The cost is nought, because the number is in the
record already being read.

**The levers**: `--no-bounce` is the control arm and restores the constant exactly, and
`--bounce-min N` is how many far samples a face takes before its bounce may stop — the trade is
unbounded rays against a per-face mottle in every interior, and it is a run-time figure for D430's
reason, that a trade nobody can sweep at run time is a trade somebody guesses at.

## The grid on flat surfaces, reported from playing — eight things it is not

*Reported mid-session as "subtle horizontal lines on everything", then "it seems to be a grid", then
"it has something to do with resolution — when I set the window to max size it gets fixed as I leave
and come back". Not closed. What follows is the elimination, because every line of it is a
measurement somebody would otherwise take again.*

| # | Decision | Kind | Why |
|---|---|---|---|
| D539 | **A per-face grid on flat surfaces: measured, narrowed to eight eliminations, and open** | measurement | The quantity is the step between neighbouring voxel faces — the second difference down a column, in units of 255 — measured on the rotunda floor and on the portico steps. **It is not the bounce**: 5.65 with the term off against 5.66 with it on, on the same patch. **Not the model**: the wall and the floor are one flat plane in the normals view. **Not the detail level**: every pixel there resolves at level 0. **Not the material**: the steps are achromatic — a luminance step of 5.66 against a colour step of 1.18 — while the clip's own `variation colour=0.030` nudges the three channels independently and would show as colour. **Not the sun, the sky or the near field, indoors**: on that patch the first two are exactly nought and the third is 255 on 92% of the pixels. **Not the ancestor prolongation**: `--no-face-prolong` reproduces it to the digit. **Not the lamps**: it is present, and larger, on the portico steps outdoors where no lamp reaches. **Not the ray footprint**: pinning the face pass's rays to a fixed reference height instead of the screen's was built, measured at 5.08 against 5.09, and reverted |
| D540 | **The resolution measurement that started it was wrong, and the way it was wrong is the reusable part** | trap | The first pass at "does it depend on resolution" compared the second difference at one pixel's step in each shot and read **5.09 / 4.19 / 3.72** at 800p, 1080p and 1440p — a clean trend, and an artifact. At 1440p a voxel is about fourteen pixels across, so a one-pixel step measures how smooth a face is INSIDE itself, not how much two faces disagree. Resampling every shot to one size first and measuring there gives **5.09 / 5.28 / 4.89**: flat. A per-face quantity has to be measured per face, and a step in pixels is not a step in faces at two resolutions. The reported symptom really does change with the window, so what changes is still open — but it is not the size of this |

**What is left, and it is a class rather than a bug.** Every light term on a face is a Monte Carlo
estimate over that face's own samples, and **nothing filters across faces**. The sun's visibility is a
proportion over about eighty-five samples, so at half shadow its standard deviation is about five per
cent — which is the size measured in the steps' penumbra (10.98 of a mean of 146, against 13.56 of 89
with bounce off). Indoors the same shape arrives through whichever term is carrying the light there.
Two faces side by side draw different samples, so they disagree, and a disagreement that is per face
on a voxel grid **is** a grid. **That is R5, the face denoise, by name** — filtering across
neighbouring faces is exactly its shape, and D381/D382 already assigned face-to-face variance to it
after measuring 2.5 of 255 on a wall. These are three to fourteen, so that measurement understated
what a penumbra and an interior do.

**One clue is not yet explained and is the thread to pull.** *"It gets fixed as I leave and come back
to the same place"*: a face's ambient term converges once and then stops for the life of the face
(`kFaceAmbientDone`), and its bounce stops with it. So a face that converged while the room was still
filling with light keeps that darker answer for ever, while its neighbour — claimed a few seconds
later — converged against a brighter room. Walking away and back evicts and re-measures them
together. That would produce a per-face patchwork that heals on a revisit and has nothing to do with
sampling noise, and it is testable: the same camera, one run settled from cold against one run whose
store was claimed after the room converged.

## There is no minimum light

*Asked for in one line — "if something in the game receives no light it's pitch black, there's no
minimum light" — and then again for the air: "make fog or haze not able to help you see better in
pitch black environments". Both were true faults and neither could have been found by measuring the
facility, because the facility is lit.*

| # | Decision | Kind | Why |
|---|---|---|---|
| D541 | **Two ambient constants are deleted, not switched off** | change | `kIndirectFloor = 0.5` was the share of the sky term applied wherever the measured sky visibility fell short — an assumption standing in for bounced light, with an expiry written into its own comment. `kGroundBounce = 0.12` was worse and had no such note: it was added to **every surface in the world**, multiplied by the near field and by nothing else, so a face sealed inside solid stone with no sun, no sky and no lamp still received a fixed share of the sky's radiance. R9's bounce measures what both were pretending to be, so both are gone. What replaces them is arithmetic that cannot produce light out of nothing: sun × its measured visibility, plus sky × its measured visibility, plus lamps, plus the measured bounce. Every term is nought when nothing reaches the face, so the face is black |
| D542 | **The air was the second way in, and a worse one** | change | `apply_media` lit every cubic metre of fog with `sun_radiance()` and `sky_radiance(up)` whether or not either could reach it, so a sealed room with fog in it **glowed** — and the glow sits in FRONT of the walls, so no amount of black paint on them removes it. Fog was a torch. It now takes `sun_reach` and `sky_reach` from the caller, which the composite fills with the surface's own measured sun and sky visibility: nought in a sealed room, one in the open, and the fix costs two multiplies. **It is a proxy and is written down as one** — the air halfway along a ray is not the surface at the end of it, so fog in front of a shadowed wall is now dimmer than it should be. That is the error this renderer is required to make: the alternative is an occlusion ray per pixel, which is exactly what this function was rewritten to remove at twenty milliseconds of a thirty millisecond frame |
| D543 | **The clause has a gate, because it is the kind that gets lost** | instrument | An ambient constant is invisible in every measurement of a lit scene: it reads as taste, it survives every image diff, and the two above lasted the whole rewrite. `clips/sealed_dark.clip` is four metres of air with two metres of stone on every side — no opening, no emitter, no sky — and `tools\darkroom.ps1` renders it, ignores the developer overlay, and reports the brightest channel in the picture. **Measured: brightest channel 0 of 255, every pixel, clear and with `--fog 0.08,0.9,0.6,40,0`, and again with `--no-bounce`.** Run it after anything that touches the composite or the air |

**What this costs, measured on the facility.** Almost nothing, which is the interesting part: the
enclosed camera's mean pixel goes **126.3 → 124.8** and the portico's **135.0 → 133.6**, because the
light those constants were faking is now light the bounce measures. What moves is the dark end —
**the darkest pixel indoors falls from 4.7 of 255 to 0.4**, and it was 4.7 because it could not go
lower. Nothing anywhere is lifted off nought any more.

**One thing that reads like a floor and is not.** A face with no samples yet is drawn with the
composite's own fallback — full sun, fully open sky — because a surface that has not been measured
has not been shown to be dark. That is latency and it lasts a frame or two (D313, R9d). It errs
bright on purpose, and making it err dark instead is a separate decision with its own trade.

**And `--no-bounce` is no longer a way back to the old picture**, which is worth saying because it
was one yesterday. The floor it used to restore does not exist. With the bounce off, an interior is
lit by the sun, the sky and the lamps it can actually see, and by nothing else.

## D544 — the upload sends what fits, and the light while moving turns out to have been missing

*The face pass shades what the CARD holds, and D531 measured the card running up to 434,838 records
behind the store while flying. The cause was one line: an upload that ran out of staging cleared
NOTHING and restaged the whole dirty set next frame, so it ran out in the same place for ever. This
is the fix, and what it uncovered is worth more than the fix.*

| # | Decision | Kind | Why |
|---|---|---|---|
| D544 | **A partial upload marks clean exactly what it sent** | change | `DirtySet::clear_range` is the primitive that was missing, and each staged run is marked clean as it is staged — so a frame that runs out of staging still makes progress and the next one carries on from where it stopped. The old rule sounds like the safe one and is not: what it guards against is marking clean what was never sent, and per-run clearing never does that, while the whole-set retry restages the same prefix until the camera stops moving. Measured flying at 1440p over 400 frames: **the card is 0 records ahead of the store against 80,211**, and the upload **ran out of staging on 1 frame against 253**. `--whole-set-retry` is the control arm and `tests/test_dirty_set.cpp` is the gate — six cases, including that clearing every run of a `runs()` snapshot drains the set exactly as `clear()` does, because that is the property the change rests on |
| D545 | **The backlog was not costing frames, it was skipping the light — and the pass got THREE TIMES more expensive because it started doing its job** | measurement | The obvious reading of D531 was that the card holding stale records made the pass shade more than it should. It was the other way round, and the audit says so in one line: **`seen on the card: 0 of 721,911`** in the control arm. Nought. The card's bucket table was so far behind that `node_face_lookup` could not find the faces the pixels were resolving, so nothing was stamped as seen, `may_cast` was false everywhere, and **the shading pass was skipping almost all of it**. The same run claimed **8,255 provisional stand-ins in one frame against 723**, each taking a fresh unbounded ray and lamp burst every frame, and sent the host **0 read-reports against 22,702**. So the light while flying was not being computed at all; it was being replaced, frame by frame, by throwaway coarse guesses. With the upload draining, the faces pass reads **6.784–7.188 ms against 1.992–2.007** and total GPU **14.099 against 10.020** — and none of that is new work, it is the work that was being dropped. **A pass that got cheaper because its inputs never arrived is D527's trap arriving through the upload**, and the number that tells them apart is the same kind: not a time, but how much of the store the frame could actually see |
| D546 | **What it looks like, which is the blocky flicker again through a third door** | measurement | Same frame, same flight, two flags of one build: the control draws the balustrade, the window reveal and the cornice as hard-edged black and white **blocks**, and the fix draws them as lit stone. **44.90 of 255 over 2,761,206 pixels of 3,686,400**, with speckle **23.86 and 2,720 fireflies against 19.92 and 944**. That is D502's reported picture — *"the light becomes like squares and they flicker rapidly"* — with a cause that has nothing to do with the store filling up: the store was fine, and the card simply had not been told. **Three separate roads now lead to that one picture** (a full store, a refused claim, and an upload that could not keep up), which is worth remembering the next time it is reported: read `the card is N records ahead of the store` and `seen on the card` before believing any theory about the store itself |

**What is left, and it is a budget question rather than a fault.** The faces pass is now **6.8–7.2 ms
against its 4.40 ms budget** while flying at 1440p, because it is doing the work it used to skip.
That is the honest state and it should not be hidden by comparing it against a control that was
drawing blocks. The levers that exist are `--bounce-min`, `kSkyBurst`, `kLampBurst` and the face gate;
the one the plan already owes is **R5's denoise**, which is what lets every one of those converge on
fewer samples. Nothing here is a reason to put the backlog back.

**One thing to know before touching the upload again.** `stage_regions` takes the store by reference
now, and marks each run clean *inside* the loop that walks `runs()`. That is safe only because
`runs()` hands back a snapshot vector rather than a view, and the test named *"clearing every staged
run drains a set the same as clear()"* is what pins it. If `runs()` is ever made lazy, that loop
becomes a walk over a container being mutated underneath it.

## D547 — the light remembers where you have been standing

*Reported from playing, in these words: "when i stay still for a while and then move back the voxel
faces where i stood still are way betterly rendered than the rest and are often brighter". Both
halves are real, they have one cause, and it is the only term in this renderer that measures
something still in motion.*

**Photograph the dwell before theorising about the seam.** The instrument is `--cut`, which jumps the
camera once at a measured frame, so one arm can arrive at a view the other has been staring at:

```powershell
.\build\bin\WorldShaper.exe --screenshot dwell.png --screenshot-frame 900 --settle `
  --width 1280 --height 800 --cam "0,2,-20,90,0" --quality 7 --no-vsync --no-update-check `
  --no-auto-quality
.\build\bin\WorldShaper.exe --screenshot arrive.png --screenshot-frame 900 --settle `
  --width 1280 --height 800 --cam "0,2,-20,-90,0" --cut "600,0,2,-20,90,0" --quality 7 `
  --no-vsync --no-update-check --no-auto-quality
```

| # | Decision | Kind | Why |
|---|---|---|---|
| D547 | **The picture is a function of how long the camera has been pointed at it, and the number says so** | measurement | Close camera, `--settle`, one build, whole-frame mean pixel by how many measured frames the camera had been standing there: **131.290 at 150, 131.794 at 300, 132.608 at 900, 132.697 at 2,700**. Nothing in the world is changing across that — it is the same camera looking at the same settled scene, content hash unmoved. Arriving 300 frames before the shot instead of dwelling the whole run reads **5.461 of 255 over 260,752 pixels of 1,024,000, against a run-to-run floor of 2.868 over 79,519** — and the arrival is the DARKER of the two, which is the direction the player named |
| D548 | **It is the bounce, and `--no-bounce` proves it rather than an argument doing so** | measurement | The same pair with the bounce off: **2.172 over 46,198 against a floor of 1.480 over 31,278**. So the excess over the floor goes from **2.59 of 255 and 181,233 pixels to 0.69 and 14,920** — the bounce carries about three quarters of the whole dwell dependence. It carries most of the noise too: the run-to-run floor itself halves with the term off. Two flags of one build, per D407 |
| D549 | **A cumulative mean cannot measure a room that is still filling** | cause | `bounce_radiance` reads what the face a ray landed on is giving off **at that moment**, and that face is itself climbing from black as it takes its own samples: this is a progressive radiosity solve, iterated one ray at a time. The estimator over it was `sum / far_n`, a mean over the face's whole history — so it converges to the **average of the climb** rather than to the top of it, and its time constant grows with the sample count, which is exactly "how long have you been looking at me". Then `kFaceAmbientDone` freezes whatever it holds for the life of the face. Every other term in the record measures something that does not move — geometry, or a sun that has not — so a cumulative mean is right for all of them and wrong only here |

**Why the obvious reading of this is wrong, and it is worth not repeating.** The seam looks like a
convergence-latency problem, and `--debug-mode 19` after a 35-degree turn shows exactly that: the two
thirds of the facade the camera had been pointed at is solid green — converged and silent — and the
third it has just revealed is grey. That picture is true and it is not the fault. It closes in two
seconds by itself; the brightness does not close at all. **Read the mean pixel against dwell time
before reading any convergence view**, because the two faults draw the same picture and only one of
them goes away on its own.

## D550 — a bounce that remembers the last N samples instead of all of them

*The fix for D549. It is three words of the record it already had and one `min`.*

| # | Decision | Kind | Why |
|---|---|---|---|
| D550 | **The bounce is a mean with a memory** | change | `mean += (sample - mean) / min(far_n, kBounceMemory)`, stored where the sum used to be, so the record does not grow and the two readers lose a divide rather than gaining one. A mean with a memory of N has the variance of a simple mean of 2N - 1 samples, so the noise becomes a number that is chosen rather than one that is accepted — and past N samples the estimator tracks the room's CURRENT light with a bounded lag whatever the face's age. `--bounce-memory N` sweeps it; the control arm is a memory larger than `far_n` can reach, which is the cumulative mean exactly rather than near it |
| D551 | **`kBounceMin` is four memories, because a face that stops has frozen whatever it holds** | change | 128 to 512. Three turnovers leave 5% of the fill-up in the mean and the fourth is the memory it spends filling before it forgets anything at all. **Nothing about the RATE changes** — it is the same one unbounded ray every `face_stride` frames the face was already casting — so no frame casts more rays than it did; a static camera simply falls silent later. Read the two numbers together and not one at a time: memory 32 at min 256 is the NOISIEST arm in the sweep below, because most of the noise came from the far COUNT and not from the memory |
| D552 | **A cap below the floor is a floor that does not exist** | trap | `far_done` terminated on `far_n >= kSkyFarConverged`, which is 256. Invisible while the bounce asked for 128, and it binds every face the moment the bounce asks for more: the face would stop at 256 samples however many the constant said, silently, with the only symptom a term stopping short of its own setting. It is `max(kSkyFarConverged, bounce_floor)` now, and `--no-bounce` leaves the floor at nought and the cap at exactly what it was. Trap 7's shape living in a comparison rather than in an answer |

**The sweep, because both numbers are a trade and neither was guessed at.** Close camera, 1280x800,
`--settle`, frame 2,700 — every face converged and silent in every arm, so this compares the frozen
answers rather than a race between them. The unbiased value is what the shortest memory converges to,
since a memory of 32 has no fill-up left in it at all:

| memory | kBounceMin | speckle | mean pixel | short of unbiased |
|---|---|---|---|---|
| none, cumulative | 128 | 47.63 | 132.708 | **-0.85** |
| 32 | 256 | 54.00 | 133.553 | 0.00 |
| 64 | 256 | 49.42 | 133.440 | -0.11 |
| 96 | 384 | 47.08 | 133.460 | -0.09 |
| **128** | **512** | **45.53** | **133.504** | **-0.05** |

So the dwell bias goes from 0.85 of 255 to 0.05 **and the picture gets quieter by 4.4%**. The second
half is not a bonus, it is where the noise always was: the far ray answers the sky as well, and four
times the samples is half the error in `open_sky`.

**Measured on the two cameras the fault lives between**, 1280x800, `--settle`, frame 2,700, two flags
of one build:

| | speckle | mean pixel | before against after | that arm's own floor |
|---|---|---|---|---|
| **enclosed** | **20.701 to 16.970** | **122.785 to 126.412** | 5.086 over 236,117 px | 2.842 over 49,860 |
| outdoor | 16.752 to 15.351 | 161.590 to 161.649 | 0.586 over 13,716 px | 0.450 over 8,221 |

That signature is the diagnosis restated: **the enclosed room, where every bounce lands on a wall,
gets 18% less speckle and 3.6 of 255 more light — and the outdoor view does not move**, because
outdoors the far ray reaches sky on the first sample and there is no fill-up to average over.

**And the dwell dependence itself, measured with the room already lit before the camera arrives** —
close camera, cut at measured frame 3,000, shot at 4,200, so the arrival has twenty seconds:

| | before | after |
|---|---|---|
| run-to-run floor | 2.755 over 69,065 px | **2.284 over 47,788** |
| arriving against dwelling | 3.336 over 99,398 px | **3.173 over 88,589** |

**What it costs, and there is one number and it is not nothing.** Flying at 1440p, two interleaved
rounds of each arm: faces **6.691 / 7.349 before against 7.258 / 6.529 after**, total GPU 13.719 /
14.298 against 14.088 / 13.372 — inside each other's spread, and provably rather than luckily: no
face lives long enough while flying to reach even 128 far samples, let alone 512. A static camera's
first 900 frames are equal too (faces 1.926 against 1.932). The cost is entirely in the tail:

| close camera, frames 1,350-2,700 | before | after |
|---|---|---|
| faces pass | 0.607 ms | **1.427 ms** |
| total GPU | 3.506 ms | 4.427 ms |

A face that has stopped costs nothing, and this makes a face stop after about forty seconds of
standing still instead of ten. **Both are far inside the pass's 4.40 ms budget and neither is a moving
frame**, which is the case the budget is written for — but it is a real cost and it should not be
discovered later. **The cheap way to reclaim it is known and deliberately not built here**: a face
only needs the extra turnovers while the light it is sampling is still moving, and `bounce_radiance`
can already see whether the face it landed on has finished, in a record it has already loaded. That
would let a face stop at 128 once its own sources are silent. It is not built because the case that
costs is an interior, where every ray lands on another face that is also waiting — so the wavefront
has to start somewhere, and nothing has measured where.

**What this does NOT fix, and it is the other half of what the player described.** "Way betterly
rendered" is also a sample count: a face the camera has just revealed carries **46 sun samples against
203**, and the sun's counters halve at `kFaceWindow` rather than growing for ever, so a fresh face
catches up in about seventeen seconds and then matches exactly. That is latency, it is bounded, it is
the same everywhere, and it is R5's — filtering across neighbouring faces is what lets every term here
converge on fewer samples. What is closed is that the answer a face converges TO no longer depends on
when it was claimed.

**Gates**: 515 tests, 18.67 M assertions; `tools\darkroom.ps1` BLACK clear and with fog;
`--validation` clean; `GPU mirror matches`, *leaf for leaf*, *mask for mask*.

| # | Decision | Kind | Why |
|---|---|---|---|
| D553 | **Changing what a word MEANS makes every rule written about it suspect, and one of them was wrong** | trap | Four places touch the bounce words and three of them were obvious: the composite, the gathering ray and the accumulate. The fourth was the edit reset, which scaled words 9-11 by `seed / far_n` with a comment explaining that it had to, *"because it is a SUM over that same count"* — true when it was written, and after this change it divides a MEAN by four on every edit and reads as the room going dark whenever the player chisels. It was found by reading every writer of those words rather than by a picture, and the picture would have been slow to accuse it: an edit already reopens half the terms in the room. The general form is worth more than the fix — **a rule written about a quantity outlives the representation it was written for**, so the sweep after a representation change is over the rules and not over the compiler errors, of which there were none. The gate is that an edited room is not darker: enclosed camera under `--chisel 60,16`, mean pixel **127.341 after against 127.177 before and 126.368 unedited**, with speckle 21.80 against 21.95 |

**The four writers, so the next change to this record has the list**: `face_light_seed` (copies the
ancestor's mean, and does NOT scale it — the lamps beside it are still a sum and are still scaled,
which is why the two blocks look different), the far-ray accumulate, the edit reset, and the
`edit_min.w == 2` path's counterpart in `face_work_of`. The readers are `resolve.comp` and
`bounce_radiance`. `kFaceLightWords` in `shaders/face_terms.glsl` is the one place the layout is
declared, and D534 is what happens when it is not.

## D554 — the coarse face keeps the light, and a gathering ray may read it

*R9f's first half, and the first thing in R9 that a player will see rather than measure. The store
keeps the coarse pyramid after the fine faces under it are gone, and a ray that lands on a surface
with nothing to say reads the coarse face standing over it instead of returning black.*

**What was wrong, and it was written down in the code that caused it.** A stand-in is claimed only
when a fine face under it is NEW (`claim_stand_in`, from the feedback report in `main.cpp`), and
`FaceStore::last_read_` is stamped by a claim and by nothing else. So on a camera that has stopped
discovering geometry — which is every camera a few seconds after it stops moving — a stand-in is
never stamped again and is the COLDEST record in the store. After `cold_frames` it is evicted while
every one of its children is still live. The comment at that claim site had already reasoned about
keeping one warm and dismissed it: *"a stand-in whose children are all live is a stand-in nothing
reads"*. That is true, and it is about the wrong moment. The moment that matters is the one after:
the camera leaves, the children go cold too, and what the room has to be rebuilt from is the thing
that was thrown away first.

**Measured, close camera, 1280x800, `--settle`, frame 900, two interleaved rounds of one build, same
content hash `766f2fd63f1a01c4` in every run:**

| | control (`--no-coarse-keep --no-coarse-bounce`) | R9f |
|---|---|---|
| coarse faces live in the store | **0** and **37** of ~711,000 | **21,794** and **21,788** |
| stand-ins given up over the run | 21,796 / 21,785 | **0** / **0** |
| faces by level | 0 and 1 only | 0, 1, **3, 4, 6** |
| gathering rays that found no light and had a coarse face that did | 2,881 of 32,153 (9.0%) | **8,291 of 27,016 (30.7%)** |
| faces pass | 1.792 / 1.947 ms | 1.756 / 1.547 ms |

The control arm holds **nothing at all above level 1**. Every stand-in the run ever claimed was
evicted, and the level histogram is the proof that needs no theory behind it.

**What the player sees, which needed an instrument that did not exist.** `--cut` is now REPEATABLE,
because one cut asks what ARRIVING somewhere costs and two ask what LEAVING costs: stand at A, cut to
B for longer than the store's cold window, cut back to A, and diff against a run that never left.
That is R9f's own gate wording — *walking out of a lit room and back finds it lit* — and one cut
cannot express it, which is why the case a player reported as the world relighting itself had never
been measured. Enclosed camera, away at frame 300, back at 1,200:

| three frames after coming back | control | R9f |
|---|---|---|
| the card's own provisional stand-ins claimed that frame | **3,137** | **99** |
| speckle | 34.35 | **19.58** |
| fireflies | 1,494 | **387** |

A provisional stand-in is the most expensive face in the renderer: it re-claims itself every frame,
so it can never accumulate, and it takes a fresh unbounded ray and a fresh lamp burst every frame it
is needed (D316–D318, and D502 is the picture it draws). Thirty-two times fewer of them is the
blockiness going, and the speckle figure beside it is the same fact as a number.

**The mean difference at that instant does NOT improve (46.0 against 45.2), and the reason is worth
recording rather than hiding**: three frames after a 900-frame absence the NODE POOL is still
rebuilding, so most of that difference is geometry drawn from coarse nodes and has nothing to do with
light. By return+60 the geometry is back and the difference is 7.291 → **6.265**. Two systems recover
at different rates and only one of them is this change's — which is also the warning for the next
person measuring a reveal: a picture taken in the first few frames after a cut is a photograph of
residency, whatever it was pointed at.

**The converged picture moves, permanently, and that is the point rather than a side effect.** Frame
2,700, every face silent in both arms, close camera: mean pixel **133.508 → 140.043**, speckle
**45.549 → 38.504**, the two pictures 7.431 of 255 apart over 333,886 pixels against a run-to-run
floor of 2.85. A gathering ray that lands on a real surface and returns nought is saying that surface
emits nothing, which is simply false; it now reads that surface's own measured light at 25 cm instead
of at 3 cm. The speckle falling is the second half of the argument: a term that was a random mixture
of "the light there" and "black because the store forgot" is noisy by construction.

**It errs bright where the old answer erred dark, so the gate that exists for exactly that was run
first**: `tools\darkroom.ps1` and `-Fog` are both **BLACK, brightest channel 0 of 255, every pixel**.
No light is invented where there is none; what is recovered is light that was measured and thrown
away.

**Where it does most and least, converged, one round each:**

| frame 2,700 | control mean pixel | R9f | picture apart | speckle | fireflies |
|---|---|---|---|---|---|
| close | 133.508 | **140.043** | 7.431 over 333,886 | 45.5 → **38.5** | 81 → 81 |
| enclosed | 126.428 | **127.574** | 3.276 over 72,140 | 17.0 → **16.1** | 36 → **9** |
| outdoor | 161.598 | 161.713 | 0.214 over 6,565 | 16.4 → **16.2** | 288 → **171** |

Most where the store forgets most, nothing outdoors where the rays reach sky, and quieter in all
three. **Gate: the 42-run grid moves +0.17%** — 149.4 → 149.7 ms over 21 rows a side, same content
hash on every row — with speckle **216.5 → 201.5, −6.9%** and fireflies 1,711 → 1,325. Two rows
flagged over 3% and both are noise, shown by re-running them (trap 9): close/deck reads 4.483 / 4.679
/ 4.527 in the control against 4.725 / 4.564 / 4.504, and the arms interleave inside each other's
spread. Flying at 1440p the arms overlap: faces **6.31–6.69 against 6.54–6.91 ms**, with the sun
stride and the sun samples per face identical, which is the check D527 says to make before reading
either number.

| # | Decision | Kind | Why |
|---|---|---|---|
| D554 | **A coarse stand-in is evicted only under pressure, never merely for going cold** | rule | It is the coldest record in the store by construction and the one everything else is rebuilt from. The pyramid is 2.9% of the store on the close camera and 9.1% outdoors, it is bounded by the world's surface at 25 cm rather than by the screen, and under pressure it is spent with everything else — a stand-in held through a full table would be the store refusing a face a pixel wants in order to keep one nobody is looking at, which is D502's picture through a new door |
| D555 | **A gathering ray that finds no light walks UP, and stops at the first ancestor with an answer** | rule | Three hash probes on a ray that had already found nothing. The walk starts at the level the marcher stopped at, which the ray's own footprint chose, so "three levels up" is bounded by the distance rather than by a constant — which is §8 R9's second risk (*a fold read too coarsely is flat rather than noisy*) answered by construction instead of by a rule somebody has to remember. The albedo and the normal still come from the HIT: a stand-in lends its measurement of the light and never its idea of what is there |
| D556 | **The stand-in flag is a host-side array, not a bit of `packed`** | trap | The third time this exact decision has had to be made (D295, D528). `packed` is mirrored and the uploader sends whole records for dirty slots, so a host-side flag change on a live record sends the host's zeroed counters over the light the card accumulated. D528 measured that at 29,882 faces losing their light per flight, with the picture, the mirror and every audit still reading right |

## D557 — the third door into "a budget divided by the wrong population is a silent quality setting"

**This change looked free and was not, and the number that said so is on no pass table.** The sun's
stride is its ray budget divided among the faces that want one. A kept stand-in is a PRIMARY face — a
pixel asked for the fine face under it — so 21,799 of them went into that denominator, and the stride
went from 5 to 6 on the close camera. What that cost, at frame 2,700:

| | control | R9f before the fix | after |
|---|---|---|---|
| faces that have finished their ambient term | 475,632 of 476,230 | **107,582 of 497,656** | 475,855 of 475,855 |
| faces pass | 1.564 ms | **1.553 ms** | 1.425 ms |

**The timing was the same to a hundredth while a quarter as much had converged**, which is D527's
sentence exactly and the third route into it — D527 was the off-screen set, its successor was the
watermark counting evicted slots, and this is the coarse pyramid. The far ray needs `kBounceMin` = 512
samples at one per stride frames, so 512x5 = 2,560 lands before frame 2,700 and 512x6 = 3,072 does
not: a threshold sitting exactly where the measurement was taken, which is why the effect was fourfold
rather than the 20% the stride change looks like. **A convergence figure that is a step function of a
budget is one to measure at two frame counts, not one.**

The fix is to subtract the pyramid, as D527 subtracted the off-screen class, on the same argument: a
stand-in nobody is reading has `may_cast` false and casts nothing at all, so it has no business in a
budget for rays. A stand-in a pixel IS reading is not subtracted back, deliberately — that is the
transient where its fine children are missing, and a face standing in for five hundred and twelve
others while they are found should be refreshed often rather than rationed.

**And the audit line had the same fault**: those 21,783 faces read as *still bursting* while costing
not one ray, because they were counted in `ambient_live`. They have a line of their own now (*the
coarse pyramid on the card*), which is the argument the secondary block already carried, one class
along: a counter that gives one number to two states sends the next reader to the wrong pass.

| # | Decision | Kind | Why |
|---|---|---|---|
| D557 | **Any face that cannot cast comes out of the stride's denominator** | rule | Three separate populations have now been wrongly included in it. The general form is the one to keep: the sun's stride is a silent quality setting, and the only defence is to print a convergence figure beside every timing — `ambient on the card`, `sun samples each` and the stride itself are all on the audit for this reason |
| D558 | **An ignorance stop carries no face key, so nothing can walk up from it** | finding | `node_face_hit` runs at the leaf hit and nowhere else, so a ray stopped by a cell the pool has not built has `face_level == kNoFaceLevel`. R9f's other clause — *a ray that reaches an unbuilt region gets light rather than nothing* — therefore needs the marcher changed before it can be attempted, and that is a bigger change than this one. Recorded because the probe word for it was written, could not be filled, and is left named rather than removed |

## D559 — the gathering ray's own counters, and what they say

**The light pass had three audit lines about what it has FINISHED and none about what its rays are
LANDING on**, so the one question R9 exists to answer had no number and could only be guessed at from
a picture. `kLightProbeWords` in `shaders/node.glsl` is one word a question over the whole dispatch,
cleared before every face dispatch and read back at the screenshot audit — so it is a rate over the
frame rather than a lifetime total dominated by the startup transient, which is the same reason every
other figure in this project is taken under `--settle`.

The dials for it live in word 0 of that same buffer rather than in the push block, because `NodePush`
is exactly 128 bytes full and 128 is what Vulkan guarantees, which is what the Steam Deck gives.
Changing what an existing field means was the alternative, and D553 is the standing measurement of
what that costs. Word 0 is written with `vkCmdUpdateBuffer` and the counters are cleared with a
separate `vkCmdFillBuffer`, so "the host owns this word and the card owns the rest" is a property of
the code rather than a rule somebody has to remember (D528).

What it says on the close camera, settled, with the rule on: **79,310 gathering rays a frame, 47.3%
reaching sky, 18.6% landing on a lit face, 19.9% on a surface with no face in the store at all, 14.2%
on a face that has measured nothing yet** — and 30.7% of that last third answerable by the coarse face
above them. Flying at 1440p the same line reads 21.1%, because the pyramid is being built as fast as
the camera reveals it. **A third of what the bounce integrates is still nothing at all**, and that is
the size of what R9c and R9f–R9h have left to recover.

It also carries the histogram R10's open item has never had: *gathering rays stopped by unbuilt
geometry, by level*. On this building it is a handful a frame, all of them at level 3.

**Its own cost is measured rather than assumed**, because an instrument whose cost is unknown is
trap 20 waiting to happen: flying at 1440p, two interleaved rounds of `--no-light-probe` against the
default read **6.383 / 6.245 ms against 6.329 / 6.327** on the faces pass, which is the arms inside
each other's spread. One atomic on a handful of words per unbounded ray is nothing beside the march
that produced it.

| # | Decision | Kind | Why |
|---|---|---|---|
| D559 | **The probe is read in the audit's FIRST submit, not its second** | trap | The second is nested inside "the mirror matched" and "the store fits in the staging ring", and a counter about what rays found has nothing to do with either. D529 is that fault: the audit that could not run and the audit with nothing to report were one picture, and the numbers went missing from the one case that costs |
| D560 | **`--cut` is repeatable** | instrument | One cut measures arriving; two measure leaving and coming back, which is what a player does and what R9f's gate is written about. A cut whose frame is not after the one before it is WARNED about rather than silently reordered, because a harness that writes them in the wrong order has measured something nobody asked for and both arms would look clean — trap 15. Writing the harness for it walked straight into trap 4 as well: a local `$a` in a script with a `[string]$A` parameter is the same variable, and every character of the joined string arrived as its own argument |
