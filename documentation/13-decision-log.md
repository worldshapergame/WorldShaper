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

## Open items carried forward

- **O21.** Link to the deprecated WorldShaper repository (UI style reference only).
- Round-1 and round-2 questions are otherwise closed. Round-3 questions will be raised per stage as they arise.
