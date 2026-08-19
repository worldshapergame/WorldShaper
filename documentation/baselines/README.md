# Baselines, and what they are and are not comparable with

Each `.csv` here is a grid recorded by `tools/baseline.ps1` at a named moment. They are the only
figures in this repository that a later run can be diffed against automatically, which is exactly
why the thing they cannot say has to be written down where they live.

**A baseline is comparable with a later run only if the WORLD is the same world.** `baseline.ps1`
matches its rows by (mode, view, size) and never compares across cameras — that part is safe, and
D633 measured it. What it cannot notice is the world itself moving underneath a row.

## The rows here that are no longer comparable, and why

**Every estate row predates D700 (2026-08-19) and is not comparable across it.** D700 bounded eight
paint rules that had been asked at every solid voxel of the estate, and **11,443 voxels — 8.1% of
the building — came back in different materials**, on purpose, because those materials were wrong.

The trap is that this is invisible to every instrument here:

- the **matter** did not move at all — 140,924 voxels, 165,812 faces, 6,015,997 shape evaluations
  and 235 components, identical on both arms over two runs each;
- so **voxel counts, face counts and geometry timings all still line up**;
- only the materials changed, and no column in these files carries a material census.

A run compared against a pre-D700 estate row will therefore agree on everything it checks and be
comparing two different buildings. **Re-record before quoting an estate row across that commit**, and
delete any `build/bin/clips/facility.world` first — it is a derived cache, git-ignored, and after
D700 it holds the old paint (see §5 of `22-rewrite-handover.md`).

## What is still good

`r0-face-count.csv` is a property of the geometry and the camera grid, not of the paint, so it
stands. `r11a-sample-cost.csv` is per-node sampling cost and its ABSOLUTE seconds are pre-D700 —
D700 cuts the estate's whole-clip sample roughly in half — but its shape (the ratio between levels,
and empty against with-matter) is what it was recorded for and that still holds.

## And the general rule this is an instance of

**Before comparing two figures, check the `scene:` line and its content hash** — that is trap 8 and
it is the oldest one here. This file exists because D700 is the case where the content hash is *not
enough on its own*: two worlds with identical geometry and different materials are two worlds, and
only a material census separates them.
