# 19 - Automatic quality

## What it does

Holds a frame rate by spending detail where it is worth most. The target is the monitor's own
refresh rate, because rendering faster than the display can show is work nobody sees — and on
a machine with room to spare that time is better spent on samples than thrown away.

The first time the game runs it **measures the machine it is on** rather than guessing, and
remembers the answer.

## What was measured first

At 1280x720 on the development machine, one pass dominates each mode and everything else is
rounding error:

| Pass | Real time | Path traced | Path traced, enclosed room |
| --- | --- | --- | --- |
| streaming | 0.000 ms | 0.000 ms | 0.001 ms |
| **visibility** | **1.233 ms** | — | — |
| **pathtrace** | — | **2.495 ms** | **19.988 ms** |
| resolve | 0.065 ms | — | — |
| blit | 0.011 ms | 0.011 ms | 0.012 ms |
| hud | 0.004 ms | 0.004 ms | 0.005 ms |
| **frame** | 1.313 ms | 2.510 ms | 20.006 ms |

So the knobs worth having are the ones inside those two shaders, and the enclosed path-traced
case is where the headroom has to come from — it is eight times its own outdoor cost.

Streaming, resolve, blit and the HUD together are under a tenth of a millisecond. They get no
knob at all: a setting that cannot move the frame time only costs the player a decision.

## The ladder

Eight levels, in `src/game/quality.hpp`. They are ordered by **what each costs to look at**,
not by what each saves — the cheapest harm is spent first.

| Level | Resolution | Detail bias | Refine stride | Bounces | Shadow target |
| --- | --- | --- | --- | --- | --- |
| 7 | 1.00 | 1.00 | 2 | 64 | 128 |
| 6 | 1.00 | 1.00 | 4 | 48 | 96 |
| 5 | 1.00 | 1.00 | 5 | 32 | 80 |
| 4 | 1.00 | 1.05 | 6 | 20 | 64 |
| 3 | 1.00 | 1.15 | 8 | 12 | 48 |
| 2 | 0.80 | 1.25 | 10 | 8 | 40 |
| 1 | 0.65 | 1.40 | 12 | 6 | 32 |
| 0 | 0.50 | 1.60 | 16 | 4 | 24 |

Sample counts and the refine stride move first: they cost noise while the camera is moving and
nothing at all once it stops, so on a machine that can hold the target while still they are
free. Resolution moves last and only on the bottom three rungs, because scaling the whole
picture is the one change nobody fails to notice.

Note the top rung is **better than the old fixed defaults** — stride 2 against 4, shadow target
128 against 96. On a machine with headroom the game now looks better than it used to, which is
the point of measuring rather than picking one setting for everybody.

## The first run

Ninety frames of warm-up, then ninety frames timed at full detail. The warm-up matters: shaders
are still compiling and the first chunks are still arriving, and timing those measures the
loading screen rather than the machine.

The result picks a starting rung, **one below what the measurement strictly allows**. The
benchmark is one camera in one place and the first thing a player does is walk somewhere more
expensive; being asked to drop a level in the first ten seconds reads as the game misjudging
itself, where quietly climbing after a while reads as nothing at all.

Re-run it any time with `--benchmark`.

## Settings

`%LOCALAPPDATA%\WorldShaper\settings.txt`, one `key value` per line, on purpose:

```
target_fps 59.79
quality_level 7
auto_quality 1
```

A player who wants to force a level should be able to open it in Notepad and see what the game
decided about their machine. A missing or half-written file costs a benchmark, not a crash.

Written on exit rather than on every change — a controller that touches the disk each time it
moves would write during exactly the stutter that made it move.

Command line, until there is a settings screen:

```bash
WorldShaper.exe --target-fps 144 --quality 5 --no-auto-quality --benchmark
```

## How it moves

- Frame times are smoothed, so one stalled frame — a chunk arriving, a window dragged — never
  drops the quality for everybody.
- Twenty consecutive frames over budget to drop; ninety under it to climb. Dropping is quicker
  because being below the target is felt immediately and being above it is not felt at all.
- It only climbs when there is room for the *next* rung too. Without that margin a controller
  steps up into a frame time it cannot hold, drops back, and spends its life alternating —
  which is worse to look at than simply sitting one rung low.

## Scripted runs never adapt

`--screenshot` disables the controller, skips the benchmark, and **ignores the saved level**,
rendering at full detail unless `--quality` says otherwise. Otherwise every measurement in this
repository would be taken at whatever quality the last interactive session settled on, and two
numbers taken a day apart would not be comparable.

## Not done yet

`resolution_scale` is carried by the ladder but **not applied**. The dispatch size, the
parameter block's resolution and the descriptor-bound images all derive from the swapchain
extent, so rendering smaller than the window means changing all three together and making the
blit scale up. That is a change to the frame's structure rather than a knob, and it is the next
piece of work. Until then levels 0-2 differ from 3 only in their other knobs.
