# 00 — Vision

*Revised after answer round 1.*

## One line

WorldShaper is a voxel creation game where every atom of the world is a real, simulated, editable voxel — and the player can shape all of it: terrain, materials, creatures, machines, characters, and the rules themselves.

## Scale constant

**1 meter = 32 voxels.** One voxel = 3.125 cm. One cubic meter = 32,768 voxels. The player is exactly 2 m = 64 voxels tall, and characters are built at that same resolution (answer G1).

Baked into physics units, chunk sizes, detail metrics and network quantisation. Never changed after Stage 2 without a migration plan.

## Pillars

1. **Everything is a real voxel.** No parametric surfaces, no analytical shapes at runtime, no texture maps standing in for geometry. Sandstone is thousands of real voxels arranged to look like sandstone. Generation *uses* noise and fractals; the output is always real voxels you can dig out one at a time.
2. **Total customization, including the rules.** Worlds, characters, materials, machines, and whole game modes are player-authored (answers B1, F5, K5). Modding is not a feature bolted on — it is Lua scripting reaching every system, at runtime.
3. **Open-ended voxels.** Every individual voxel can have its own colour, transparency, tags, properties and state fields, without limit (answers C2–C5, C11). `oak` is a tag; whether it burns is a separate property; being "sandstone" is a naming convention, not an engine case.
4. **Everything simulates and everything is conserved.** Fluids with pressure, momentum and mixing; gases that mix, rise and are exactly conserved; full heat diffusion through solids, fluids and air; cloth, rope, soft bodies that soak and stain; rigid bodies that dent and fracture. One cohesive system. Matter is never silently created or destroyed.
5. **Infinite world, infinite render distance, no steps at all.** Detail is a continuous function of distance with no discrete transition anywhere in the math. A voxel renders at full individual detail whenever it covers one pixel — the physical maximum.
6. **Path traced by default, locked 30 on a Steam Deck.** Lighting is computed in world space on voxel faces rather than per screen pixel, which decouples lighting cost from resolution and removes most of the noise.
7. **Multiplayer that costs nothing, forever.** Up to 32 players, no dedicated server, no host, no port forwarding, no subscription, no third-party service that can shut down or start charging.

## Non-negotiables

- **Licensing:** the game ships under **MIT-0** as open source (answers A10, A12, O1), eventually on Steam (answer A11). Dependencies must be permissive (MIT/BSD/Apache-2.0/zlib/ISC/CC0/public domain) so nothing contaminates that. See `08-tech-stack-and-licensing.md`.
- **Performance is the project's pass/fail criterion.** You named bad framerate, low resolution and short detail range as the things that would make this a failure (answer N5). Budgets in `09-performance-budgets.md` are enforced, per stage, on real Steam Deck hardware.
- **Minimum spec is Steam Deck** at 1280×800 / 30 FPS locked, dynamic resolution allowed (answers A6, D1, D2). Development target is the RTX 5060 Ti 16 GB machine at 1440p.
- **GPU-accelerated but multiplayer-safe:** all shared-state simulation is integer-only and deterministic. Lua may use floats because scripts emit ops rather than mutating state.
- **Windows first**, Linux/Steam Deck kept building continuously. No macOS (answer A5).

## Explicit non-goals for v1

- Audio (answer E16 — lands in Stage 23).
- Weather and time-of-day systems (answer B9).
- Vegetation and pre-authored structures (answer F9 — the world-gen node editor lets players populate worlds with their own clips instead).
- Voice and text chat (answer J8).
- Localisation (answer L6), colourblind modes (answer L5 — remappable controls only).
- Region permissions (answer J9).
- Anti-cheat (answer J7 — only when survival arrives).
- Console ports, mobile, a pre-authored campaign.

## Success criteria for v1

- A player joins a friend's world by pasting one invite code, in under 15 seconds, with no configuration.
- A player digs a 100 m tunnel, floods it, freezes the water, walks away for an hour, comes back and finds it exactly as they left it — with no matter lost.
- A player sculpts an arbitrary creature out of voxels and it walks, unaided, with a procedurally generated skeleton and gait.
- A player builds a working piston-driven vehicle, stands on it, and jumps while it moves, carrying its momentum.
- A player writes a Lua mod that adds a new material and a new reaction, and plays it with friends who did not install it manually.
- All of the above at a **locked 30 FPS on a Steam Deck**, and 60+ at 1440p on a mid-range desktop.

## How this project is run

Answers A2, A3 and N7 set the working model, and it shapes everything:

- **You do not write code.** I write all of it. Every stage ends with an executable you double-click.
- **Automated tests are the safety net**, because there is no second engineer reviewing. This is why Stages 0–2 are heavily weighted toward test infrastructure — which you explicitly authorised (answer N6).
- **Your job** is design calls, playing checkpoint builds, and telling me what feels wrong. That is the valuable half.
- **Explanations** live in `12-plain-english.md`, written without jargon and updated every stage.
