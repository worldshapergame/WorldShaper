# 26 — Integrating the fifteen: what is done, what is waiting, and the one collision

*Written 2026-08-16. Fifteen agents were given one piece each of "port every feature of the game's
path tracer to the phone rasteriser", each in its own git worktree. This is the state of that
integration and the order the rest of it should go in. It exists because the merge is the part that
was NOT delegated, and it needs doing with a clear head rather than at the end of a long session.*

---

## 1. Where each agent's work is

Every agent's tree is committed on its own branch, `worktree-agent-<id>`. Nothing is lost, and any
of them can be inspected with `git show <branch> --stat`.

| piece | branch suffix | state |
|---|---|---|
| shapes view shaded with real materials | `a9194590aad3cd747` | **merged** |
| paint stack export (`FLDG` + `PANT`) | `ad525c13d53b7b42d` | **merged** |
| ambient occlusion | `a048d1a63ecd106c5` | done, **conflicts** |
| sun shadow map + contact | `a667a5c956087be61` | done, not yet merged |
| colour irradiance volume | `a3152a149c26dcb5a` | done, **conflicts** |
| reflection probes | `acdc889a8d97e0f2d` | **done**, not yet merged |
| emissive light list | `ab5180c8f00d521a4` | **done**, not yet merged |
| material + thickness volume | `a37a9ce69787f558e` | **done**, not yet merged |
| screen-space reflections | `a643b6e516b884abc` | **done**, not yet merged |
| refraction / glass / translucency | `a99ae7b903fb4ca82` | **done**, not yet merged |
| BRDF: clearcoat, sheen, anisotropy | `afeeda78b79e4a87b` | **done**, not yet merged |
| post, bloom, fog, frame budget | `ab2021708dcf200a0` | **done**, not yet merged |
| GLSL field evaluator | `acc8d6b191e041a28` | **done**, not yet merged |
| paint stack evaluation | `a08d1f2578fdf6279` | **done**, not yet merged |
| paint verification + cost | `af84b1903c7344d71` | **done**, not yet merged |

## 2. The one collision, and it is the same one four times

**Four agents independently implemented the chunk directory.** They were each told to write it "as
if you are the first, and it merges if another already added it", which was right about the
intent and wrong about the mechanics: two implementations of one mechanism do not merge, they
conflict, and the marked regions cannot help because both edits are *in* their own regions and both
are correct on their own.

What differs:

| | paint export | ambient occlusion | irradiance |
|---|---|---|---|
| struct | `WebChunk` | `Chunk` | its own |
| writer | its own | `append_chunks()` | its own |
| `format.js` reader | inline | `parseChunks()` → `clip.chunks` | **`clip.chunk('FOUR')`, zero-copy** |

**Resolve it once, in this order, and the rest is mechanical:**

1. **Take the irradiance agent's `format.js` reader.** `clip.chunk('FOUR')` returning a zero-copy
   view or null is the right door for all fifteen and the only one that does not make every later
   agent edit the reader.
2. **Take the ambient-occlusion agent's `append_chunks()`** on the C++ side — it is the one written
   to be pushed into rather than edited, which is what four more agents still need.
3. **Rename `WebChunk` → `Chunk`** in the paint export's hunks, which is a `sed` and nothing more.
4. Then merge the rest in any order. Each adds one `Chunk` push and one `clip.chunk()` read.

**The header layout is not in dispute** — all four wrote the same one, which is the part that
mattered:

```
u32 at 200   chunkOffset        u32 at 204   chunkCount
entry, 16 bytes:  char fourcc[4];  u32 offset;  u32 size;  u32 reserved
FORMAT_VERSION = 3
```

Payloads are padded to sixteen bytes so a reader can take a typed-array view straight onto one; the
padding is not counted in `size`.

## 3. What must not be released until it is true

**The shapes view's colours are a stub.** `material_at` hashes each shape's own attributes into the
material table, so every shape gets *a* material rather than *its* material — the sampler's wall
panel is green because it is a hash, not because it is moss. The question that started this work was
whether the raw view shows the colours a clip will have in game, and shipping arbitrary ones is a
worse answer than shipping grey. **It releases when `web/js/features/paint.js` exports
`MATERIAL_AT_GLSL` and the shading file picks it up** — which is one line, already wired, behind a
caught dynamic import.

## 4. The findings that outlive the features

**A facility fragment carries 348 paint rules**, and `facility/terrace` exports **4,829 field nodes
at depth 54** with 22 rules reaching curvature, occlusion or facing. That is the real size of
"evaluate the paint stack at the marched hit point", per pixel, on a phone. The format ships
per-rule bounding boxes and a `COSTLY` flag so it *can* be made cheap. It is not yet cheap.

