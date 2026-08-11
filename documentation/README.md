# WorldShaper — Documentation

This folder is the source of truth for design, architecture, and planning. Code follows these docs; when reality disagrees with a doc, the doc gets updated in the same change.

## Index

| File | Purpose |
|---|---|
| [00-vision.md](00-vision.md) | What the game is, pillars, non-negotiables, explicit non-goals |
| [01-open-questions.md](01-open-questions.md) | Every unanswered design/tech question, grouped and numbered |
| [02-architecture-overview.md](02-architecture-overview.md) | System map, module boundaries, data flow, threading model |
| [03-voxel-data-model.md](03-voxel-data-model.md) | Bricks, chunks, palettes, materials, tags, properties, sparse state |
| [04-rendering.md](04-rendering.md) | Continuous LOD, visibility, face-cache path tracing, refraction/dispersion/caustics |
| [05-simulation.md](05-simulation.md) | Cellular automata, conservation of matter, fluids/gas/sand, rigid & soft bodies |
| [06-multiplayer.md](06-multiplayer.md) | Serverless networking, NAT traversal, determinism, reconciliation |
| [07-roadmap.md](07-roadmap.md) | Staged build plan with playable checkpoints and exit criteria |
| [08-tech-stack-and-licensing.md](08-tech-stack-and-licensing.md) | Language, API, third-party libs, license audit |
| [09-performance-budgets.md](09-performance-budgets.md) | Hard numbers per frame, per tier of hardware |
| [10-glossary.md](10-glossary.md) | Precise definitions — read this before arguing about a term |
| [11-reality-check.md](11-reality-check.md) | Where the stated requirements conflict with physics/hardware, and the resolution |
| **[12-plain-english.md](12-plain-english.md)** | **How the whole game works, in plain language with no jargon. Start here.** |
| [13-decision-log.md](13-decision-log.md) | Every decision made, when, and why |
| [14-ui-style.md](14-ui-style.md) | The visual language: no colour, blurred glass, per-pixel opposing ink, tooltips |
| [17-crash-reports.md](17-crash-reports.md) | What a crash writes down, where it puts it, and how to prove it works |
| [18-overnight-loop.md](18-overnight-loop.md) | Running the game on itself while you sleep: loop.bat, the journal, and how to stop it cleanly |
| [19-auto-quality.md](19-auto-quality.md) | What costs frame time, the ladder of quality levels, and the benchmark the first run takes. **Its path-traced figures are withdrawn — see 21 §6** |
| [20-clip-forge.md](20-clip-forge.md) | A clip is a description, not a box of voxels: the field, the vocabulary, weathering, and the instruments that say whether what came out is what was meant |
| **[21-renderer-rewrite.md](21-renderer-rewrite.md)** | **The from-scratch rewrite of the path tracer, the chunk system and streaming. §8 is the work plan, §8.0 is where it stands** |
| **[22-rewrite-handover.md](22-rewrite-handover.md)** | **Start here to pick the rewrite up cold: what is done, what is next, and every trap that has already cost a day** |
| [23-shell-and-libraries.md](23-shell-and-libraries.md) | The shell: the title the game opens on, docked windows, libraries as real folders, the community browser, and the two views of one editor. Stage 15's specification |

## Status

**Stages 0, 1 and 2 done.** `build.bat` / `run.bat` / `test.bat` at the project root. 523 tests, 18.1 M assertions passing; zero Vulkan validation warnings. Verified on the RTX 5060 Ti dev machine; Steam Deck validation is deferred (no hardware — decision D62).

Two headless audits run in CI and in `test.bat`:

- `--ticks 1000000` — a million random ops against a real world, every invariant checked, the matter ledger reconciled against a full recount, byte-identical save round trip. **7.5–8.4 M voxel writes/second, 0.437 bytes per voxel.**
- the node pool's three audits, which run at every screenshot rather than in a mode of their own: `GPU mirror matches` (the card holds what the pool holds), *the node pool agrees with the world, leaf for leaf* and *mask for mask*. They replace `--stream-frames`, the chunk-mirror audit R1e deleted along with chunk residency itself.

**Stages 0–4 done.** `run.bat` opens on the scripted test scene and you can fly around it. The ray marcher resolves detail per pixel as a continuous function of distance, with an ordered dither between neighbouring levels so there is no discrete transition anywhere in the maths. Streaming is driven by what the renderer can see: the marcher reports the chunks it wanted and could not find, and residency follows the view rather than the camera.

Dev machine, 2560×1440, after streaming converges: **3.0 ms** standing in the scene with all 98 chunks resident, **0.27 ms** looking at the whole thing from 900 m with 4 chunks resident. Memory follows what the view needs, not where the camera is.

**Stage 15's shell — its first five steps — is in.** The game opens on a **title**, not in a world:
two buttons, a room behind them, and nothing loaded until somebody asks for something to be loaded.
Docked windows re-dock to any edge and split one; every number is a slider you can double-click and
type past the end of; the worlds library is a file manager over the real folder, with an author name
written into every file that travels with every copy of it. Leaving a world tears it down and comes
back to the title, which is the first thing to exercise the many-worlds rule at all. See
[23-shell-and-libraries.md](23-shell-and-libraries.md) §9 for what is built and the five things that
are not.

Next: **Stage 5 — the chisel**, and the `.wsworld` container that Stage 15 still owes.

Answer rounds 1 and 2 are complete and folded into every document. One question remains open: **O21**, a link to the deprecated project for UI *style* reference only.

Locked: C++20 + Vulkan 1.3 + Lua · Windows first, Steam Deck as the performance floor · Unlicense, open source · 20 Hz integer simulation · 32 players, no host, no infrastructure · 24 stages, 19 playable checkpoints.

## Working rules

1. **No unresolved question blocks progress silently.** If a stage needs an answer, it is listed in `01-open-questions.md` with the stage that blocks on it.
2. **Every stage ends in something runnable.** Not every stage ends in something *fun*; the ones that do are marked `PLAYABLE`.
3. **Performance is a feature with a number.** Every subsystem has a budget in `09-performance-budgets.md`. Exceeding it is a bug.
4. **Multiplayer is not a later problem.** Any system that mutates world state must be expressible as a deterministic, replayable operation from day one, even before networking exists.
5. **Conservation of matter is an invariant, not a goal.** Tested in CI with a headless mass-audit harness.
