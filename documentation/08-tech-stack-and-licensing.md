# 08 — Tech Stack and Licensing

*Revised after answer round 1: **C++20 confirmed** (A1), **Lua modding pipeline** (A1, K5), **Unlicense open source, Steam later** (A10–A12), **Steam Deck feature floor** (A6), **Windows first, no macOS** (A5), **pixel font** (L3).*

## 0. The project's own license

WorldShaper ships under **MIT-0** (MIT No Attribution) — answers A10 and O1. Practical consequences:

- Anyone may use, modify and redistribute the code for anything, with **no attribution requirement** — the same practical freedom the Unlicense was chosen for, but as a real license grant, so no jurisdiction or legal department rejects it.
- **Third-party code keeps its own license.** MIT/BSD/Apache dependencies require their notices reproduced, so the build generates `THIRD_PARTY_LICENSES.txt` shipped beside the executable. The bundle is therefore not attribution-free even though our code is.
- Fine on Steam; Steam's terms concern distribution, not licensing.

**Hard rule for dependencies:** every one must be usable in a commercially distributed, royalty-free game with no copyleft obligation. That means **MIT, BSD-2/3, Apache-2.0, zlib, ISC, BSL-1.0, CC0, or public domain**. Nothing else, ever, including tools that ship with the game.

**Banned outright:** GPL/AGPL (copyleft), LGPL when statically linked (and we static link everything), CC-BY-NC/CC-BY-SA assets, anything with a royalty, revenue share, seat fee, or "free until you earn $X" clause, and any SDK whose terms can change under us.

---

## 1. The stack (decided — answer A1: C++ with a Lua modding pipeline)

### C++20 + Vulkan 1.3

| Component | Library | License | Why |
|---|---|---|---|
| Window / input / gamepad / audio device | **SDL3** | zlib | The most permissive license in existence; one API for Windows/Linux/macOS/Steam Deck |
| Graphics + compute API | **Vulkan 1.3** | Khronos, Apache-2.0 headers | Bindless, async compute, subgroup ops, 64-bit atomics. Fully supported on Steam Deck (RDNA2), which is the floor |
| **Scripting / modding** | **Lua 5.4** (or LuaJIT) | MIT | Answer A1. Sandboxed, one VM per world, op-emitting API only |
| Vulkan loader | **volk** | MIT | Avoids the loader DLL indirection |
| GPU allocator | **VMA** | MIT | Sub-allocation, defrag; writing this ourselves is a waste of a month |
| Shader language | **Slang** | MIT (Khronos-hosted) | Generics, modules, autodiff; compiles to SPIR-V. Massive quality-of-life win for a path tracer |
| Shader fallback | **glslang** | BSD-3 / MIT | If Slang is dropped for any reason |
| Compression | **zstd** + **LZ4** | BSD-3 (+ patent grant) / BSD-2 | Chunk saves and network bulk transfer |
| Hashing | **xxHash** | BSD-2 | Chunk digests for reconciliation |
| Crypto | **Monocypher** (or libsodium) | CC0/BSD-2 (ISC) | X25519 + ChaCha20-Poly1305 for the transport |
| Debug UI | **Dear ImGui** | MIT | Dev tools only; the game UI is our own |
| Profiler | **Tracy** | BSD-3 | Frame/GPU/lock profiling; indispensable for this project |
| Tests | **doctest** | MIT | Fast to compile, header-only |
| Image IO | **stb_image / stb_image_write** | MIT / public domain | Screenshots, icon/UI atlases |
| Font rasterization | **stb_truetype** | MIT / public domain | No FreeType (also fine, but this is simpler) |
| Fonts | **Ours** — a three-by-five pixel face drawn in `assets/font/` (answer L3, decisions D437–D440) | MIT-0, like the rest | A letter here is *matter*, so the face has to be as small as a legible letter can be and every glyph has to be ours to redraw. No third-party face is drawn at three pixels, and none may be modified into one under OFL without renaming it |
| Build | **CMake** | BSD-3 | Universal |
| *(later)* Steam integration | **Steamworks SDK** | Valve SDK terms | Only at Stage 23. Free, optional, and isolated behind an interface so the game runs identically without it (answer A11) |

Everything above is vendored into `third_party/` and pinned by commit, so no dependency can move, break, or relicense underneath us. A `LICENSES.md` is generated from the vendored tree and shipped with the game.

### Why the C++ choice needs extra discipline here

C++ has no memory safety, and there is no second engineer to catch a mistake (answers A2, A3). Compensations, applied from Stage 0:

- Address/undefined-behaviour sanitisers on every CI run and every debug build.
- No raw `new`/`delete`; pool and arena allocators only, with bounds-checked handles instead of pointers into voxel data.
- Static analysis in CI, warnings as errors.
- Fuzz tests on every data-structure boundary.

This is a real cost of the C++ decision, paid up front rather than in crash reports later.

---

## 2. Explicitly not used, and why

| Thing | Reason |
|---|---|
| Unity / Unreal / Godot | Unity and Unreal have revenue terms; Godot is MIT and fine legally, but its renderer and node model actively fight everything in this design. We need direct GPU control. |
| PhysX / Havok / Bullet | We need *deterministic fixed-point voxel* physics — none of these do that. Bullet is zlib and fine legally, just wrong for the job. |
| FMOD / Wwise | Proprietary with commercial terms. SDL audio + our own mixer instead. |
| Qt | LGPL; static linking obligations are a trap. |
| DLSS / FSR / XeSS binaries | Vendor SDKs with their own terms and hardware lock-in. Our own temporal upscaler, which is easier here because lighting is already temporally stable. |
| OptiX / CUDA | NVIDIA-only; Steam Deck is AMD. |
| Hosek-Wilkie sky model | Its reference implementation has a non-commercial-flavored license. We implement a physically-based single-scattering atmosphere from published equations instead. |
| Any "free tier" cloud service | Can start charging or shut down. Violates "free forever". |

---

## 3. Platform / GPU feature floor

Minimum is **Steam Deck** (answer A6) — which is a considerably *higher* floor than an old integrated GPU, so the engine can require modern features:

- **Vulkan 1.3** with: descriptor indexing (bindless), buffer device address, 8/16-bit storage, shader int64, **64-bit atomics**, subgroup ballot/arithmetic/shuffle, synchronization2, dynamic rendering.
- **Optional, used if measured faster:** hardware ray query (Deck has it), cooperative matrix, mesh shaders. Never required.
- Covers: Steam Deck / RDNA2+, NVIDIA Turing+, Intel Arc, modern AMD and Intel integrated parts.
- **Windows is the primary platform** (answer A5). Linux is kept building continuously because the Deck runs it and the Deck is the performance floor — so Linux support is free rather than an extra project.
- **No macOS.** MoltenVK lacks features this renderer depends on.

The binding constraint on the Deck is **memory bandwidth (88 GB/s shared)**, not features or compute. See `09-performance-budgets.md` §1.

---

## 4. Asset licensing policy

- No downloaded models, textures, or sounds unless CC0 or explicitly public domain, recorded in `ASSET_SOURCES.md` with the URL and license text.
- Procedurally generated content (noise-based materials) is ours by construction — a further argument for the "materials are pattern generators" design.
- Any AI-generated asset gets flagged in the same file so provenance is auditable later.
