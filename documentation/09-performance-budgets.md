# 09 — Performance Budgets

*Revised after answer round 1. Minimum spec is now **Steam Deck** (answer A6), target **1280×800 / 30 FPS** with dynamic resolution allowed (answers D1, D2). Primary development target is the **RTX 5060 Ti 16 GB / Core Ultra 5 225 / 32 GB DDR5** machine (answer A4).*

You named bad performance as the one thing that would make this a failure (answer N5). So a budget here is not a target — it is a number a subsystem is **not allowed to exceed**. Exceeding it is a bug, not a trade-off to discuss later. Every roadmap stage has a perf gate and does not close until it is met.

---

## 1. Hardware tiers

| Tier | Reference hardware | Target |
|---|---|---|
| **T0 Floor** | **Steam Deck** (RDNA2, 8 CU, 88 GB/s shared) | 1280×800, 30 FPS locked |
| **T1 Low** | GTX 1650 / RX 5500 / Ryzen 7840U iGPU | 1080p, 30–45 FPS |
| **T2 Mid** | RTX 3060 / RX 6600 | 1080p, 60 FPS |
| **T3 Dev** | **RTX 5060 Ti 16 GB** | 1440p, 60–90 FPS |
| **T4 High** | RTX 4080 / 5080+ | 4K, 60+ FPS |

**T0 must run and must stay locked at 30.** Everything above T0 buys resolution and light-convergence speed, never the ability to play.

### The Steam Deck constraint is bandwidth, not compute

88 GB/s shared with the CPU is the real ceiling. At 1280×800 / 30 FPS that is **~2.9 GB of memory traffic per frame** for *everything*. The design fights for that number:

| Technique | Bandwidth saved |
|---|---|
| 64-byte occupancy bitmask (skip 512 voxels per cache line) | ~10× fewer brick fetches |
| Palette-compressed bricks (~220 B vs 2 KB) | ~9× per brick touched |
| Face cache (32 B per shading result, reused by many pixels) | ~20× vs re-shading per pixel |
| Beam optimisation + temporal start distance | 60–80% fewer ray steps |
| Visibility buffer (16 B/pixel, no material fetch during traversal) | large |

**Every pass has a bandwidth budget as well as a time budget**, listed below. This is the number that actually decides whether the Deck holds 30.

---

## 2. GPU frame budget — T0 Steam Deck, 1280×800, 30 FPS (33.3 ms)

Simulation runs at 20 Hz (answer E1) — 2 ticks per 3 frames — so its cost is amortised: 4.5 ms of tick work appears as 3.0 ms/frame average.

| Pass | Time | Bandwidth |
|---|---|---|
| Streaming upload + eviction | 0.8 ms | 120 MB |
| Edit application | 0.4 ms | 40 MB |
| **Simulation** (amortised, 20 Hz) | 3.0 ms | 500 MB |
| Hierarchy update (dirty paths only) | 0.8 ms | 80 MB |
| Beam / coarse visibility (1/8 res) | 0.9 ms | 90 MB |
| **Primary visibility** | 9.5 ms | 900 MB |
| Face selection + compaction | 0.7 ms | 60 MB |
| **Face shading** (30k faces × 1 path, 2 bounces amortised) | 7.5 ms | 620 MB |
| Face denoise (cache space) | 1.6 ms | 150 MB |
| Specular / refraction (3% of pixels) | 1.8 ms | 120 MB |
| Caustic photons (30k) | 0.7 ms | 50 MB |
| Volumetric fog (1/4 res, temporal) | 1.4 ms | 110 MB |
| Composite + post + TAA + upscale | 2.2 ms | 180 MB |
| UI | 0.6 ms | 30 MB |
| **Total** | **≈31.9 ms** | **≈3.05 GB** |

1.4 ms of headroom. Dynamic resolution is the release valve: internal resolution drops before the framerate does, and only the visibility + specular + fog passes scale with it.

**Note the shape:** visibility dominates, not lighting. That is the whole point of the face cache — the expensive part of path tracing has been moved off the per-pixel path.

