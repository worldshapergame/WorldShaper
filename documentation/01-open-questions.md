# 01 — Open Questions

Answer format: edit this file, put the answer inline under the question as `**A:**`. Anything marked **BLOCKING** must be answered before the listed stage can start.

---

## A. Project, team, constraints

- **A1.** **BLOCKING (Stage 0)** — What language do you want to write this in? (Recommendation: C++20 or Rust. Both are viable; the whole build system and my code style depend on this answer.)
c++, modding pipeline with LUA
- **A2.** **BLOCKING (Stage 0)** — Are you the only developer, or will others contribute? Affects how much I invest in build tooling, code docs, and test infrastructure.
i am the only dev and i dont even know how to code
- **A3.** What is your own coding experience level with: C++ / Rust / GPU compute shaders / graphics APIs? I will match explanation depth in code comments and docs to this.
nothing, gamer
- **A4.** What hardware do you develop on? Exact GPU, VRAM, CPU, RAM. This becomes the primary perf target and the thing I write budgets against.
5060 ti 16gb, intel core ultra 5 225, 32gb ddr5
- **A5.** **BLOCKING (Stage 0)** — Target platforms for v1: Windows only? Windows + Linux? macOS? (macOS has no Vulkan and no compute-shader parity — it would need a MoltenVK layer or a second backend, and it materially changes the renderer plan.)
windows is the main one, i dont care about mac support
- **A6.** What is the *minimum* GPU you want to support? e.g. Intel UHD 620, GTX 1050, Steam Deck, RX 580. This sets the feature floor (bindless? subgroup ops? 64-bit atomics? ray query?).
steam deck
- **A7.** Is there an existing WorldShaper codebase I should read before designing the rewrite — even for reference on what did and did not work?
no
- **A8.** What went wrong with the previous version? Specific failures are the most valuable input I can get.
dont ever look at the old version i dont want you taking inspiration from it
- **A9.** Do you have a target release window, or is this open-ended?
open ended
- **A10.** Will the game be sold, free, or free-with-optional-paid? (Affects the multiplayer options: e.g. Steam's free relay network is only available to games shipped on Steam.)
free, open source, the unlicense, but i plan on releasing on steam as well eventually
- **A11.** Do you intend to ship on Steam? (Yes to this unlocks a genuinely free, no-server, no-port-forward networking layer — see M-section.)
eventually when the game is done
- **A12.** Are you okay with the engine being open source, or should it be closed?
open source

## B. Scope and gameplay

- **B1.** Is WorldShaper a *creative sandbox only*, or is there survival/progression? You mention crafting as a future thing — how central is it?
main thing is creation but you can also play survival or any mode of game the player builds or scripts
- **B2.** Is there combat? PvP? NPCs/creatures with AI?
yes, and euphoria like physics for ragdoll
- **B3.** Single-world-per-save, or many worlds a player switches between?
many worlds
- **B4.** Is there an inventory/hotbar, or does the player have unlimited access to all materials in creative?
unlimited access in creative
- **B5.** How big is a "world" conceptually — one infinite planet, or infinite flat space, or space with multiple planets?
one infinite world with terrain
- **B6.** Is there gravity everywhere, or per-region gravity (e.g. spaceships, planets)?
gravity everywhere, but, there can be artifacts that warp it
- **B7.** Do players build in-world only, or is there also a separate "editor mode" (e.g. an isolated volume for authoring characters/materials/props)?
yes they can build those things inside worlds or outside of them
- **B8.** Should creations be shareable as files (a `.wsobj`-style format) that other players can import?
absolutely
- **B9.** Is time-of-day / weather in scope for v1?
not really
- **B10.** Water bodies: are oceans/rivers *generated* as real simulated fluid, or as "settled" static water that only becomes dynamic when disturbed? (Massive perf implication — recommendation is the latter.)
the latter
- **B11.** What is the intended session length and player count per world? 2 friends? 8? 32?
servers of up to 32 people, infinite session length or how much the player wants
- **B12.** Should the world persist while nobody is in it (i.e. simulation catch-up on load), or freeze exactly?
yes, persist

## C. Voxel data model

- **C1.** **BLOCKING (Stage 1)** — What is the maximum number of distinct *material definitions* in a world? 256? 65,536? 4 billion? This sets the per-voxel index width and therefore memory for the entire game.
infinite
- **C2.** "Infinite tags and properties" — do you want tags/properties to be per **material definition** (thousands of materials, each with unlimited properties) or per **individual voxel instance** (each of the billions of voxels can have its own unique property set)? These are radically different in cost. Recommendation: properties live on the material; individual voxels get a small sparse *override* table plus a few per-voxel dynamic state fields (temperature, damage, wetness, charge). Is that acceptable?
per voxel
- **C3.** Which per-voxel *dynamic* state fields must exist at all times? Candidate list: temperature, integrity/damage, wetness, electrical charge, fill amount (for fluids), velocity (for moving matter). Each field costs ~1 byte per voxel across billions of voxels — every one must be justified.
infinite
- **C4.** Should a voxel's *color* be per-voxel (unique color per voxel, expensive) or per-material-with-variation-index (cheap, e.g. 16 shade variants baked into the material)? Recommendation: a small per-voxel variation index (4–8 bits) that maps into the material's palette.
per voxel
- **C5.** Do you need per-voxel arbitrary user data (like a script handle, a container inventory, a sign's text)? If yes, that is a sparse side-table keyed by voxel coordinate.
yes
- **C6.** Should voxels be able to be *partially* filled (sub-voxel geometry — slopes, smooth surfaces), or is everything a full cube? Full cubes are dramatically simpler and faster and fit the art direction.
full cube
- **C7.** How are reactions authored? Data files (JSON/TOML-like), a visual node editor in-game, or an embedded scripting language? **BLOCKING (Stage 11)**
all three
- **C8.** Should reaction rules be moddable by players at runtime, or compiled into the build? (Runtime modding + multiplayer determinism = every peer must have identical rule sets; needs a content hash handshake.)
at runtime
- **C9.** What is a "material" in the UI? Is `sandstone` a single brush that paints a pattern of several underlying voxel types, or is it one voxel type with a noise-driven color? You describe the former — confirm: a material is a **pattern generator** that emits a mix of tagged voxel types.
the former
- **C10.** When the player digs sandstone, what goes in their inventory — "sandstone" or the individual constituent voxel types? (Affects crafting design a lot.)
sandstone
- **C11.** Should the engine support voxel *transparency at the material level* (glass, water) with per-voxel varying opacity, or fixed per-material?
per voxel
- **C12.** Max world size: is 64-bit voxel coordinates (±2.8×10¹¹ meters, ~1,800 AU) enough for "infinite", or do you literally need unbounded/arbitrary-precision coordinates?
64 bit is enough

## D. Rendering

- **D1.** **BLOCKING (Stage 3)** — Target resolution and framerate for the minimum spec. e.g. "1080p 60" or "1080p 30 with internal 720p render scale".
720p 30 fps
- **D2.** Is dynamic resolution scaling acceptable (render at lower internal resolution and upscale when frame time spikes)?
yes
- **D3.** Are you okay with temporal accumulation (a frame reusing the previous frame's lighting)? It's essentially mandatory for path tracing at these speeds, but it produces slight ghosting on fast motion.
yes
- **D4.** How many light bounces do you want as the default quality? (1 bounce + sky is dramatically cheaper than 3.)
default is 2 + sky
- **D5.** Sun/sky: one directional sun plus a sky model, plus unlimited emissive voxels? Any other light types (spotlights, point lights as entities)?
yes
- **D6.** Should emissive voxels be true light sources (any voxel can light the world) or only specially flagged ones? True-any-voxel is beautiful but needs an emissive-voxel acceleration structure.
yes
- **D7.** Dispersion is expensive (multiple wavelength samples per refracted ray). Should it be always-on, on for specific materials only, or a quality setting?
for specific materials only and a quality setting
- **D8.** Caustics: acceptable to compute them progressively (they "converge" over ~0.2 s and are slightly soft), or must they be sharp and instant?
progressively
- **D9.** Do you want a non-path-traced fallback renderer for the weakest hardware (direct sun + cached ambient, no GI)? Recommendation: yes, and it comes free from the face-cache design.
yes
- **D10.** Screen-space effects allowed at all (SSAO, screen-space reflections) as a low-end fallback, or strictly world-space only?
low end fallback
- **D11.** Should distant voxels (sub-pixel) keep their individual colors averaged (correct, slightly blurry) or snap to a dominant color (crisper, aliased)?
correct slightly blurry
- **D12.** Do you want visible voxel grid edges / bevels / a slight normal variation to read the voxel scale, or perfectly flat faces?
flat
- **D13.** Fog / atmospheric scattering — in scope? At infinite render distance, some aerial perspective is nearly required for depth readability.
yes, volumetric
- **D14.** Motion blur, depth of field, bloom, exposure/auto-exposure — which of these do you want?
all of them, physically correct based on the pt, motion blur is also speed based
- **D15.** First person, third person, or both? Third person means the player model is always visible and always shaded.
first person but the camera is stuck between the eyes of the playermodel and you can see your own playermodel
- **D16.** Is there a UI style direction? (In-world diegetic UI vs. conventional 2D overlay.)
diegetic ui

## E. Simulation

- **E1.** **BLOCKING (Stage 11)** — What tick rate should the world simulation run at? Candidates: 20 Hz (Minecraft-like, cheap, network-friendly), 30 Hz, 60 Hz (fluid looks much better, 3× the cost and bandwidth).
20hz
- **E2.** Should simulation run on the GPU (fast, huge scale, determinism is hard) or CPU (slower, easy determinism)? Recommendation: GPU with strictly integer arithmetic and a fixed pass order, which *is* deterministic if we never use float or unordered atomics. Do you accept that constraint on all simulation code?
yes i accept
- **E3.** How far from a player should simulation stay active? (e.g. 128 m radius.) Beyond that, matter freezes exactly. Is frozen-far-simulation acceptable, or do you need distant water to keep flowing while nobody watches?frozen far simulation is acceptable but it must remember where all things were including fluids solids etc and the state they were at
- **E4.** Fluids: do you want *volume-conserving* fluid with pressure (water finds its level, flows uphill under pressure, in pipes) or simple falling-water? The former is much better and much more expensive.fluid with pressure
- **E5.** Should fluids have horizontal momentum/current, or only settle-and-spread?
horizontal momentum and current, fluids should also mix
- **E6.** Gases: do they mix (smoke + steam), have buoyancy by density, and disperse to nothing over distance, or must they also be exactly conserved forever? Note: exact conservation of gas means smoke never disappears — it just becomes very thin. Is that the intent? (It is defensible and quite cool, but needs a "gas density below threshold merges into atmosphere" pool to stay cheap. That pool keeps global conservation.), they do mix have buoyancy and are exactly conserved
- **E7.** Temperature: full heat diffusion between voxels, or a simplified "burning/hot flag with a timer"?
full heat diffussion with air temperature too
- **E8.** Should structural integrity exist (unsupported structures collapse)? If yes, at what granularity — per connected component, or a stress solve?
yes, you decide
- **E9.** Rigid bodies: when a player cuts a building in half, should the disconnected part become a physics object automatically, or stay floating until pushed?
become physics automaticaLLY
- **E10.** Max simultaneous rigid bodies you want to support (100? 10,000?), and max voxels per body.
infinite, infinite
- **E11.** Do rigid voxel bodies deform/dent on impact, or only fracture into smaller bodies? can be both
- **E12.** Cloth/rope: how simulated? Real constraint solving (accurate, expensive) or verlet approximations? How many cloth particles at once?
verlet aproximations
- **E13.** Should a fluid be able to soak into / interact with cloth and soft bodies, or do the systems just collide?
soak, fluids also can leave stains on things
- **E14.** Explosions: instant sphere removal, or a propagating pressure wave through voxels? The latter is far cooler and fits the CA model.
later
- **E15.** Should sleeping/frozen regions be visually distinguishable in a debug view? (I'll build it either way, asking whether it's a shipped feature.) in debug they should look different
- **E16.** Sound: is audio in scope for v1? Voxel-material-based footsteps/impacts?
not in scope for v1

## F. Terrain and generation

- **F1.** You want fully 3D fractal terrain, no heightmaps. Should the *same* fractal continue infinitely with no biome boundaries, or should there be distinct biomes/regions?
the biomes themselves should be open ended, also fractal like, infinite biomes, might blend between each other or not different blending types too infinite biome variability
- **F2.** Should terrain vary at *all* scales (a mountain looks like a boulder looks like a pebble), or should specific scales have specific character? you can edit before generating the world the size of anything and everything
- **F3.** Caves, overhangs, floating islands, and enclosed cavities — all expected, correct?
yes
- **F4.** Is there a "sea level" / global water, or is water purely local?
local
- **F5.** Should terrain generation be authorable by the player (a node graph for world generation), or fixed by the game?
authorable, you can even pick what terrain materials are made of, and wether the world uses buildings instead of trees or anything you created or got saved
- **F6.** Is a flat creative world a permanent shipped mode, or just a dev scaffold?
a permanent shipped mode
- **F7.** Should generation be reproducible from a seed, so that only *edits* need to be stored/transmitted? (Strongly recommended — it makes both saving and multiplayer far cheaper. Confirm.)
yes
- **F8.** How deep does the world go? Infinite downward too? Is there a bottom or a core?
infinite
- **F9.** Vegetation/structures — in scope, or terrain only for v1?
not in scope for v1

## G. Player, characters, animation

- **G1.** Player is 2 m / 64 voxels tall. Is the character body made of voxels at the same 3.125 cm scale, or a finer scale for detail?
same scale
- **G2.** Is the character a rigged skeleton with voxel volumes attached to bones, or a freeform voxel blob with a procedural rig? **BLOCKING (Stage 15)**
freeform voxel b lob with procedural rig
- **G3.** How much customization: body proportions, non-humanoid bodies (four legs, wings, no legs), or humanoid-with-cosmetics?
infinite customization except game breaking survival or multiplayer breaking like being invisible or the size of a mountain
- **G4.** If non-humanoid bodies are allowed, how does locomotion animation work — hand-authored per-rig, or procedural (IK-driven gait)?
procedural
- **G5.** Should characters be destructible/damageable at voxel level?
yes
- **G6.** Are character voxels simulated (hair as rope, cloth capes) or static?
simulated
- **G7.** Player abilities in creative: fly, noclip, infinite reach, scaling brush size? Any of these need to exist in multiplayer with permissions.
all of those
- **G8.** First-person hands/tools visible?
yes

## H. Building tools

- **H1.** What brush shapes do you want? Sphere, cube, cylinder, line, plane, flood-fill, extrude, copy/paste volume, mirror, radial symmetry?
for now just chisel, this is you press right (place) or left (carve) click setting first point then move your camera and release setting the second point, first point is whatever voxel youre looking at, second point is the voxel a certain distance from the camera
- **H2.** Should there be an in-game selection/clipboard system with rotation and scaling of pasted volumes? (Scaling voxel volumes is non-trivial — resampling loses detail.)
yes, and you can save them onto your library of creations (voxel creations are called clips)
- **H3.** Undo/redo depth and whether it's per-player in multiplayer.
yes, per player
- **H4.** Should players be able to save "prefabs"/blueprints and stamp them?
yes, and even procedural clips
- **H5.** Is there a blueprint/schematic import from other voxel formats (.vox / MagicaVoxel, .schematic)? Note: reading `.vox` is trivial and license-free.
yes all of those
- **H6.** Terrain smoothing/sculpting tools (erode, smooth, noise-add) in addition to place/erase?
yes
- **H7.** Should there be a "material eyedropper" and a material mixing/authoring UI in-game?
yes

## I. Logic and mechanisms (future but affects design now)

- **I1.** Logic style: redstone-like signal propagation through voxels, wire entities, or a node-graph "programming" panel?
node graph programming/visual programming too. and wires and logic components similar to little big planet or dreams (ps4) but logic components and wires can be destructed and are subject to physics and can be attached to objects
- **I2.** Should logic be simulated in the same CA pass as matter, or a separate system with its own tick?
whatever is better
- **I3.** Mechanical parts: are pistons/motors/hinges *voxel-built* (player assembles them from voxels) or *placed components* with fixed behavior?
placed components but made out of voxels and highly customizable in behaviour and looks and destructable
- **I4.** Can a machine be arbitrarily large (a 500 m crane) and still articulate? That constrains the rigid body solver a lot.
yes
- **I5.** Do you want vehicles a player can ride (a moving reference frame with a player standing on it)? That is a significant amount of extra work and needs to be planned for early.
players must create their own vehicles with the mechanics and logic of the game and yes they can stand on them and will move them and even conserve their momentum so they can even jump while being on top of something moving
- **I6.** Should logic be able to *create and destroy* voxels (a drill, a printer)? If so, conservation of matter needs a source/sink accounting model.
yes

## J. Multiplayer

- **J1.** **BLOCKING (Stage 14)** — Max simultaneous players in a world?
32
- **J2.** How do players find each other? Options: (a) invite code / paste a connection string, (b) friends list via a platform (Steam), (c) public lobby browser (requires a server — conflicts with "no servers"), (d) LAN discovery.
a (with the username they choose to type, if two people are called the same you just add "2" or "3" to the second or third person with that name), b and d
- **J3.** Is it acceptable that a small fraction of network configurations (symmetric NAT on both ends, ~5–10% of pairs) cannot connect *directly* and must relay through another player in the session? That is the only truly free, serverless fallback.
yes
- **J4.** Is the host a player (their machine is authoritative and they get an advantage in latency), or should authority be distributed?
authority distributed
- **J5.** What happens when the host leaves — session ends, or host migration?
players stay on that world and it can be their own world
- **J6.** Should worlds be portable — a player takes the save file and hosts it themselves later?
yes
- **J7.** Is cheating a concern? A player-hosted, deterministic model has essentially no anti-cheat. For a creative sandbox with friends this is usually fine — confirm.
only for the future survival mode
- **J8.** Is voice chat / text chat in scope?
yes but not for v1
- **J9.** Do you want permissions (who can edit what region)?
no
- **J10.** Expected/tolerable bandwidth per player. Something like 50–200 KB/s upstream for the host per peer is realistic; is that acceptable for your audience?
whatever you choose
- **J11.** Should players be able to be in *very* distant parts of the world simultaneously (which multiplies the simulation cost by number of active regions)?
yes

## K. Persistence and content

- **K1.** Save format: single file per world, or a directory of region files? Recommendation: directory of region files + a seed + an edit journal.
single file per world
- **K2.** Should saves be forward/backward compatible across versions, or is breaking saves acceptable during development?
breaking saves is acceptable
- **K3.** Autosave frequency and whether saving can stall the frame (it must not).
every time you modify something yourself or close the world, stalling is not permitted
- **K4.** Any interest in a replay system (record the op stream, play back the world's history)? It comes almost free from the deterministic op model and is an amazing feature.
sure but for much much later
- **K5.** Modding: what surface should mods have? Materials/reactions only, or full scripting?
full scripting, anything, modding is a huge part of the game

## L. Art, audio, UX

- **L1.** Do you have art direction references? (Teardown, Minecraft, Noita, Astroneer, Trove — which is closest?)
no
- **L2.** Who makes the art assets — you, me procedurally, or someone else?
anyone, built ins are made by you and maybe tweaked and perfected by me, you make procedural things only
- **L3.** Font/UI licensing: I will only use OFL/Apache fonts. Any preference?
a pixelated font
- **L4.** Is there a target audience/age?
no
- **L5.** Accessibility requirements — colorblind modes, remappable controls, motion sickness options (FOV, motion blur off, no head bob)?
remappable controls
- **L6.** Localization in scope for v1?
no

## M. Networking transport (technical, needs your call)

- **M1.** **BLOCKING (Stage 14)** — Which NAT traversal strategy is acceptable, given "no servers, no router changes, free forever"?
  - (a) **Public STUN + UDP hole punching + peer relay fallback.** Fully free, uses public STUN servers (free, run by Google/Cloudflare/others) purely to learn your own public address. No server of ours. ~90% direct connect rate; the rest relay through another player. **Recommended.**
  - (b) **BitTorrent Mainline DHT for rendezvous.** No servers at all, not even STUN. More complex, slower to connect (5–30 s), and some networks block DHT traffic.
  - (c) **Steam Datagram Relay** (if shipping on Steam). Free for the developer, essentially 100% connect rate, Valve pays for the relays. Requires Steam.
  - (d) **IPv6-direct-first** with (a) as fallback. Increasingly viable; many ISPs give every device a routable IPv6 address, meaning no NAT at all.
  - My recommendation is (d) → (a) → peer relay, plus (c) as an optional extra if you ship on Steam.

do your recommendation d, a then c when i publish to steam
- **M2.** Are you willing to ever run *any* tiny piece of infrastructure (a 5 MB rendezvous binary on a $0 free-tier host)? If yes, connection reliability goes to ~100% and the design gets simpler. If no, we go with M1(d)+(a)+relay.
no
- **M3.** Is LAN play (no internet at all) a requirement?
no

## N. Questions I need answered about your intent, not just implementation

- **N1.** When you say "billions of voxels", do you mean billions *stored and simulated* at once, or billions *visible on screen* at once? These need very different engineering. (My reading: billions visible via the LOD hierarchy, tens of millions actively simulated. Confirm.)
your reading yes
- **N2.** When you say "no LODs", do you mean (a) no *visible popping or quality steps* — which is achievable — or (b) literally rendering every individual voxel out to infinity, which is mathematically impossible since it's unbounded data? Please read `11-reality-check.md` §1 and confirm my interpretation.
i mean visible popping or quality steps, and its not even visible or not, there shouldnt be at all steps
- **N3.** "Path tracing that colors voxel faces, not pixels" — I've designed this as a world-space face radiance cache (see `04-rendering.md`). Refraction, dispersion, and mirror reflections are *view-dependent* and physically cannot be stored per face; they must be per-pixel rays. Do you accept a hybrid: diffuse/rough lighting cached per face (cheap, low-noise) + a small per-pixel budget for glass/mirrors? yes
- **N4.** How much do you care about physical correctness vs. how it looks? E.g. real Snell's law vs. a fast approximation that looks the same in 95% of cases. 
fast aproximation
- **N5.** What is the single feature that, if it did not work well, would make you consider the project a failure?
bad performance, low fps, low resolution for a playable experience, short range of high detail voxels
- **N6.** What is your appetite for a longer Stage 0–2 (boring foundations, tests, tooling) in exchange for the rest going much faster and breaking much less?
absolute, you can make it as long as you need
- **N7.** How do you want to work: do you want to read/understand all the code, or do you want it to work and stay out of your way? This changes how much I explain and how exotic the optimizations get.
i wish to understand more or less how it works but in a very superficial way in a language that isnt technical and is just a non code jargon explanation and common language explanation.
> Answered. `12-plain-english.md` exists for this and is updated every stage.

---

# ROUND 2

*Round 1 is fully folded into the docs (see `13-decision-log.md`). **Nothing below blocks Stage 0** — I can start building while you think about these. Same format: put your answer on the line under the question.*

## O. Follow-ups and ambiguities

- **O1.** The Unlicense is a public-domain dedication rather than a license grant, and some jurisdictions and companies treat that as legally shaky. **MIT-0** or **CC0-1.0** give the same practical freedom with less ambiguity. Keep the Unlicense, or switch?
switch

- **O2.** B12 and E3 read slightly differently to each other. My assumption: when you leave an area or close a world, everything **freezes exactly** — every fluid level, temperature, current and stain preserved — and resumes bit-perfectly when you return. Nothing settles, drains or burns down while you are away. Correct? Or did you want the world to *catch up* on the elapsed time when you come back (fires burn out, water finishes draining)?
it catches up but not while you close the world, this allows for weathering of structures etc

- **O3.** E14 — you wrote "later". I read that as "the latter", i.e. explosions are propagating pressure waves rather than instant sphere removal. Confirm? (If you meant "do explosions later in development", that also works, but the design differs.)
i meant latter
- **O4.** Voxel types are deduplicated, so identical voxels are free. If someone builds something with tens of millions of genuinely unique voxels and the type table fills up, should the game (a) just warn and keep going until memory runs out, (b) auto-merge near-identical types (imperceptible colour rounding) to reclaim space, or (c) stop accepting new unique types? My recommendation: (b), with a visible indicator.
b, but only for non visual properties, visual properties should allow for billions of voxels with their own unique color or material parameter combination
- **O5.** The chisel: is a **box** the only shape for now, or do you also want the same drag interaction to produce spheres/cylinders/ramps via a mode key? And what should the default "distance from camera" for the second point be — fixed, scroll-adjustable, or snapped to whatever surface the ray hits?
i also want it to produce shapes in the future, actually, make both first point and second point a distance from the camera its just scroll adjustable if you hold a key like G, if its set to a distance of 0 it instead switches to be whatever voxel you aim at so its not by distance if its a distance of 0, make it so that you can also set extra points with middle click in a way so that the shape you draw will have to have those voxels of the extra points at their edge
- **O6.** Survival is "future" but you flagged anti-cheat as mattering then (J7). Should v1 already store the things survival needs (matter scarcity ledger per player, owner-side validation hooks), or is a later retrofit acceptable? Recommendation: build the hooks now — they are nearly free and expensive to add later.
build the hooks now
- **O7.** Gravity-warping artifacts (B6) — what are they in the fiction? A placeable object with a radius? A painted volume? A logic component you build? And does gravity in the affected zone point toward a point (planet-like), along a direction, or away (repulsion)?
actual voxels with a gravity warp property, it can be toward a point and or repulsion
- **O8.** "Infinite customization except game-breaking" (G3) — what are the actual limits? I suggest character height 0.5 m – 4 m, volume within 4× the default, no fully transparent or invisible materials on a character in multiplayer. Adjust as you like.
whatever you consider appropiate
- **O9.** When you stamp a clip into a world, should it become ordinary world voxels by default, or a free-standing physics object? (Both will exist; asking about the default.)
depends on the clip, a basketball ball is free standing, a house is ordinary world voxels
- **O10.** Fluid mixing: I plan a maximum of 4 mixed components per voxel (e.g. water + oil + dye + silt), which is plenty for anything visible and keeps the tick cheap. Acceptable?
infinite mixed components
- **O11.** Worst-case multiplayer bandwidth: all 32 players in one room costs about 120 KB/s upstream each — roughly a 1 Mbit upload, which excludes some slow rural connections. Options: accept it, or cap the *observed* player count (you see everyone, but full-fidelity animation for the nearest N). Recommendation: accept, plus automatic fidelity falloff for distant players.
do your recommendation
- **O12.** How diegetic is the diegetic UI? Is the **main menu** also an in-world 3D space, or a conventional screen with the in-world treatment reserved for gameplay UI (tool settings, clip library, logic panels)? Fully in-world main menus are lovely but slow to navigate and slow to build.
everything is diegetic except exceptionally hard things to implement
- **O13.** Naming: executable `WorldShaper.exe`, worlds `.wsworld`, clips `.wsclip`, mods `.wsmod`. Any preferences?
whatever you see fit
- **O14.** Should the performance overlay (FPS, frame time, memory) be a shipped player-facing feature, or dev-only?
shipped, off by default but you can turn on in settings
- **O15.** **Do you own a Steam Deck?** The perf gates say "tested on real hardware at every playable checkpoint". If not, I will build against Deck-equivalent settings and simulated bandwidth limits, and we validate later — worth knowing now.
no
- **O16.** For Stages 3–5 I need a test world before terrain exists. Should it be a plain flat plane, a hand-built test scene (rooms, towers, tunnels — better for testing lighting and collision), or a simple noise landscape? Recommendation: a scripted test scene, since it doubles as a regression benchmark.
scripted test scene
- **O17.** Modding is Lua. Should native code plugins (compiled extensions) ever be allowed, or is Lua the only surface forever? Native plugins are more powerful but are a security and stability problem in multiplayer.
they are allowed
- **O18.** When you join a world using mods you do not have, should the game **download them automatically from peers**, or refuse to connect and tell you what to install? Auto-download is far nicer and is how it should eventually work, but it means running other people's code — I would want it sandboxed and off by default outside a friends list.
it auto downloads but it first tells you what it will download and only from friends
- **O19.** Should the world-generation node editor and the logic/programming node editor be **the same editor** with different node sets? Recommendation: yes — one system, half the work, and players learn one interface.
yes
- **O20.** NPCs (B2): are creatures entirely player-authored (sculpt a body, wire up behaviour with logic/Lua), or does the game also ship built-in creatures? Recommendation: player-authored as the system, with a few built-in examples that are themselves made with that system and fully editable.
your recommendation
## P. Ready to build

- **P1.** Anything in the revised plan you want changed before I start?
i wont read it its too long
- **P2.** Shall I begin **Stage 0** (build system, window, GPU setup, test harness, debug HUD, one-click run)? It produces a boring but real executable and every later stage stands on it.
yes

also, bear in mind the only thing i want you to reuse from the deprecated worldshaper project is the style of the ui, its on github maybe you have access if you dont tell me, only the style and transparent opposing ink thing, neither the structure or anything just the vibe.

you can also look at the controls tooltips which i would want you to implement something like that too

---

# ROUND 3 — raised per stage, as the working rules say

## Q. The shell (Stage 15) — raised against the specification of 2026-08-11

- **Q1.** "all values must be sliders which you can also type their value … any value with no caps" —
  does *no caps* mean **no limits on the value**, or **no capital letters in the interface's text**?
  The two are different work and both read naturally there.

**A: no limits on values.** A slider shows the useful range; typing goes past either end. Recorded as
D444, with the two boundaries the rule needs written beside it: a value that would genuinely break
the game is still refused *and says why*, and a value that is not a number is not a slider.

- **Q2.** The community tab searches "files or folders any player has, auto published". Answer M2 says
  no infrastructure of ours, ever, and a search over strangers' content normally needs somewhere to
  look. Which way — reachable peers only, an index somewhere, or publish-on-purpose plus an index?

**A:** *"it uses multiplayer laws like free multiplayer with no hosting and no port forwarding so that
you can browse any file or folder any online player has at that moment, files and folders are tagged
with the user that made them too and it shows it, even within your own library it says the original
author, if the original author deletes his copy its still on your library and it still says who made
it but its just not available on the community browser anymore."* Recorded as D448–D451, with the one
limit that follows from having no rendezvous: the browser reaches who you can reach, and the DHT that
would widen it to strangers is named as its own sub-step rather than assumed.