# 02 — Architecture Overview

*Revised after answer round 1: adds the Lua scripting layer, region-ownership authority, the many-worlds manager, and the diegetic UI layer.*

## Layer map

```
┌─────────────────────────────────────────────────────────────────────┐
│  Mods / Scripts     Lua sandbox — reads snapshots, EMITS OPS only    │
├─────────────────────────────────────────────────────────────────────┤
│  Game        player · tools · clips · characters · NPCs · diegetic UI│
├─────────────────────────────────────────────────────────────────────┤
│  World       world manager · op queue · region authority · save      │
├──────────────┬──────────────┬───────────────┬───────────────────────┤
│  VoxelStore  │  Simulation  │  Physics      │  Net                  │
│  bricks,     │  cellular    │  rigid/soft/  │  transport, NAT,      │
│  types,      │  automata,   │  ragdoll,     │  op replication,      │
│  hierarchy   │  reactions   │  collision    │  reconciliation       │
├──────────────┴──────────────┴───────────────┴───────────────────────┤
│  Render      visibility · face cache · path tracer · denoise · post  │
├─────────────────────────────────────────────────────────────────────┤
│  GPU         device, bindless, memory, pipelines, timeline sync      │
├─────────────────────────────────────────────────────────────────────┤
│  Core        allocators, jobs, fixed-point math, IO, log, tests      │
└─────────────────────────────────────────────────────────────────────┘
```

Dependencies point strictly downward. `Render` may read `VoxelStore` but never mutate it. `Simulation`, `Physics`, `Game` and `Scripts` mutate only through the op queue. This rule is what makes 32-player serverless multiplayer possible later without a rewrite.

**The scripting layer sits at the top on purpose.** Lua can read the world and request changes, but it can never write shared state directly — so mods may use floating point, randomness and anything else without endangering determinism (`11-reality-check.md` §8).

## The central idea: everything is an Op

Every mutation of shared world state — a brush stroke, a water cell moving, a piston extending, a tree burning — is expressible as an **Op**: a small, deterministic, serializable record.

```
Op = { tick, originator, kind, payload }
```

Two categories:

- **Intent ops** (small, networked): "player 3 applied a sphere brush of material X at position P with radius R at tick T". Bandwidth cost: ~32 bytes.
- **Derived ops** (never networked): the millions of cell transfers the simulation performs. These are *recomputed identically on every peer* from the same starting state and the same intent ops. They are never sent.

This is why the world can contain billions of voxels and still be networked over a home connection: **only intent crosses the wire.**

Consequences that must be respected from Stage 1, long before networking exists:
1. Simulation must be a pure function of `(world state, tick, intent ops)`.
2. No wall-clock time, no frame time, no `rand()` without a seeded, tick-derived stream.
3. No floating point in any code path that mutates shared state.
4. Ops carry the tick they apply to, not "now".

## Authority (answer J4 — distributed, no host)

The world is divided into **authority regions** (64 m cells). Each has exactly one owner peer at a time, which orders that region's ops, runs its scripts and NPC AI, and generates its terrain. Ownership is leased, transferred to the nearest stable peer, and re-elected deterministically if an owner vanishes. In single-player you own every region, so the same code path runs offline — no separate "singleplayer mode" to diverge and rot. Details in `06-multiplayer.md` §2.

## Tick model

- **Simulation tick**: **20 Hz fixed** (answer E1). Integer-only. Deterministic. Drives cellular automata, physics, ragdolls and logic.
- **Render frame**: variable rate, decoupled, interpolates between the last two ticks for smooth motion.
- **Face cache update**: budgeted per frame, not per tick. It is a cache; it may lag.

A frame never blocks on a tick and a tick never blocks on a frame. If the sim falls behind, it runs multiple ticks per frame up to a cap, then time-dilates (and warns) rather than spiraling.

## Threading

| Thread | Owns |
|---|---|
| Main | window, input, frame orchestration, GPU submission |
| Sim | tick loop; dispatches GPU sim work and CPU-side physics; runs owned-region Lua under a hard time budget |
| Job pool (N-2 workers) | terrain generation, meshing (if used), compression, save IO, network serialization |
| IO | disk, one thread, never blocks anything |
| Net | socket pump, ~1 kHz, lock-free queues to Sim |

Cross-thread data moves through single-producer/single-consumer ring buffers. There are no mutexes on hot paths.

## GPU work per frame