## 3. GPU frame budget — T3 dev machine, 1440p, 60 FPS (16.6 ms)

| Pass | Budget | Measured, 2026-08-19 |
|---|---|---|
| Streaming + edits | 0.6 ms | |
| Simulation (amortised) | 1.8 ms | |
| Hierarchy update | 0.3 ms | |
| Beam + primary visibility | 4.2 ms | **D698.** The beam is BUILT. At 4K, arm to arm: 1.18 ms outdoors against 16.81 without it, 1.96 against 15.07 close, 2.32 against 12.49 sky. The beam pass itself is 0.019–0.348 ms. At the `distant` camera it is a net COST, 0.76 against 0.56, where the ray was already trivial |
| Face selection | 0.3 ms | |
| Face shading (300k faces × 4 paths, 2 bounces + sky) | 4.4 ms | |
| Face denoise | 0.6 ms | |
| Specular / refraction / dispersion (25% of pixels) | 1.8 ms | **D703.** The reflected image costs +0.4% of total GPU on a clip where EVERY surface is reflective, and provably nothing where none is: 0 blocks and 0 lobe rays in both arms |
| Caustic photons (300k) | 0.5 ms | |
| Volumetric fog | 0.7 ms | |
| Composite + post (bloom, DOF, motion blur, exposure, TAA) | 1.0 ms | **D702.** Post is its own pass now. At 4K: **0.706 ms still, MET**; **1.113 ms with the camera turning, MISSED**, of which 0.374 is a pure copy that goes if post is presented rather than written back. 0.109 / 0.327 / 0.706 across the three resolutions against pixel ratios 1 : 3.6 : 8.1 |
| UI | 0.3 ms | |
| **Total** | **≈16.5 ms** | |

*Blank means nothing was re-measured against that row on 2026-08-19, not that it is met. The three
rows that carry a figure are the three passes that changed. **Two of the three are stated as met AND
missed**, because a budget written for a still 1440p frame and a figure taken at 4K with the camera
turning are different questions and averaging them would hide the one that matters.*

## 4. CPU budget (main thread, per frame)

| System | T0 | T3 |
|---|---|---|
| Input + window | 0.3 ms | 0.2 ms |
| Frame orchestration + GPU submission | 1.6 ms | 1.0 ms |
| Op queue + prediction | 0.5 ms | 0.3 ms |
| Script dispatch (Lua, budgeted) | 0.8 ms | 0.5 ms |
| UI logic | 0.6 ms | 0.4 ms |
| **Main thread total** | **≤ 3.8 ms** | **≤ 2.4 ms** |

Everything else runs on the job pool: terrain generation, compression, save IO, network serialisation, brick packing, voxel type interning. **The main thread never touches the disk and never waits on a job.**

Lua has a hard per-tick time budget. A script that exceeds it is suspended and reported in the HUD — a bad mod can never hitch the game (answer K5).

## 5. Simulation budget (per 20 Hz tick)

| Metric | T0 | T3 |
|---|---|---|
| Awake bricks | 15,000 (≈7.7 M voxels) | 90,000 (≈46 M voxels) |
| Cell transfers | 3 M/tick | 20 M/tick |
| Active rigid bodies | 600 | 6,000 |
| Verlet particles (cloth/rope/soft) | 60,000 | 400,000 |
| Active ragdolls | 8 | 60 |
| GPU time per tick | 4.5 ms | 2.7 ms |
| Overrun policy | up to 3 catch-up ticks/frame, then time-dilate with a HUD warning | |

## 6. Memory

| | Steam Deck (6 GB usable) | RTX 5060 Ti (16 GB) |
|---|---|---|
| Brick pool | 512 MB | 4 GB |
| Voxel type table | 64 MB | 512 MB |
| Hierarchy nodes | 96 MB | 768 MB |
| Face radiance cache | 192 MB | 1.5 GB |
| Simulation layers | 192 MB | 1.5 GB |
| Render targets | 90 MB | 400 MB |
| **GPU total** | **≈1.15 GB** | **≈8.7 GB** |
| System RAM | 2.5 GB | 6 GB |

