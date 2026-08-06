# 06 — Multiplayer

*Revised after answer round 1: **32 players** (J1), **distributed authority, no host** (J4), **invite codes with usernames + Steam friends + LAN** (J2), **peer relay accepted** (J3), **no infrastructure of ours, ever** (M2), **IPv6 → STUN → relay, plus Steam later** (M1), **no permissions** (J9), **anti-cheat only for future survival** (J7).*

Goal: two friends play together in under 15 seconds, with no port forwarding, no account, no server of ours, and no recurring cost to anyone, forever — scaling to 32.

---

## 1. Why the world can be networked at all

A world contains billions of voxels. **None of them are sent.**

```
world = generate(seed, worldGenGraph) + replay(orderedIntentOps)
```

- **Terrain** is a pure function of the seed and the world-gen graph (answer F7) — never transmitted.
- **Player actions** are tiny intent ops, 16–48 bytes: *"player 7, tick 8214, chisel-place, voxel type 91, from P₁ to P₂"*.
- **Simulation results** — the millions of water cells that moved this tick — are recomputed identically on every machine and **never transmitted**.

The expensive part of a voxel game costs nothing on the wire.

## 2. Authority: region ownership, no host

You asked for distributed authority (J4) and for the world to keep going when anyone leaves (J5). The model:

- The world is divided into **authority regions** (64 m cells — 8×8×8 chunks).
- Each region has exactly **one owner peer** at a time. The owner decides the final order of ops in that region and broadcasts them.
- Ownership is claimed by the peer that has been closest and stable longest, transferred with a lease + version handshake, and **released cleanly on leave**.
- If an owner disappears without releasing, the remaining peers that have the region loaded elect a new owner deterministically (lowest peer id among candidates), resuming from the last acknowledged tick.
- **Lua scripts, NPC AI, and world generation for a region all run on that region's owner**, and their effects leave as ordinary ops. This is what makes float-using Lua mods safe (see `11-reality-check.md` §8).

There is no global host, no single machine everyone depends on, and no migration event to survive — leaving is just releasing your regions. Every peer already holds the full world, so anyone can keep playing it alone afterwards (answer J5) or take the file and host it later (answer J6).

**Your own edits are applied instantly and locally (prediction)** and confirmed when the owner's ordering comes back. If the owner reorders them behind someone else's, your local state rewinds and replays — invisible in practice for building, which is almost never contended.

## 3. Topology: not a mesh

32 peers fully meshed would be 992 connections. Instead:

- Each peer connects only to peers whose **interest regions overlap** its own — typically 4–10 links.
- For broadcast within a region, the owner builds a **gossip spanning tree**: it sends to k≈3 peers, who forward. Upstream cost per peer stays flat no matter how many players join.
- The tree rebuilds automatically on join/leave/latency change, preferring low-RTT, high-upstream peers.

Result: upstream per peer stays ≤120 KB/s even with all 32 players standing in the same room, and ~25 KB/s when spread out.

## 4. Reconciliation — the safety net

Determinism is enforced hard (`05-simulation.md` §10), but a driver bug or a mod mismatch must not break a world.

- Each peer computes a rolling 64-bit hash per chunk on the GPU.
- Peers periodically exchange compact digests of nearby chunks (a few hundred bytes).
- Mismatch → the region owner sends that chunk compressed (2–20 KB); the client replaces it.

Divergence becomes a one-chunk hiccup that self-heals in one round trip. The same path handles a player arriving in a heavily modified area.

## 5. Joining

1. Peer receives **seed + world-gen graph + content hash + op-log tail**.
2. Peer generates terrain locally on the GPU (fast) and applies the compressed edit journal near its spawn.
3. Distant regions stream lazily along the same demand-driven path the renderer already uses.

Megabytes, not gigabytes. Serialisation happens on the job pool from an immutable snapshot, so nobody's frame stalls.

**Content hash handshake:** mods, materials, and reaction rules are hashed (answer C8). A mismatch refuses the connection with a plain-language message naming the differing mod — never a silent desync.

## 6. Transport

Custom UDP. Nothing else satisfies "free forever, Unlicense-compatible, no dependency risk".

- **Unreliable** channel: player positions, animation, held tool — superseded data, never resent.
- **Reliable-ordered** channel: intent ops. Selective acknowledgement, no head-of-line blocking.
- **Reliable-bulk** channel: chunk repairs and join transfers. Chunked, resumable, low priority, congestion-controlled so it never starves ops.
- Congestion control: BBR-style rate estimation; never fills buffers to the point of adding latency.
- Encryption: X25519 + ChaCha20-Poly1305 from a public-domain reference implementation. Prevents casual tampering and address spoofing.

## 7. Connectivity ladder (answer M1: d → a → c)

| Step | Mechanism | Infrastructure | Coverage |
|---|---|---|---|
| 1 | **IPv6 direct** | none | growing share — no NAT at all |
| 2 | **LAN discovery** (UDP multicast) | none | same network |
| 3 | **UDP hole punching**, own address learned from **public STUN** | none of ours | ~85–92% of remaining pairs |
| 4 | **Peer relay** — an already-connected player forwards for the blocked pair | none | everything else (answer J3) |
| 5 | **Steam Datagram Relay**, once you ship on Steam (answers A11, M1) | Valve's, free to you | ~100% |

**Rendezvous without any server (answer M2 = no infrastructure):**

- The world creator generates an **invite code** encoding their chosen **username** plus their candidate addresses and a session key. Duplicate usernames get a numeric suffix — "Alex", "Alex2", "Alex3" (answer J2).
- The friend pastes the code. That is the whole flow.
- Once inside, peers exchange each other's candidates automatically, so only the first person needs a code and connections form peer-to-peer from there.
- Steam friends-list invites replace the paste step once you ship on Steam.

**Honest limitation:** invite codes are point-in-time. If the creator's public address changes (router reboot, mobile network), the old code stops working and a new one must be shared. Solving that permanently is exactly what a rendezvous server does, and you have ruled that out — correctly, for the "free forever" goal.

## 8. Player state

Position, orientation, animation, held tool: unreliable, 20 Hz, quantised (position to 1/8 voxel = 4 mm, rotation 16 bits/axis), delta-compressed against last ack. ~35–60 bytes per player per update. Interpolated for rendering; briefly extrapolated on loss.

Movement is client-authoritative with sanity limits (answer J7 — cheating matters only for the future survival mode). Creative flight and noclip (answer G7) are features, not exploits. When survival arrives, its authority model tightens *inside the same op framework* — server-style validation of survival-relevant ops by the region owner — without redesigning the network layer.

## 9. What every other system must do to keep this possible

Applied in code review from Stage 1, long before networking exists:

1. Every world mutation goes through an **Op**. No direct writes from gameplay or script code.
2. Ops are serialisable and carry a tick.
3. **No floating point** in shared-state mutation. Lua is exempt because it only *emits* ops (§2).
4. No wall-clock time in simulation — only ticks.
5. All randomness from `hash(coord, tick, salt)`.
6. All content is hashed; peers refuse mismatched connections with a clear message.
7. Any stateful system can produce a hash of its state for reconciliation.

Following these from the start is why Stage 16 is "wire it up" rather than "rewrite the game".