```
1.  Streaming        upload requested bricks, evict LRU          (async copy queue)
2.  Edits            apply this frame's brush/edit ops           (compute)
3.  Simulation       N cellular passes over dirty bricks         (compute, sim tick only)
4.  Hierarchy update rebuild mip pyramid for dirty bricks        (compute)
5.  Visibility       per-pixel hierarchical ray march            (compute)
6.  Face selection   collect visible face IDs, build update list (compute)
7.  Face shading     path trace from selected faces, budgeted    (compute)
8.  Face denoise     spatiotemporal filter in cache space        (compute)
9.  Specular pass    per-pixel refraction/reflection/dispersion  (compute)
10. Photons          caustic energy deposited into face cache    (compute)
11. Volumetrics      froxel fog, aerial perspective, gas media   (compute)
12. Composite        exposure, bloom, DOF, motion blur, tonemap, (compute)
                     TAA, temporal upscale
13. UI               diegetic panels + debug overlay             (graphics)
```

Steps 3–4 run only on sim ticks (20 Hz). Steps 6–8, 10 and 11 are budget-capped and degrade gracefully — they lose light-convergence speed, never framerate.

## Module boundaries (what each one is not allowed to know)

- `VoxelStore` knows nothing about rendering, networking, or gameplay. It is a data structure with an edit API and an invariant checker.
- `Simulation` knows nothing about the GPU *API* — it emits work descriptions that the GPU layer executes. This lets it run headless on CPU in tests.
- `Render` is read-only with respect to the world and can be entirely disabled (headless mode) for tests and dedicated-host scenarios.
- `Net` never touches voxels directly, only ops.
- `Scripts` cannot see the GPU, the network, or the file system. They see a world snapshot and an op-emitting API.

## Scripting boundary (answers A1, C7, C8, K5)

Lua 5.4, sandboxed, one VM per world, running on the Sim thread for regions this peer owns.

- **Read:** a consistent snapshot of the previous tick — voxels, types, tags, properties, entities, events.
- **Write:** queued ops only. Nothing a script does takes effect until the op is ordered and applied like any other mutation.
- **Budget:** hard per-tick time limit. Overrun suspends the script and reports it in the HUD; a bad mod cannot hitch the game.
- **Events:** scripts subscribe (`on_tick`, `on_place`, `on_break`, `on_reaction`, `on_damage`, `on_join`) rather than polling.
- **Hashing:** every loaded script and data file contributes to the world's content hash, so peers cannot silently run different rules.

Because of this boundary, mods are unrestricted in what language features they use and still cannot break determinism, conservation, or multiplayer.

## Many worlds (answer B3)

A `WorldManager` owns a list of worlds, each a single `.wsworld` container (answer K1). Only one world is resident at a time; switching tears down pools and rebuilds them rather than sharing state, so worlds can never contaminate each other. Clips, materials and mods live outside worlds in a shared user library.

## The shell (Stage 15 — `23-shell-and-libraries.md`)

Two modules sit above `Game`, and the layer rule applies to them unchanged: **the interface mutates
the world only through ops.** A library that renames a file touches the file system; a library that
stamps a clip emits an op like any other edit, and multiplayer replicates it for nothing.

```
ws_ui        docking, windows, widgets, the ink rule, input hit-testing, the library
             knows: core, game, platform, the file system
             does NOT know: the device, the network, the world's internals
```

`ws_library` is inside `ws_ui` rather than beside it (`src/ui/library.*`): a library is a file
manager and a file manager is a window, and splitting them would be two modules that only ever
appear together. It knows `platform` because the input snapshot and the audio device are there, and
it produces a **list of marks** rather than drawing anything — which is what lets the whole of it
be tested without a window, and what keeps the device out of it.

**The shell is a state machine over three states and a lifetime over one world** (D458). The
process owns the window, the card, the interface and its sound; an `Application` owns exactly one
world and is destroyed when that world is left. The rule above — *a world is torn down on the way
out, never shared* — is therefore structural rather than remembered: every pool in a second world is
new because there is nowhere for an old one to survive.

**The shell is a state machine with three states — title, library, world** (D441). The game opens on
the title and builds nothing until something is chosen, which is what first exercises the tear-down
path above: today there is exactly one world per process and it is built before the first frame. The
loading screen already covers the transition and already reads `shaders/ui.glsl`.

**A library is a file manager over the real folder** in `%LOCALAPPDATA%\WorldShaper\` (D445), not a
database with a folder underneath it — so nothing in the game may hold state about a file that the
file does not itself carry.

## Headless mode is a first-class citizen

From Stage 1, the whole game must be runnable with `--headless`: no window, no GPU (CPU reference simulation), fixed tick count, deterministic. This is how conservation-of-matter tests, determinism tests, and multiplayer desync tests run in CI. It costs a little discipline early and saves months later.

## Error philosophy

- Invariant violations (matter created, brick pool corruption, hierarchy mismatch) → assert and crash in dev, log + self-heal in release.
- Every subsystem exposes counters; the debug HUD shows them all with per-frame cost. Performance regressions are caught by looking, not guessing.