**A likely bug in the game, not the viewer.** `apply_origin` translates a paint rule's `test` and
leaves its `place` alone. On `facility/terrace`, `terr_damp` is authored at y 12.10–12.40 and its
unshifted box sits 3.5 m out, where the rule is pruned as unreachable — four weathering coats gone
silently. **`plan_sample` in `src/forge/sample.cpp` does not do that shift either**, so the game's
own sampler may be culling the facility's placed weathering coats against a box 3.5 m out of
position, with no error and no warning, on the only scene this project is judged against. Not
fixed: `src/` belongs to a second line of work. The agent with an arm on the real sampler was asked
to settle it with a material histogram rather than an argument.

**The light-grid bias trap has now bitten `web/js/gl.js` three times in one day** — the slice cap
twice and the shapes shading once — and every time the symptom was black. A fetch taken at a
surface reads the matter behind it. The fix is a bias along the normal of `lightCell` plus half a
voxel, and the control that proves it is not merely brightening: a point inside a doorway reads
68/76/92 before and 68/77/93 after.

**Two agents could not give a frame-cost number and were right not to.** The only GL here is
SwiftShader on a shared box, and the control arm varied 16% between two runs *of itself* — larger
than the effect being measured. Both quoted structural costs (passes, fetches, bytes) instead and
said why. Any absolute millisecond figure in this work is SwiftShader, never a phone.

---

## 5. Two decisions that must be settled at the merge, not by whoever merges last

**The capture target's resolution, and refraction is right.** The SSR agent built one offscreen
target at half the canvas in each axis, which is correct for reflections: a reflection is read
through a mip chain and does not care. The refraction agent took that same target — verified in a
throwaway integration tree rather than assumed, `sharedCapture: true`, no second target and no
extra pass — and then showed why the resolution is wrong for its own use: on `glass_test`, **the
wall seen through a clear pane has staircased edges the same wall beside the pane does not.** A
look straight through nearly-clear glass is a direct view, not a blurred one.

So: **one target, full-resolution colour, with the mip chain SSR wants built on top of it.** Half
resolution saves a quarter of the pixels on one pass and costs a visible artefact on every pane in
the building.

**Display-space capture is fine, and the decode must happen in the right place.** Both agents
independently wrote the same closed-form ACES inverse (delete one at merge). The refraction agent
established the constraint: the encoding must come off **before** Beer-Lambert and go back on
after, because transmittance attenuates radiance. Multiplying the encoded value instead
over-saturates — a white wall at radiance 3 behind ruby glass differs by about a fifth of green
and blue between the two routes. `RGBA16F` is not needed, which matters because it wants
`EXT_color_buffer_float` and a phone may not have it.

**Two traps found the hard way, both silent:**

- The fallback capture copy must be **`RGB8`, not `RGBA8`**. The canvas is `alpha:false`, so an
  RGBA destination is `INVALID_OPERATION` every frame, with a black texture and no other symptom.
- A **back-quote inside a GLSL comment** closes the JavaScript template literal and kills the page
  with an error naming whatever identifier follows it. It reads like a shader fault and is a JS
  one. It has now caught **three separate agents**, one of them twice, costing rounds of
  screenshots that silently rendered nothing.

## 6. What the fleet has shown it cannot do yet, and it is one thing

Three separate agents hit the same wall from three directions:

- **refraction**: `absorb` only demonstrates its *angle* dependence, because every pane in the
  viewer is currently 12 cm thick. A 300 mm block of ruby glass and a 12 mm pane of it come out
  identical — the exact comparison `_contract.clip` exists to make. That is `THCK`'s to fix.
- **refraction again**: a taper behind an alabaster pilaster **does not glow through it**, because
  the light volume holds sun and sky only. `clips/facility/chapel.clip` was built around that
  effect. Fixing it means the emissive lights reaching the irradiance volume.
- **the slice cap and the shapes view** still read one material for a whole clip, which is
  `MVOL`'s to fix.

All three are the same shape of gap: a term that is correct in the shader and starved of the baked
data it needs. None is a shader bug and none will be found by looking at a shader.

---

## 7. The frame, measured — and the fleet-wide worry does not reproduce

**The "every clip pays for every feature" alarm is not supported.** The SSR agent measured its own
feature branched entirely off at 1.8× a pristine build and flagged it as possibly making the whole
fleet's approach wrong. The budget agent took the control with a tighter instrument — the same
shader, same scene, same resolution, lobes *compiled out* versus merely *branched around*:

```
sampler, rung 4, 544x424    lean (compiled out)   858   838
                            fat  (branched round) 830   789
```

The lean build is if anything **slower**, by less than the noise floor. Four branched-around
lighting lobes and a fog function cost nothing measurable.

This is **not a refutation of the original observation** — SSR added two texture-sampling loops and
a `mat4`, a far larger register footprint than four branches — and **SwiftShader is not a mobile
compiler**, so neither number transfers to a phone. What can be said is that the *general* claim
does not survive a control. Settling it needs a real mobile GPU on an unloaded machine. The fix is
built regardless and lives in the ladder, where it belongs rather than in fourteen features:
`#define WS_LEAN` makes the feature test a constant so the compiler deletes the branches, and rung
4 lazily links a second program.

