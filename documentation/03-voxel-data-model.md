# 03 — Voxel Data Model

*Revised after answer round 1. Core change: **per-voxel identity via interned voxel types**, which delivers "every voxel can have its own color, properties and tags" at near-zero cost for the 99.99% of voxels that are not actually unique.*

Everything downstream — render speed, memory ceiling, network cost, simulation throughput — is decided here. This is the most important document in the folder.

## 1. Spatial hierarchy

| Level | Size | Contents |
|---|---|---|
| **Voxel** | 3.125 cm | one palette index |
| **Brick** | 8³ voxels = 25 cm | occupancy bitmask + palette + packed indices |
| **Chunk** | 32³ bricks = 256³ voxels = **8 m** | sparse brick octree (5 levels) + metadata |
| **Node (L+1…L+n)** | 2× per level, unbounded upward | sparse octree above chunks, for distant rendering |

Coordinates are **signed 64-bit integers in voxel units** (answer C12): ±2.8×10¹¹ m of world, exactly representable. Gameplay math near the player uses a **floating origin** so 32-bit floats never lose precision.

The world is one **sparse octree with hashed nodes**. Empty space costs zero bytes. The tree grows upward as the player travels — no world boundary, infinite downward too (answer F8).

## 2. The voxel type — how "per-voxel everything" is affordable

You asked for per-voxel properties, per-voxel color, per-voxel transparency, per-voxel tags, and unlimited per-voxel state fields (answers C1–C5, C11). Taken literally at 3 bytes of color plus a property table per voxel, a billion voxels is tens of gigabytes. The resolution is **interning** (also called hash-consing):

> A voxel stores a **32-bit VoxelTypeId**. A VoxelType is an immutable record of *everything* a voxel can be. Two voxels that are identical in every respect share one id automatically. Making a voxel unique allocates one new record — and only that one voxel pays for it.

```
VoxelType  (interned, deduplicated world-wide)
  behaviourId : u32   -> BehaviourRecord   // what it IS and DOES
  visualId    : u32   -> VisualRecord      // what it LOOKS like

BehaviourRecord (~48–96 bytes, aggressively deduplicated, may auto-merge)
  baseMaterial, tags, physical properties (density, friction, phase,
  hardness, ignition/melt temperature, conductivity, gravity warp, ...),
  reactions, scriptRef

VisualRecord (16 bytes, deduplicated but NEVER merged)
  color rgb8 · roughness · metallic · ior · emissive · translucency · absorption
```

**Why the split (answer O4).** You want auto-merging to reclaim space, but *only for non-visual data* — colour and material appearance must support billions of unique combinations. So:

- **Behaviour** records are few by nature (how many genuinely distinct physical substances does a world have?) and may be merged when two are imperceptibly different, reclaiming space.
- **Visual** records are never merged and never rounded. When a brick's colours stop fitting a palette, the brick switches to a **direct visual layer** — 4 bytes per voxel, allocated only on that brick. A billion uniquely coloured voxels costs ~4 GB of visual layers *and only in the bricks that actually contain them*; a billion identically coloured ones cost 16 bytes.

That delivers "billions of voxels each with their own colour and material parameters" honestly, while still letting behaviour data stay compact.

**Cost in practice**

| Situation | Distinct types | Memory |
|---|---|---|
| A wall of one painted colour | 1 | 64 bytes total, for any number of voxels |
| Procedural sandstone (noise-coloured) | ~100–500 | ~32 KB, for the whole world |
| A hand-painted portrait, every voxel a different shade | 1 per voxel | 64 bytes per voxel — genuinely unique data genuinely costs |
| An entire billion-voxel world where every voxel is unique | 1 billion | ~64 GB — not possible, and nothing real does this |

So the rule is honest and simple: **sameness is free, uniqueness costs.** No arbitrary limit is imposed; the type table just grows, with a memory readout in the HUD and a warning when a world crosses a configurable threshold.

**Limit:** 32-bit ids = 4.29 billion distinct voxel types per world. That is the practical meaning of "infinite" here (answer C1). If you ever want more, the field widens to 64 bits at the cost of ~15% more brick memory.

## 3. Brick encoding

A brick holds 512 voxels. Encoding is chosen per brick automatically on write, and re-chosen when it changes:

| Form | When | Cost |
|---|---|---|
| **Uniform** | all 512 voxels the same type (air, solid rock) | 8 bytes total |
| **Bitmask + 1 type** | two types, one being air | 64 B mask + 4 B |
| **Palette 2/4/16/256** | ≤N distinct types | 64 B mask + (1/2/4/8 bits × 512) + palette |
| **Direct** | >256 types in one brick (hand-painted detail) | 64 B mask + 2048 B |

Typical results:

- Underground rock: **uniform**, 8 bytes / 512 voxels.
- Procedural terrain surface: 64 + 128 + palette ≈ **220 B / 512 voxels → 0.43 bytes/voxel**.
- Player-built painted art: 8-bit palette ≈ 1.2 bytes/voxel.
- Worst case, every voxel unique in a brick: 4 bytes/voxel + the type records.

The 64-byte occupancy bitmask on every non-uniform brick is the hottest structure in the engine: one 64-byte read lets a ray skip 512 voxels, and it lets simulation test neighbourhoods with bit operations instead of memory fetches. **On Steam Deck, memory bandwidth is the binding constraint — this bitmask is why the design fits in 88 GB/s.**

## 4. Materials vs. voxel types

A **material** is an authoring concept and a tag source; a **voxel type** is the runtime truth.

