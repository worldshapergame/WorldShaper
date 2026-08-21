# 04 — Rendering

*Revised after answer round 1: **stochastic level blending** so detail has no steps at all (N2), **2 bounces + sky default** (D4), **any voxel can emit light** (D6), **dispersion per-material + quality setting** (D7), **progressive caustics** (D8), **no-GI fallback + screen-space fallbacks allowed on low tiers** (D9, D10), **correct filtered colour at distance** (D11), **flat faces, no bevels** (D12), **volumetric fog** (D13), **all post-processing, physically derived, speed-based motion blur** (D14), **full-body first person** (D15), **diegetic UI** (D16), **fast approximations preferred over strict correctness** (N4).*

Two ideas carry this renderer:

1. **Geometry is resolved per pixel against a hierarchy** — you never touch more voxels than you have pixels, so render distance is free.
2. **Light is computed per voxel face in world space, not per pixel** — so lighting cost is decoupled from resolution, samples accumulate across frames, and noise nearly disappears.

Everything else is detail.

---

## 1. Primary visibility

### Hierarchical ray march

One ray per pixel, walking the sparse octree:

- At each node, test the child occupancy mask; skip empty children with no memory traffic.
- Descend while the node's projected footprint exceeds the pixel footprint.
- Inside a brick, a 64-byte occupancy bitmask lets one 3D-DDA step test 8×8×8 voxels with bit operations.

A mountain 40 km away resolves in 4–6 node steps; the rock at your feet in 10–12. There is no render-distance setting because distance is not what costs.

### Stochastic level blending — no steps, anywhere

Descent depth is a **continuous real number** (e.g. 7.34). The fractional part drives a blue-noise dithered choice between level 7 and level 8, per pixel, per frame; temporal accumulation resolves it into a smooth blend.

Detail therefore varies as a **continuous function of distance with no discrete transition in the math at all** — not merely one small enough to hide (answer N2). Cost: one extra random number per ray.

**The guarantee this produces**, against your stated failure condition (N5, "short range of high detail voxels"):

> A voxel renders at its true individual detail whenever it covers ≥1 pixel on screen, at any distance. Nothing degrades before that. Detail range is limited only by your screen resolution — the physical maximum.

### Making it fast

Empty-space traversal is the entire cost. Three accelerators:

1. **Beam optimisation** — a 1/8-resolution pre-pass writes a conservative start distance per tile. **BUILT, 2026-08-19, D698** — `shaders/beam.comp`, one coarse ray per 8x8 tile CORNER at sixteen pixel widths, and each tile starts its rays at the smallest of its four. The estimate here said 60–80% of steps; measured at 4K it is **1.18 ms against 16.81 outdoors and 1.96 against 15.07 close**, with `distant` a net cost at 0.76 against 0.56 because the ray there was already trivial. `--no-beam` restores this line's "before" exactly.
2. **Temporal reprojection** — last frame's depth gives an even tighter start, validated against the coarse pass so it can never produce a wrong hit.
3. **Optional raster prepass** — rasterise resident coarse-node bounds to establish tight per-pixel ray intervals. Built as an alternative path; a startup benchmark picks per device.

### Visibility buffer

Per pixel, 16 bytes: `nodeId · voxelIndex · faceDir · lodLevel · depth · in-face position`. No material fetch, no shading — just "which face did I hit". This is a large bandwidth win on Steam Deck.

---

## 2. The face radiance cache

### Structure

A GPU hash table keyed by **(node coordinate, level, face direction)** — one entry per visible voxel face, at the detail level it is being viewed at.

```
FaceEntry (32 bytes)
  radiance   rgb9e5     // outgoing diffuse radiance, denoised
  raw        rgb9e5     // pre-filter accumulator
  photons    rgb9e5     // caustic energy (see §3)
  samples    u16
  variance   f16
  lastSeen   u32
  flags      u8
```

192 MB on Steam Deck holds ~6 M live faces; a 1280×800 screen is 1 M pixels and the face-to-pixel ratio is typically 1:4 to 1:30, so that covers a full screen of geometry several times over including reflections and off-screen GI.

### Lifecycle

