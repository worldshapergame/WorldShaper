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

## Open items carried forward

- **O21.** Link to the deprecated WorldShaper repository (UI style reference only).
- Round-1 and round-2 questions are otherwise closed. Round-3 questions will be raised per stage as they arise.
