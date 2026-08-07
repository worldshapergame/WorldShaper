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

## Status

**Stages 0, 1 and 2 done.** `build.bat` / `run.bat` / `test.bat` at the project root. 146 tests, 17.6 M assertions passing; zero Vulkan validation warnings. Verified on the RTX 5060 Ti dev machine; Steam Deck validation is deferred (no hardware — decision D62).

Two headless audits run in CI and in `test.bat`:

- `--ticks 1000000` — a million random ops against a real world, every invariant checked, the matter ledger reconciled against a full recount, byte-identical save round trip. **7.5–8.4 M voxel writes/second, 0.437 bytes per voxel.**
- `--stream-frames 300` — a camera path over the scripted test scene, asserting after every frame that the GPU mirror is bit-identical to the world. **0.059 ms average residency update against a 0.8 ms budget**, 93–98% cache hit rate.

**Stages 0–4 done.** `run.bat` opens on the scripted test scene and you can fly around it. The ray marcher resolves detail per pixel as a continuous function of distance, with an ordered dither between neighbouring levels so there is no discrete transition anywhere in the maths. Streaming is driven by what the renderer can see: the marcher reports the chunks it wanted and could not find, and residency follows the view rather than the camera.

Dev machine, 2560×1440, after streaming converges: **3.0 ms** standing in the scene with all 98 chunks resident, **0.27 ms** looking at the whole thing from 900 m with 4 chunks resident. Memory follows what the view needs, not where the camera is.

Next: **Stage 5 — the chisel.** The first build you can actually make something in.

Answer rounds 1 and 2 are complete and folded into every document. One question remains open: **O21**, a link to the deprecated project for UI *style* reference only.

Locked: C++20 + Vulkan 1.3 + Lua · Windows first, Steam Deck as the performance floor · Unlicense, open source · 20 Hz integer simulation · 32 players, no host, no infrastructure · 24 stages, 19 playable checkpoints.

## Working rules

1. **No unresolved question blocks progress silently.** If a stage needs an answer, it is listed in `01-open-questions.md` with the stage that blocks on it.
2. **Every stage ends in something runnable.** Not every stage ends in something *fun*; the ones that do are marked `PLAYABLE`.
3. **Performance is a feature with a number.** Every subsystem has a budget in `09-performance-budgets.md`. Exceeding it is a bug.
4. **Multiplayer is not a later problem.** Any system that mutates world state must be expressible as a deterministic, replayable operation from day one, even before networking exists.
5. **Conservation of matter is an invariant, not a goal.** Tested in CI with a headless mass-audit harness.
