# 11 — Reality Check

*Revised after answer round 1.*

Where the stated requirements collide with hardware or math, and how the design resolves each one. Nothing here is a refusal — every item has a plan that gets you what you actually want.

---

## 1. "No LODs" — and "there shouldn't be steps at all" (answer N2)

**The conflict.** Infinite render distance means unbounded data in view. A screen has ~1–2 million pixels. Any correct renderer must decide not to individually process the 10 billion voxels behind one pixel. Whatever performs that decision *is* a level-of-detail mechanism.

**The resolution — two parts.**

**(a) Continuous descent.** The world is a sparse voxel hierarchy where every node stores the filtered average of its children. A pixel's ray descends only until the node's projected size is ≈1 pixel, then stops. Depth is chosen per pixel from screen-space error, so there are no LOD *levels* to see and no render-distance slider.

**(b) Stochastic level blending — this is the part that removes steps entirely.** Descent depth is computed as a *continuous real number*, e.g. 7.34. The fractional part drives a blue-noise dithered choice between level 7 and level 8, per pixel, per frame. Temporal accumulation resolves that into a smooth blend. The result is that detail is a **continuous function of distance with no discrete transition anywhere in the math**, not merely a transition small enough to hide. Cost: one extra random number per ray.

**Your failure criterion (N5), "short range of high detail voxels", becomes a hard guarantee:**

> A voxel is rendered at its true, individual detail whenever it covers at least one pixel on screen. Nothing dumbs down before that point, at any distance.

That is the physical maximum — below one pixel there is nothing left to show. Detail range is limited only by your screen resolution, never by an engine setting.

---

## 2. "Billions of voxels" (answer N1 — confirmed: billions visible)

| Category | Realistic budget | How |
|---|---|---|
| Voxels representable | 10¹⁵+ | sparse storage; empty space free; unexplored terrain is a seed, not data |
| Voxels resident in memory | 1–10 billion (Deck) / 8–40 billion (dev PC) | palette + bitmask bricks at ~0.4 bytes/voxel typical |
| Voxels visible (contributing to a frame) | Billions | never touched individually — ~1–2 M hierarchy leaf-equivalents per frame |
| Voxels actively simulating per tick | 5–40 million | dirty/sleep tracking; only changed bricks run |
| Voxels edited per frame | 1–10 million | GPU-side brush application |

Billions simulating *every tick* would be ~10¹¹ cell updates/second — roughly 100× beyond consumer hardware, and unnecessary: a lake only simulates at its surface and where it is disturbed. Aggressive sleep is what makes "everything is simulated" true in practice.

---

## 3. "Per-voxel colour, properties, tags, transparency, and infinite state fields" (answers C2–C5, C11)

**The conflict.** Literal per-voxel property tables at a billion voxels is tens of gigabytes.

**The resolution.** Two mechanisms, both pay-for-use, detailed in `03-voxel-data-model.md`:

1. **Interned voxel types.** A voxel stores a 32-bit id into a deduplicated table of complete voxel definitions (colour, tags, properties, script handle). Identical voxels share one record automatically. A million-voxel wall of one colour costs 64 bytes of type data. Making one voxel unique costs one new record — for that voxel only.
2. **On-demand per-brick layers.** Temperature, fill, wetness, stain, charge, damage, velocity, and any field a mod registers, allocated only on bricks that actually use them.

**Honest limit:** if every voxel in a billion-voxel region is genuinely different from every other, that is genuinely ~64 GB of information and no encoding can compress it away. Nothing real does that. There is no artificial cap; the HUD reports type-table usage and warns before it hurts.

---

## 4. "Path tracing that colours voxel faces, not pixels" (answer N3 — hybrid accepted)

Your instinct is the biggest performance idea in the design. Shading in world space at face granularity means one shading result serves every pixel that sees that face, results persist across frames (hundreds of effective samples within a second), lighting cost decouples from resolution, and distant geometry shades at coarse levels automatically.

**The limit.** Mirror/glossy reflections, refraction, and dispersion are *view-dependent* — the answer differs per eye position, so there is no single value to store on a face.

**Resolution (accepted):** diffuse + rough + GI + sky + emissive + **caustic energy** cached per face; specular/refractive paths as a small per-pixel budget (2–10% of pixels), terminating into the face cache after one bounce so they are cheap and nearly noise-free. Caustics fit the model exactly: photons deposit their energy *into* face cache entries.

With answer N4 (fast approximations preferred over strict correctness), Snell's law, dispersion and Fresnel use cheap fitted approximations that are visually indistinguishable in the cases the game actually renders.

---

## 5. Performance floor: Steam Deck (answer A6) — the real constraint is bandwidth

Steam Deck: RDNA2, 8 compute units, ~1.6 TFLOPS, and **88 GB/s of memory bandwidth shared with the CPU**. It is not compute-starved; it is bandwidth-starved. Every design decision must minimise bytes touched per pixel:

- 64-byte occupancy bitmasks let one cache line answer "is anything in these 512 voxels".
- Palette-compressed bricks mean a brick fetch is 200 bytes, not 2 KB.
- The face cache means lighting reads a 32-byte entry instead of re-fetching geometry and materials.
- Beam optimisation and temporal start-distance cut ray steps 60–80%.

