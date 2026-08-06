# 05 — Simulation

*Revised after answer round 1: **20 Hz** (E1), **GPU integer-only accepted** (E2), **fluids with pressure, momentum, current and mixing** (E4, E5), **gases mix, are buoyant and exactly conserved** (E6), **full heat diffusion including air** (E7), **structural integrity yes, my call on method** (E8), **auto-detach on cut** (E9), **no hard body limits** (E10), **dent and fracture** (E11), **Verlet** (E12), **fluids soak and stain** (E13), **explosions as pressure waves** (E14), **frozen regions remember exact state** (E3).*

One tick, one integer-only pipeline, everything in it. Fluids, gases, sand, fire, cloth, rope, rigid bodies, ragdolls and logic all move matter through the same conserved transfer protocol, so they interact correctly instead of being separate systems bolted together.

---

## 1. Tick

- **20 Hz**, fixed. Never tied to framerate. Rendering interpolates between the last two ticks.
- Pure function: `state(t+1) = f(state(t), t, intentOps(t))`.
- **Integer / fixed-point arithmetic only** in every code path that mutates shared state. This is what makes GPU simulation bit-identical across GPU vendors, which is what makes 32-player serverless multiplayer possible.
- All randomness from `hash(voxelCoord, tick, salt)` — stateless, position-derived, order-independent.
- Lua scripts do **not** run inside the tick's shared-state math. They run on the region owner and emit ops (see `06-multiplayer.md` §2), so mods may use floats freely.

## 2. Active set — why "everything simulates" is affordable

A brick is **awake** or **asleep**.

- Wakes when: edited, a neighbour brick changes across a shared face, a body touches it, its temperature is off-ambient, or a script pokes it.
- Sleeps after K consecutive ticks with zero transfers.
- Beyond every player's simulation radius, bricks **freeze exactly** (answer E3) and record the tick at which they froze. Nothing is lost — every fluid level, temperature, velocity and stain is preserved bit-for-bit.
- On return, the region **catches up** on the elapsed time (answer O2) — see §12. A *closed* world does not advance; only time the world was actually running counts.

A still lake is entirely asleep and free. Throw a rock in and a spreading front of bricks wakes, then re-sleeps.

**Global water at world-gen is created in a settled state** (answer B10) — oceans and lakes are born asleep and only become dynamic where disturbed. This is the difference between an infinite world that runs and one that does not.

## 3. Conservation of matter — the transfer protocol

The classic cellular-automata bugs are (a) two cells moving into the same empty cell → matter duplicated, (b) a cell moving out while the destination rejects it → matter destroyed. Both are eliminated structurally.

**Phase 1 — Propose.** Every awake cell computes its desired destination and writes a proposal into the *destination's* slot:

```
atomicMin(dest.proposal, (priority << 32) | sourceIndex)
priority = hash(sourceCoord, tick)
```

`atomicMin` is **order-independent**: the result does not depend on which thread arrives first, so it is bit-identical on every GPU at any occupancy. This is what lets us use atomics *and* stay deterministic.

**Phase 2 — Claim.** Each destination reads its winning proposal and **pulls** the matter in. Matter is never written by two parties.

**Phase 3 — Vacate.** Each source checks whether it was claimed. Claimed → it gives up exactly the transferred amount. Not claimed → nothing changes.

Swaps (dense fluid sinking through light fluid) use the same protocol with a reciprocal claim: both halves move or neither does.

**Every unit of matter is either where it was, or exactly one place else. Never zero places, never two.**

### The ledger

Independently, a **matter ledger** tracks totals per material per region, changed only by explicit accounted source/sink ops (creative placement, a drill, a printer — answer I6).

- Debug: assert and break, highlighting the offending brick.
- CI: `headless --audit-matter --ticks 100000` on a torture scene. Any drift fails the build.
- Release: logged and self-healed, never silently ignored.

## 4. Phases of matter

### Granular (sand, gravel, powder)
Falls; if blocked, tries diagonal-down cells chosen from the position hash so there is no directional bias or visible lean. Angle of repose from `friction`.

### Fluid — pressure, momentum, current, mixing (answers E4, E5, O10)

Each fluid voxel carries `fill` (0–255) and `velocity` (i8×3) layers, plus a **mixture** with an unlimited number of components (answer O10): one or two stored inline in the brick layer, further components spilling into a sparse per-brick component list. Typical fluid has one; muddy dyed seawater might have four; nothing caps it. Each component is ledger-tracked separately, so mixing, separating and evaporating are all mass-exact per substance.

