# 10 — Glossary

Precise meanings. When a doc or a code comment uses one of these words, it means exactly this.

| Term | Meaning |
|---|---|
| **Voxel** | One 3.125 cm cube of matter. The atom of the world. Stores a palette index into its brick's palette of voxel types. |
| **Brick** | 8×8×8 = 512 voxels. The unit of storage, compression, simulation dispatch, and sleep/wake. |
| **Chunk** | 32×32×32 bricks = 256³ voxels = an 8 m cube. The unit of streaming, saving, and network reconciliation. |
| **Node** | Any level of the sparse octree. Nodes above brick level store filtered summaries of their children. |
| **Occupancy bitmask** | 512 bits per brick saying which voxels are non-empty. The hottest data structure in the engine — used by the ray marcher, collision, and simulation. |
| **Palette** | The small set of distinct values in a brick. Voxels store 1–8 bit indices into it. |
| **Palette entry** | A 32-bit VoxelTypeId. |
| **Voxel type** | The complete definition of one voxel — base material, colour, tags, property overrides, script handle. **Interned**: identical voxels share one record automatically. This is how per-voxel colour/tags/properties stay affordable across billions of voxels. |
| **Interning (hash-consing)** | Storing one copy of each distinct value and referring to it by id. Sameness is free; uniqueness costs exactly one record. |
| **Material** | An authoring concept and a tag source: tags, default properties, a colour palette, reactions. Voxel types reference it. |
| **Tag** | A named boolean label (`stone`, `flammable`, `conductive`). Unlimited on the CPU and in Lua; the 256 hottest are a GPU bitset. |
| **Property** | A named typed value (`roughness`, `density`, `ior`). Extensible registry; only overrides are stored. |
| **Layer** | Optional per-brick, per-voxel dynamic state (temperature, fill, velocity, wetness, stain, damage, charge, support, or any mod-registered field). Allocated only on bricks that use it. |
| **Attachment** | Sparse per-voxel one-off data keyed by coordinate — a sign's text, a container's contents, a script object. |
| **Pattern generator** | The thing a "material" is in the UI: a noise/fractal/strata function that emits real tagged voxels arranged to look like a substance. "Sandstone" is a pattern generator, not a block type. |
| **VoxelVolume** | Voxels not aligned to the world grid: a rigid body, a vehicle, a character, a piece of cloth. Same brick format, plus a transform. |
| **Op** | The atomic unit of world mutation: deterministic, serializable, tick-stamped. |
| **Intent op** | A small op describing what a *player* did. The only thing sent over the network. |
| **Derived op** | A mutation the simulation produced. Recomputed identically everywhere, never sent. |
| **Tick** | One fixed-rate simulation step. Integer-only, deterministic. Not a frame. |
| **Frame** | One rendered image. Decoupled from ticks, variable rate. |
| **Awake / asleep** | Whether a brick is being simulated this tick. Sleep is what makes "everything is simulated" affordable. |
| **Transfer protocol** | The propose → claim → vacate algorithm that makes conservation of matter structural rather than checked. |
| **Matter ledger** | Per-region, per-material totals used to prove nothing was created or destroyed. |
| **Atmosphere pool** | Per-region accumulator that absorbs gas too thin to be worth simulating per-voxel, while keeping it in the ledger. |
| **Face** | One of the 6 sides of a voxel (or of a hierarchy node) at a given detail level. The unit of lighting. |
| **Face cache / face radiance cache** | The world-space hash table storing lighting per face. The core of the renderer. |
| **Parent seeding** | Initializing a new face cache entry from its parent node's value so it is never black on first appearance. |
| **Visibility buffer** | Per-pixel record of *which face was hit*, with no shading. Decouples geometry from lighting. |
| **Beam optimization** | Low-resolution pre-pass that gives fine rays a conservative starting distance. |
| **Screen-space error** | The metric deciding how deep to descend the hierarchy per pixel. The mechanism behind continuous LOD. |
| **Hero wavelength sampling** | One random wavelength per pixel per frame, reconstructed over time — how dispersion becomes affordable. |
| **Photon deposit** | Adding caustic energy directly into a face cache entry. |
| **Fixed-point** | Integer arithmetic with an implied fractional scale. Mandatory for all shared-state math because it is bit-identical everywhere. |
| **Determinism** | Same inputs → bit-identical outputs on every machine. The property that makes networking cheap. |
| **Reconciliation** | Comparing per-chunk hashes between peers and repairing any mismatch. The safety net under determinism. |
| **Interest management** | Only sending a peer the ops for regions it cares about. |
| **Hole punching** | Two peers behind NAT simultaneously sending to each other so their routers open a path. |
| **Peer relay** | A third player forwarding packets for two peers that cannot connect directly. The free, serverless fallback. |
| **Tier (T0–T4)** | Hardware performance class. **T0 = Steam Deck** and must hold a locked 30. T3 = the RTX 5060 Ti dev machine. |
| **Perf gate** | A number a stage must hit before it is considered done. |
| **Clip** | A saved voxel creation — a building, a creature, a machine — stored in the player's library and stampable into any world. May be procedural. |
| **Chisel** | The build tool: hold a mouse button, the first corner is the voxel you look at, the second is a voxel a set distance from the camera; release to fill or carve the box between them. |
| **Authority region** | A 64 m cell of the world owned by exactly one peer, which orders its ops, runs its scripts, and generates its terrain. Replaces the idea of a host. |
| **Gossip tree** | The forwarding structure used to broadcast a region's ops without a full mesh, so upstream cost stays flat as players are added. |
| **Invite code** | A pasteable string carrying a username plus candidate network addresses — the entire multiplayer setup flow. |
| **Stochastic level blending** | Choosing between two adjacent hierarchy levels with dithered randomness per pixel so detail varies continuously with distance and there is no discrete transition at all. |
| **Atmosphere pool** | Per-region accumulator absorbing gas too thin to simulate per-voxel while keeping it in the matter ledger. |
| **Propose / claim / vacate** | The three-phase transfer protocol that makes conservation of matter a structural property rather than something checked and patched. |
| **Active ragdoll** | A character whose joints are motorised and balance-controlled, transitioning continuously from controlled movement to limp as damage accumulates — never a switch flip. |
| **Support propagation** | The structural integrity method: an integer support value spreading from anchored ground; voxels below threshold detach and fall. |
| **Op-emitting** | The rule that Lua scripts request changes rather than performing them, which is what lets mods use floats without breaking determinism. |