**The real finding is that the facility's frame is 96% the opaque pass.** Per-pass GPU timing,
64,250 quads: sky 50.6 ms, opaque 1721.8, glass 13.6. Every lever the quality ladder has lives in
the other 4%, and its biggest — a 4× cut in pixels — bought only 1.6×. **The frame is
geometry-bound and nothing in this viewer has a geometry lever.** No amount of shader work fixes
that; it wants culling, or level of detail, or fewer quads.

**And the slice costs as much as the clip.** The stencil parity pass measured 336.7 ms against the
opaque pass's 305.6 — it is a second full walk of the mesh with culling and depth writes off, pure
overdraw, and it runs whenever the slider is off its stop. That is the cap added earlier today, and
it roughly doubles a clip's cost while slicing. Worth knowing before anybody optimises a shader.

**Post nearly pays for itself:** the chain costs 222 ms of new work, but compositing means the
default framebuffer no longer needs MSAA, and dropping that took 220 ms off the opaque pass. Net
cost on the facility: **3%**.

One more thing it found in the code added today: **the slice cap's copy of ACES had no `clamp` on
the end** — a fourth opinion about what "bright" means, on the one surface that meets every other
surface along an edge. All four passes now end on one injected `ws_output`.

---

## 8. The engine bug is REAL, and it is on the scene the project is judged against

Confirmed against `forge::sample` itself — not against the export — with the right control. `place`
is a **cull and nothing else**, so clearing every `has_place` must not change which materials get
painted. It changes them:

| | as authored | place cleared (control) | place shifted by origin |
|---|---|---|---|
| `estate/colonnade` 4/m | 10 materials | 12 | 12 |
| — `moss` / `lichen` | **0 / 0** | 12,392 / 5,231 | identical to the control |
| `facility` `part_terrace` 8/m | 19 materials | 22 | 22 |
| — `moss` / `lichen` / `bleached` | **0 / 0 / 0** | 210 / 100 / 4,036 | identical to the control |

**Solid voxel counts are identical in all three arms**, so this is paint and not geometry, and
shifting `place` by `origin_shift` restores the control exactly. `apply_origin` translates a paint
rule's `test` and leaves its `place`; `plan_sample` does not shift it either. **Every placed
weathering coat in the facility and the estate is silently painting nothing in the game.** No error,
no warning — a rule that never fires produces no output at all.

Not fixed here: `src/forge` belongs to a second line of work. It is one shift in `plan_sample`, and
`--place-check` in `tools/paintcheck.cpp` is the regression test for it.

## 9. The paint stack cannot be evaluated per pixel, and the number is not close

`facility/part_terrace`, 348 rules: **2,018,075 node evaluations per stack walk.** After the
per-rule bounding-box reject, **338.6 of 348 rules survive** — because only 11 of 348 carry an
`on=` place, so the box has almost nothing to bite on — leaving 338,060 nodes.

That is ~200 s a frame on this machine, and **a GPU a thousand times faster is still 200 ms a frame
for one fragment.** Cross-checked at 0.24 s per surface point per walk in optimised C++ on four
cores.

**This is not a shader-care problem and no amount of tuning reaches it.** The fix is upstream: do
not bake rules whose test is the `occlusion` or `curvature` of a large solid. Until then the raw
view needs the distance fallback and the rule cap that are now in `paintcost.js`, and the honest
answer for a facility fragment is the flat grey with a line saying why.

## 10. The two views will never agree exactly, and that is geometry rather than a bug

Measured against the sampler's own decision, `sampler.clip` at 32/m:

| where the stack is asked | disagreement |
|---|---|
| at the voxel centre | 1.872 % |
| …in single precision | 1.872 % — **f32 costs one point in 132,055** |
| `glass_test`, all shape-keyed rules | **0.000 % of 151,218 surface voxels** |
| **at the marched hit point** | **34.737 %** |

The stack ports exactly. What differs is **where it is asked**: the analytic surface and the voxel
centre are different places, 0.37 voxels apart on average and up to 3.1, and **84 % of the
disagreements are the winning rule firing by more than 1e-2** — a genuinely different pattern
value, not a rounding. The worst single confusion is 25,445 voxels of `stone` reading as `metal`
from one brick-bond rule with a 2 cm mortar joint sampled at a 3.1 cm voxel.

**No port can remove this.** The raw view shows the colours the clip's rules give *at the true
surface*; the voxel view shows what those rules gave *at the voxel centres*. Where a pattern is
finer than a voxel the two differ by construction, and the raw one is arguably the more correct.
That has to be said on the page rather than presented as an exact preview.

The guard against the trap that would have hidden all of this: `sealed_dark.clip` has one material,
so every arm agrees trivially — the tool reports `THIS RUN PROVES LITTLE` and exits non-zero rather
than printing 0.000 % six times.