At 1280×800 / 30 FPS (answer D1) with dynamic resolution allowed (answer D2), this is comfortable. Steam Deck is a *far* easier floor than the integrated-GPU floor the original plan assumed — it has Vulkan 1.3, bindless, subgroup ops, 64-bit atomics and even ray query. The feature floor moves up accordingly, which simplifies the engine.

---

## 6. "Infinite rigid bodies, infinite voxels per body" (answer E10)

No hard cap is coded. What actually happens:

- Bodies at rest against static world for K ticks are **baked back into world voxels** and stop existing as bodies. A demolished city becomes rubble-shaped terrain, not 40,000 permanent objects.
- Active bodies are budgeted per tick; if the budget is exceeded, the *least significant* bodies (small, far, slow) are put to sleep early rather than dropping the framerate.
- Very large bodies (a 500 m crane, answer I4) are handled by simulating their voxels as one rigid transform — cost scales with *contacts*, not voxel count. A million-voxel body with 8 contact points is cheap.

So: unlimited in principle, framerate-protected in practice, and never by deleting your stuff.

---

## 7. 32 players, distributed authority, no server (answers B11, J1, J3, J4, M1, M2)

**What works.** Only intent ops cross the wire (`06-multiplayer.md`); terrain comes from the seed; simulation is recomputed identically everywhere. Bandwidth is tiny per player.

**The hard parts, and the resolutions:**

- **No host, distributed authority (J4)** → the world is partitioned into **authority regions**; each region has exactly one owner peer, assigned deterministically and transferred by lease. Ownership handles ordering for that region. No global host, no single point of failure, and any peer leaving costs nothing (answer J5: everyone already has the full world and can keep playing it as their own).
- **32 peers is not a full mesh** (32×31 links would be 992 connections). Each peer connects only to peers it shares interest with, plus a **gossip spanning tree** per region for broadcast. Upstream stays flat regardless of player count — roughly 40–120 KB/s worst case.
- **Symmetric-NAT pairs (J3, accepted)** relay through another player. Free, serverless, always works.
- **No infrastructure at all (M2 = no)** means rendezvous is by **invite code with a chosen username** (answer J2) — the code carries the peer's candidate addresses. IPv6-direct first, then STUN hole punching, then peer relay, plus Steam's relay network once you ship there (answer M1).

**Honest statement:** 32-player, serverless, distributed-authority is the hardest networking configuration that exists. The design has a defined fallback at every layer, and the deterministic-op foundation means the *world* is never at risk — worst case is a slower connect or a relayed route.

---

## 8. Lua modding + runtime rule changes + determinism (answers A1, C7, C8, K5)

**The conflict.** Lua uses floating-point doubles; float in shared-state simulation breaks cross-machine determinism.

**The resolution — scripts never simulate, they *request*.** Lua runs only on the peer that owns the region, and its effects leave as ordinary integer **ops** that every peer replays identically. Consequences:

- Mods can use floats, randomness, and anything else freely. Determinism is unaffected.
- Runtime rule changes are safe: content is hashed and peers refuse to connect on mismatch with a clear message.
- Scripts get a synchronous read-only view of the world and a queued write API. No script can stall a frame — a script exceeding its time budget is suspended and reported, never allowed to hitch the game.

---

## 9. Conservation of matter, with drills and printers (answers E6, I6)

Conservation is structural, not checked-and-patched: a two-phase propose/claim/vacate protocol means every unit of matter is either where it was or exactly one place else — never zero places, never two.

Gas is exactly conserved as you asked (E6). To keep that affordable, gas thinner than a threshold merges into a per-region **atmosphere pool** — still a tracked quantity in the ledger, just no longer costing per-voxel work, and returned if a vacuum forms.

Logic that creates or destroys matter (E6/I6 — drills, printers) goes through an explicit **accounted source/sink op**, so the ledger stays exact and a survival mode can later make matter actually scarce.

---

## 10. "Everything is real voxels, no parametric anything"

Consistent as stated. Generation *uses* noise and fractals to decide where voxels go, then writes real voxels; nothing is analytical at runtime.

The only tension is memory: 1 km³ of fully detailed terrain is ~10¹³ voxels. Resolution: unexplored, unmodified terrain is stored as *its seed* and regenerated on demand (answer F7). It is real voxel data whenever anyone looks at or touches it, and only becomes stored data once modified. Saves stay small; the invariant "any voxel can be dug out" holds everywhere.

---

## 11. One developer who does not code (answers A2, A3, N6, N7)

Not a technical conflict, but the biggest project risk, so it is stated plainly once:

- **I write 100% of the code.** You never have to open a file. Every stage ends with something you double-click and play.
- **Automated tests are the safety net**, because there is no second engineer to catch mistakes. This is why answer N6 (long, thorough foundations) is the right call and why Stages 0–2 are heavily weighted toward test infrastructure.
- **Your job** is design decisions, playing the checkpoint builds, and telling me what feels wrong. That is genuinely the valuable half.
- **Scale:** this is a multi-year project at hobby pace. It is shippable and enjoyable long before it is finished — that is what the 19 playable checkpoints are for.
- **Explanations** are written in the reply to the user as a short list of what to look at in the
  build. A standing plain-language document was tried for a year and never read (2026-08-19).
