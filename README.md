# WorldShaper

A voxel creation game. Everything is a real voxel — no parametric surfaces, no analytic
shapes at runtime — at **32 voxels to the metre**, which is a little over 3 cm each.

Written from scratch in C++20 and Vulkan 1.3. No engine.

## Look at the clips in your browser

### **[worldshapergame.github.io/WorldShaper](https://worldshapergame.github.io/WorldShaper/)**

Every clip in this repository, drawn in a browser and made for a phone. Pick one, turn it
round, drag the slider along the top to cut it in half and see inside, or switch to **Walk**
and go up the steps and in through the door. Tap the up button twice to fly.

It is baked from the clip files by the game's own sampler and rebuilt on every push, so a
fragment somebody changed ten minutes ago is what you are looking at — and a clip open on the
page reloads itself, in place, when it changes. The materials are the clips' own; the light is
baked and rasterised rather than path traced, because a phone cannot path trace.

`documentation/24-clip-viewer.md` is how it works.

## What is actually built

Stages 0–4 are complete and measured; Stage 5 is most of the way there. In the current
build you can fly around a scripted test scene, carve and place at 3 cm resolution, copy
regions and stamp them back with rotation, resizing and repeats, and undo any of it without
limit.

| | |
|---|---|
| **Renderer** | Ray marcher with continuous detail — no LODs, no view-distance setting. A mountain 40 km away costs about what a rock at your feet costs, for the same number of pixels. |
| **Streaming** | The renderer says what it wants: rays that reach a place the world has and the graphics card does not write it down, and it gets loaded. Nothing is loaded by radius. |
| **World** | 64-bit voxel coordinates, sparse chunks of 256³, bricks of 8³ that pick their own encoding. About 0.15 bytes per voxel on the test scene. |
| **Edits** | Every change to the world is an `Op` — a small, deterministic, serialisable record. `apply_op` is the only function that writes voxels, which is what makes replay and multiplayer an integration rather than a rewrite. |
| **Conservation** | A matter ledger tracks every voxel that appears or disappears and is audited against a full recount. Nothing is created or destroyed without something saying so. |

## Building

Windows, Visual Studio build tools, and the Vulkan SDK.

```bat
build.bat
```

`build.bat clean` throws the build directory away first. `test.bat` runs the unit tests, a
headless world audit and a node pool audit — one small screenshot, taken only so that the
three checks which run inside every frame get a frame to run inside. `run.bat` starts the
game.

## Controls

Tap **1–9** to pick a tool slot; hold a number and scroll to cycle the tools on it. Chisel
on 1, clipboard on 2.

**Move** — WASD, space up, C down, shift faster, wheel for speed.

**Chisel** — hold left to carve, right to place. The press sets one corner, the release the
other. Both corners sit at a distance in front of you: hold **G** and scroll to change it,
or wind it to zero and it snaps to the voxel under the crosshair. Middle click drops a
constraint point the shape has to reach. **P** overwrite, **O** first point, **Q**/**E**
material.

**Clipboard** — select a box the same way and you get a see-through ghost of the real
voxels. Scroll to slide it (it speeds up the longer you keep going), shift+scroll a whole
clip length, middle click to send it to the crosshair. **.** and **,** adjust whatever **/**
is pointed at — how many copies, or the size. Arrow keys turn it; in size mode they stretch
along the axis you are facing. Left click stamps, right click drops it.

**Both** — **Z** undo, **X** redo, **R** clear, **backspace** cancel.

**F1** developer panel, **F2** overlay, **F3** debug views, **F5** reload shaders, **F11**
vsync.

## Documentation

[`documentation/`](documentation/) is the design in full — the vision, every question asked
and answered, the architecture, and a decision log with the reasoning behind every choice
including the ones that turned out wrong.

One file is worth starting with:

- [`07-roadmap.md`](documentation/07-roadmap.md) — twenty-four stages with nineteen playable
  checkpoints, and where things actually stand.

## Licence

[MIT-0](LICENSE). Do anything you like with it, attribution not required.