- **Volume transfer** is integer and conserved exactly.
- **Pressure:** a bounded pressure value propagates through connected fluid, so water finds its level, rises in vessels, and flows through pipes — required for anything mechanical. Capped so a deep ocean cannot form a geyser.
- **Momentum:** velocity advects with the fluid and is damped by viscosity and wall friction. Water poured sideways keeps going sideways; a river has a current; a burst pipe sprays.
- **Mixing:** two fluids in one voxel are tracked as a mixture of up to 4 components with integer ratios. Mixtures inherit blended colour, density and viscosity, and separate over time by density (oil rises out of water). Mixture components are ledger-tracked individually, so mixing never loses matter.
- **Density-driven layering** comes free from the reciprocal swap.

### Gas (answer E6 — mixes, buoyant, exactly conserved)

Same machinery plus buoyancy from density difference against local air and a diffusion term. Gases mix as fluids do.

To keep exact conservation affordable, gas thinner than a threshold merges into a per-region **atmosphere pool** — one accumulated number per region per gas, still fully ledger-tracked, no longer costing per-voxel work, and drawn back out if a vacuum forms. Smoke thins realistically and is never deleted.

### Solid / soft solid
Static in the cellular pass; handled by §6 and §7.

## 5. Heat and reactions

`temperature` is a per-brick layer covering **solids, fluids and air alike** (answer E7). Heat diffuses using the same conserved-transfer protocol applied to thermal energy — energy in equals energy out, with conductivity from the material. Air carries heat, so a fire warms a room, hot air rises by buoyancy, and a sealed room heats up.

A **reaction** is a data-defined rule, authorable three ways (answer C7): data files, an in-game node editor, and Lua.

```
Reaction "wood_burns"
  when   self.tags has [flammable]
     and self.temperature >= self.ignition_temp
     and neighbor.tags has [oxidizer]
  chance 0.15 per tick
  emit   heat 40
  become { charcoal 0.35, ash 0.10 }
  spawn  { smoke 0.55 } into nearest gas-capable neighbor
```

**Mass fractions must sum to 1.0.** A validator rejects unbalanced rules at content-load time and in CI — a modder physically cannot write a rule that breaks conservation; the game refuses to load it.

Rules match by **tag bitset AND**, so "wood burns" applies to every wood-tagged material anyone ever authors, including player-made ones.

## 6. Rigid bodies (answers E9–E11, I4)

A disconnected piece of world becomes a `VoxelVolume` automatically (answer E9).

- **Detection:** connected-component labelling over brick occupancy by deterministic GPU label propagation, run only on bricks touched by destruction this tick.
- **Mass properties** summed from voxel densities — a stone wall is heavy, a wooden one is not, automatically.
- **Broadphase:** the world grid is the spatial hash.
- **Narrowphase:** transform one body's bricks into the other's local space and AND the occupancy bitmasks; set bits are contacts. Body-vs-world is cheaper still.
- **Solver:** fixed-point sequential impulse, fixed iteration count, stable-ID ordering → deterministic.
- **Cost scales with contacts, not voxels** — a 500 m crane (answer I4) with 8 contact points is cheap.
- **Denting and fracture (answer E11):** impacts below a material's hardness plastically deform the contact region (voxels displaced, conserved); above it, the body re-runs connected components with failed voxels removed and splits.
- **Re-baking:** a body at rest against static world for K ticks is baked back into world voxels and stops existing. Rubble becomes terrain.
- **No hard limits (answer E10):** budgets are enforced by sleeping the least significant bodies early, never by deleting them.

### Gravity (answers B6, O7)
A global gravity vector by default. Warping comes from **real voxels carrying a `gravity_warp` property** — strength plus mode (attract toward the voxel, or repel from it). No special entity type; a gravity artifact is something you build out of voxels and can dig apart.

Contributions from nearby warp voxels are accumulated into a coarse **gravity field grid** (one sample per brick, rebuilt incrementally when warp voxels change), which everything reads: rigid bodies, fluids, granular matter, cloth, characters. So a gravity artifact bends water and sand exactly as it bends objects, and destroying it restores normal gravity immediately.

## 7. Soft bodies, cloth, rope (answers E12, E13)

**Verlet integration** with distance/bending constraints, solved in a deterministic graph-coloured order with a fixed iteration count, all fixed-point.

- **Cloth:** 2D lattice, distance + bend constraints.
- **Rope:** 1D chain, optional twist.
- **Soft solid:** 3D lattice with shape matching.
- Voxels bind to the lattice; rays are transformed into the deformed space for rendering. Particle count is fixed, so conservation is trivial.