- `oak_plank` is a material: a tag set (`wood`, `flammable`, `buildable`), default properties (density, hardness, ignition temperature), and a colour palette.
- A **material brush / pattern generator** (answer C9) is a function — noise, fractal, strata, voronoi, layered — that paints *real voxels*, choosing a colour and a base material per voxel. "Sandstone" is a brush that emits `stone`-tagged voxels in a sandstone-looking arrangement.
- There is no "sandstone block" anywhere in the engine. Only stone voxels arranged sandstone-ly. Exactly as you specified.
- When the player digs it, the *brush identity* is what goes to the inventory ("sandstone", answer C10) — recorded as a lightweight provenance tag on the voxel type, not as engine special-casing.

### Tags

Unlimited per voxel type. Two representations:

- **CPU / Lua:** unlimited, string-interned `u32` ids. Used by crafting, mods, UI, queries.
- **GPU:** a 256-bit bitset per type covering the tags simulation and rendering test every tick. Tag→bit assignment happens at world load from the tag registry — data-driven, never hardcoded.

### Properties

A registry, not a struct. Mods add properties without touching engine code (answers C7, C8, K5):

```
register("roughness", f16, 0.5)      register("ior", f16, 1.0)
register("metallic", f16, 0.0)       register("emissive", f16x3, 0)
register("translucency", f16, 0.0)   register("absorption", f16x3, 0)
register("density", u16, 1000)       register("friction", u8, 128)
register("phase", enum, SOLID)       register("hardness", u8, 128)
register("ignition_temp", u16)       register("melt_temp", u16)
register("conductivity", u8)         ...unbounded, from data files or Lua
```

A voxel type stores only what it overrides. The ~14 properties the GPU needs every frame are packed into a 32-byte struct indexed by type id; the rest live CPU-side and are fetched only by gameplay and script code.

## 5. Per-voxel dynamic state — unlimited fields, pay-for-use

Some things vary per voxel and change constantly: how hot it is, how full of water, how wet, how damaged, how charged, how stained. These are **per-brick layers**, registered like properties, allocated only on bricks that actually have them (answer C3: "infinite" fields, implemented as an open registry).

| Layer | Size when present | Allocated when |
|---|---|---|
| `temperature` u16 | 1 KB | brick is not at ambient (answer E7: full heat diffusion, air included) |
| `fill` u8 | 512 B | brick contains fluid or gas |
| `velocity` i8×3 | 1.5 KB | fluid with momentum (answer E5) |
| `wetness` u8 | 512 B | soaked cloth/soft bodies (answer E13) |
| `stain` u32 | 2 KB | fluid left a colour mark (answer E13) |
| `damage` u8 | 512 B | structural damage taken |
| `charge` u8 | 512 B | part of a logic circuit |
| `support` u8 | 512 B | structural integrity solve (answer E8) |
| *mod-registered* | any | whenever a mod writes to it |

A brick with no layers costs nothing extra. A still-water brick costs one `fill` layer. **State exists where it is happening, not everywhere** — which is how "any voxel can have any state" stays affordable across billions of voxels.

Genuinely one-off data (a sign's text, a container's contents, a Lua object handle) lives in a sparse `VoxelAttachment` hash map keyed by 64-bit voxel coordinate (answer C5).

## 6. Filtered hierarchy (the continuous-detail data)

Every node above brick level stores a filtered summary of its children, updated incrementally along the path of an edit (~30 nodes per edited voxel, not the world):

```
NodeSummary (16 bytes)
  avgColor      : rgb9e5          // correctly filtered — answer D11
  normalCone    : oct16 + angle8
  coverage      : u8              // partial solidity, drives edge anti-aliasing
  dominantType  : u32
  flags         : u8              // has emissive / has transparent / uniform
```

## 7. Unaligned voxel objects

Anything off the world grid — a falling boulder, a vehicle, a character, a flag, a logic component — is a **VoxelVolume**:

```
VoxelVolume
  bricks    : local sparse octree (identical format to the world)
  transform : fixed-point position (i64 voxel + i32 subvoxel) + i32 quaternion
  motion    : linear/angular velocity, fixed-point
  massProps : mass, centre of mass, inertia tensor — derived from voxel densities
  binding   : NONE | RIGID | SOFT_LATTICE | CLOTH | ROPE | ARTICULATED
```

Rendered by transforming rays into local space and marching the same brick structure, gathered under a small top-level BVH. Because the format is identical to the world's, every editing, simulation, material, and script path works on them unchanged. Cutting a building in half yields a VoxelVolume containing exactly the voxels that were there — no conversion, no loss.

## 8. Memory budget

Sized for **Steam Deck (16 GB shared, ~6 GB usable for the game)** as the floor and 16 GB VRAM as the dev target.

| Consumer | Steam Deck | RTX 5060 Ti 16 GB |
|---|---|---|
| Brick pool | 512 MB | 4 GB |
| Voxel type table | 64 MB (≈1M types) | 512 MB (≈8M types) |
| Hierarchy nodes | 96 MB | 768 MB |
| Face radiance cache | 192 MB | 1.5 GB |
| Simulation layers | 192 MB | 1.5 GB |
| Render targets | 90 MB | 400 MB |
| **GPU total** | **≈1.1 GB** | **≈8.7 GB** |
| CPU mirror + undo + staging | 1 GB | 3 GB |

All pools are fixed-size, allocated at startup from detected memory. **The game never allocates GPU memory during play.** Pool pressure causes eviction, never a stall.

## 9. Invariants (asserted in debug, tested in CI)

1. Occupancy bitmask exactly matches palette indices.
2. Node summaries equal the recomputed filter of their children.
3. The voxel type table is fully deduplicated — no two records with identical content.
4. Voxel types are reference-counted; a type with zero references is freed within N ticks.
5. Total matter per material per region changes only via an accounted ledger op.
6. Serialize → deserialize → serialize is byte-identical.
7. CPU reference and GPU implementations produce identical brick contents after the same op sequence.