1. Visibility writes touched face IDs to a feedback buffer.
2. Compaction produces a unique list with priority `screenCoverage × age × variance × recentlyChanged`.
3. The top **B** faces (the frame budget) are shaded.
4. New faces are **seeded from their parent node's entry**, so a face entering view is immediately plausible and never black. This removes the "path tracer settling in" look entirely.
5. LRU eviction of anything unseen for N frames.

### Shading a face

For each selected face, trace M paths from a jittered point on it:

- **Sun** — one shadow ray, bitmask occlusion, early out.
- **Emissive voxels** — sampled from a per-region alias table over emissive faces, so any voxel can be a real light source (answer D6) without tracing blindly. The table rebuilds incrementally as emissive voxels are placed or destroyed.
- **Sky** — analytic single-scattering atmosphere; no ray needed when the shadow ray escapes.
- **Indirect** — a cosine-weighted ray; on hit, **read that face's cached radiance**.

Because every face's cache entry already contains its own multi-bounce result from earlier frames, one traced bounce yields full multi-bounce GI. The cache is a progressive radiosity solution that converges in ~10–30 frames and stays converged until something changes. Default quality is **2 explicit bounces + sky** (answer D4), which on top of the cache feedback reads as effectively unlimited bounces.

Stability: only the *previous* frame's values are read, albedo is clamped below 1, and a per-region energy ledger flags runaway in debug builds.

### Denoising

Done **in cache space**, not screen space:

- **Spatial:** average with 4–8 coplanar neighbour faces, weighted by normal and material agreement. Faces on different planes are different keys, so light never leaks across an edge and corners never blur.
- **Temporal:** exponential moving average with variance-driven blending. No reprojection, no ghosting — the data never moved, because it is attached to the world rather than the screen.

Screen-space denoising is then only needed for the small per-pixel specular budget.

### What this buys

Lighting cost is `B × M` rays per frame — a number *you set*, independent of resolution. Weak hardware buys **latency** (lights settle in ~0.4 s instead of ~0.05 s), never lower fidelity and never lower framerate.

---

## 3. View-dependent effects (per-pixel, small budget)

Answer N4 says fast approximations are preferred where they look the same, so these use fitted approximations rather than reference formulations.

### Reflections
Below a roughness threshold, trace a mirror/GGX ray; on hit, read the **already-denoised** face cache. Sharp, stable, essentially noise-free reflections from one ray per pixel. Rougher surfaces use the cached diffuse plus a cone-traced gloss lobe. On T0, screen-space reflection is permitted as a cheaper fallback (answer D10).

### Refraction and Snell's law
Refraction at every `ior` boundary, with total internal reflection. Fresnel via Schlick's approximation.

**Normals:** cube voxels give axis-aligned normals, so glass looks blocky — correct, and often desirable. For fluids and materials flagged `smooth_surface`, the normal is instead derived from the **gradient of the fill/occupancy field** across neighbours, giving genuinely smooth curvature for water and lenses while the geometry stays 100% real voxels. Per-material toggle.

### Absorption
Beer-Lambert along the in-medium path: `T = exp(-absorption × distance)`, with exact distances because we know which voxels the ray crossed. Deep water goes blue-green and thick glass goes green at the edges for free.

### Dispersion (answer D7 — per-material, plus a quality setting)
IOR varies with wavelength (Cauchy fit). **Hero wavelength sampling**: one jittered wavelength per pixel per frame, reconstructed by temporal accumulation. Cost ≈ one normal refraction ray. Enabled only on materials flagged `dispersive`, with a global quality setting controlling wavelength count (1 → 4).

### Caustics (answer D8 — progressive)
Photons are traced from the sun and emissive voxels, refracted through `caustic_source` materials, and **atomically deposited into the face cache's photon channel**. Normalised by photon count and temporally filtered.

Caustics therefore cost nothing at render time, appear correctly inside reflections and GI (they are part of the cached radiance), and produce underwater light shafts and pool-bottom patterns. They converge over ~0.2 s.

---

## 4. Volumetrics and atmosphere (answer D13)