**Soaking and staining (answer E13):** fluid contacting a soft body or cloth transfers into its `wetness` layer (ledger-tracked — the fluid genuinely leaves the world and lives in the cloth), changing weight, colour and drape, and evaporating or dripping back out over time. Any fluid contacting any surface can deposit a `stain` — a persistent per-voxel colour modification that fades or washes off. Blood, paint, mud, oil all work through this one mechanism.

## 8. Ragdolls and active characters (answer B2 — "Euphoria-like")

Characters are **articulated VoxelVolumes** driven by the procedural rig (`07-roadmap.md` Stage 17):

- Joints are motorised with fixed-point PD controllers, so a character actively *holds* poses rather than flopping.
- A **balance controller** keeps the centre of mass over the support polygon, taking steps to recover.
- **Protective behaviours** layer on top: reach toward the ground when falling, brace against impacts, grab nearby ledges, shield the head, stagger instead of instantly collapsing.
- Muscle strength decays with damage, so a character transitions continuously from fully controlled → staggering → limp. There is no "ragdoll on/off" switch.

This is the single most computationally demanding character feature in the plan and it gets its own stage.

## 9. Explosions (answer E14 — pressure wave)

Not a sphere delete. A high-pressure gas source is injected and the wave propagates through the cellular grid, attenuating through solids by hardness and damaging or detaching voxels above a threshold. Free consequences: explosions turn corners, are contained by strong walls, blow out windows down a corridor, push loose objects, and shove fluids.

## 10. Structural integrity (answer E8 — method is my call)

**Chosen method: support propagation now, stress solve later.**

Each voxel carries an integer `support` value propagated from anchored ground, attenuated by distance and by material hardness, and reduced by the load above it. Voxels below threshold detach and become rigid bodies (which then fall, collide, and re-bake). It runs only on edited regions, costs almost nothing, and produces convincing collapses: overhangs fail, undermined walls come down, arches hold if built properly.

A full stress-tensor solve (which would model bending and shear correctly) is deferred — it is a strict upgrade to the same interface and can be added later without touching anything else. Toggleable off for pure creative building.

## 12. Catch-up and weathering (answer O2)

When you leave an area and come back, the region does not resume mid-motion as if no time passed — it **catches up** on the ticks it missed. Water that was still draining has drained; a fire that was burning has burnt out and left charcoal; a hot furnace has cooled; exposed structures have weathered.

Replaying every missed tick is impossible (an hour away is 72,000 ticks over a huge area), so catch-up is a **bounded, deterministic, mass-exact fast-forward**:

| Process | How it fast-forwards |
|---|---|
| Fluid settling | Solve directly for the equilibrium the region would have reached — volume conserved exactly by construction, no per-tick iteration |
| Temperature | Relax analytically toward ambient using the elapsed time and material conductivity |
| Combustion | Integrate the reaction over elapsed ticks: fuel consumed, products created, all through the same mass-balanced reaction rules |
| Gas | Merge into the region's atmosphere pool at the diffusion rate, ledger-exact |
| Falling matter | Resolve to its resting configuration |
| Weathering / aging | An aging function per material driven by exposure (rain, air, water contact, temperature cycling) — how you get moss, rust, erosion and worn edges over long absences |

Properties that hold:

- **Cost is bounded** and independent of how long you were away — an hour and a week cost the same.
- **Deterministic**: a pure integer function of `(frozen state, elapsed ticks)`, so every peer computes the identical result and multiplayer stays in sync without transmitting anything.
- **Mass-exact**: catch-up runs through the same ledger as ordinary simulation; nothing appears or disappears.
- **Closing a world stops the clock entirely.** Only time the world was actually running counts, so a save left alone for a year is untouched when you open it.

## 12. Determinism rules (enforced by CI lint and code review)

1. No `float`/`double` in `sim/`, `physics/`, or any shader under `shaders/sim/`.
2. No unordered reductions except integer `atomicMin`/`atomicMax`/`atomicAdd`, which are provably order-independent.
3. No dependence on thread, workgroup, subgroup size, or dispatch dimensions for anything but parallelism.
4. No wall-clock or frame time — only ticks.
5. All randomness via `hash(coord, tick, salt)`.
6. Lua may not mutate shared state directly; it emits ops.
7. Every new simulation feature ships with a determinism test: 10,000 ticks on the CPU reference and the GPU path, asserting identical world hashes.