Fixed-size pools sized at startup from detected memory. Zero runtime GPU allocation.

## 7. Network (32 players, answer J1)

| Metric | Budget |
|---|---|
| Intent ops, one active builder | 2–8 KB/s |
| Player state, per observed peer | 1.2 KB/s |
| **Upstream per peer, worst case (all 32 co-located)** | **≤ 120 KB/s** |
| Upstream per peer, typical (spread out) | ≤ 25 KB/s |
| Connections held per peer | 4–10 (interest + gossip tree), never 31 |
| Join transfer | ≤ 25 MB for a heavily built world |
| Reconciliation digests | ≤ 3 KB/s |
| Added input latency on your own actions | 0 ms (predicted locally) |
| Tolerable RTT | 300 ms with no gameplay degradation |

## 8. Load and save

| Event | Budget |
|---|---|
| Cold start to main menu | ≤ 3 s |
| **`play now` offered** | **≤ 5 s (T0: ≤ 8 s)** — measured to the button rather than to the frame loop. **The cold estate is 5.6 s and is over this row**; from the cache it is 0.15 s, which is every launch after the first. D730 |
| Enter a world, by pressing it | ≤ 0.5 s from the press to the first frame |
| **The whole world present** | **≤ 10 s** — the coarse up-front build, measured at 8.5 s on the estate (D722) |
| Finish the whole world at authored detail | **no budget: it is linear in how much clip there is** |
| Join a session (click to playing) | ≤ 15 s |
| **Save on every edit (answer K3)** | **0 ms main-thread stall** — append-only journal written on the IO thread |
| World close / compaction | ≤ 2 s, backgrounded |

**And "the whole world present" is a separate row because it is a separate promise.** From
2026-08-20 the up-front coarse build is on again (D722): the whole clip goes into the world at
0.5 m voxels before the ladder starts, which on the estate is 8.5 s and ten megabytes. That is the
row a player feels as "the world is there". Taking it to the detail it was authored at is the ladder's
own work and has no budget, because it is linear in how much building there is and nothing in the
engine can shorten it — twelve levers are measured across D683 and D722 and the best is 1.3x.

**And the button is only offered once there is a world to enter (D730).** It used to be offered at
the earliest point the ladder could rebuild everything below it, which is a true statement about the
ladder and says nothing about time: pressing it thirty frames in gave **empty sky**, and ten seconds
in gave empty sky with a sliver of floor. A button offering that is not a way in, it is a way out of
the loading screen. So the offer waits for the up-front build — 5.6 s cold, 0.15 s from the cache.

**Why "enter a world" is measured to the button now.** From 2026-08-20 a load does not end when a
frame can be drawn; it ends when the ladder has taken every node of the clip to the detail it was
authored at and the finished world has been written to the cache (D721, and it was asked for). On
the estate that is tens of minutes the first time and a 400 ms read of a file every time after.

There is no grain at which the whole estate is instant and still a building — D686 measured four of
them — so a budget on finishing it would be a budget on how much building there is, which is a
decision for whoever authors the clip. What a budget CAN hold is the wait that is compulsory, and
that is the wait until the player is allowed to stop watching: `play now` is offered as soon as the
clip has been parsed, and pressing it puts them in the world with everything built so far. **So the
five seconds moved from "the load has finished" to "the load has stopped being compulsory", which
is the number a player actually feels.** `a player could have entered from t+N ms` is printed on
every launch and is the figure this row is judged on.

## 9. Regression policy

- A benchmark scene (fixed camera path, fixed op script, fixed seed) runs headless in CI and records per-pass GPU time **and bytes moved**.
- A >5% regression in any pass fails the build, naming the pass.
- Frame time is judged at the **99th percentile**, not the average. A locked 30 beats an average 45 with hitches — and on a handheld, a locked 30 is the whole experience.
- Steam Deck is tested on real hardware at every playable checkpoint, not simulated.