- Froxel (frustum-voxel) volume at 1/4 resolution with temporal reintegration.
- Participating media from three sources: the atmosphere model (aerial perspective — essential for depth readability at infinite view distance), local fog volumes, and **simulated gas voxels** (smoke, steam) read directly from the `fill` layer. Smoke you create is the same smoke that scatters light.
- Sun shafts fall out of the same integration; no separate god-ray pass.
- Cost: 1.4 ms on T0, 0.7 ms on T3.

## 5. Post-processing (answer D14 — all of it, physically derived)

Because the renderer is a path tracer, these are derived from real quantities rather than faked:

| Effect | Basis |
|---|---|
| **Exposure** | Physical camera model (EV from scene luminance histogram), auto or manual |
| **Bloom** | Energy above sensor saturation, physically weighted, wavelength-tinted |
| **Depth of field** | Real aperture and focal length; circle of confusion from depth |
| **Motion blur** | Per-pixel velocity from the visibility buffer × shutter angle — **speed-based**, so it appears when you move fast and vanishes when you do not |
| **Tonemap** | AgX-style, implemented from published math |
| **TAA** | Light — lighting is already temporally stable, so ghosting is minimal. Also resolves stochastic level blending and hero-wavelength dispersion |
| **Upscaling** | Own temporal upscaler; no vendor SDKs (see `08-tech-stack-and-licensing.md`) |

All individually toggleable, since motion blur and DOF are also comfort settings.

## 6. Faces, edges, and the look (answer D12)

Perfectly flat faces, no bevels, no normal variation, no grid lines. The voxel scale reads through lighting and through the `coverage`-based partial-coverage anti-aliasing that keeps distant edges smooth without supersampling. Distant colour is correctly filtered — slightly soft rather than crisp and aliased (answer D11).

## 7. Quality tiers

| Tier | Faces/frame | Paths/face | Specular px | Photons | Target |
|---|---|---|---|---|---|
| **T0 Steam Deck** | 30k | 1 | 3% | 30k | 1280×800 / 30 |
| T1 Low | 60k | 1 | 8% | 60k | 1080p / 30–45 |
| T2 Mid | 140k | 2 | 15% | 150k | 1080p / 60 |
| **T3 Dev (5060 Ti)** | 300k | 4 | 25% | 300k | 1440p / 60–90 |
| T4 High | 700k | 8 | 100% | 1M | 4K / 60 |
| *Fallback (answer D9)* | direct sun + cached ambient, no GI, SSAO, SSR | — | — | — | emergency floor |

Geometry is identical in every tier. Only light convergence speed and specular coverage change.

## 8. Hardware ray tracing

Not required, and **now measured rather than asserted** (D849).

RT cores accelerate one half of a ray: *which volume did I hit*. This marcher already answers that
out of the octree — an empty cell is jumped rather than stepped, `skip_level` going to the far side
of the empty block in one step at whatever size the descent found it. So the case for a BVH is
exactly how long the walk it would replace is, and on `clips/facility.clip` (573 M solid voxels)
that walk is **3.65 to 5.38 outer steps a ray**, over 1.8 billion rays, agreeing across two
independent instruments. A BVH descent over a world that size is fifteen to twenty node tests.

Run it yourself: `tools\marchsteps.ps1`. `--march-stats` counts every march in the frame;
`--debug-mode 12` gives the per-pixel distribution for primary rays.

The one arm where the walk is long is `--infinite-detail` (R8e, not done): **277.86 steps a ray**,
18.3 ms against a 9.5 ms budget, and 186,878 faces lit only because their sun ray ran out of the
512-step cap. That is a real problem and it is still not a hardware one — those steps are spent
below voxel level where there is no geometry to build a structure over, and the fix is to traverse
at brick granularity as the shipping arm does. See D849.

## 9. Camera and UI

**Full-body first person** (answer D15): the camera sits between the eyes of the real, shaded player model. You see your own body when you look down, your hands and tools are the actual rig (answer G8), and other players see exactly what you see. No separate view-model rig, no floating arms.

**Diegetic UI** (answer D16): menus, inventories and tool settings are rendered as in-world objects — real geometry, lit by the real renderer. Architecturally this means the UI system is a scene-graph of voxel/quad panels with ray-based interaction, not a 2D overlay. HUD-critical text uses a pixel font (answer L3) rendered crisply in screen space where legibility demands it.
