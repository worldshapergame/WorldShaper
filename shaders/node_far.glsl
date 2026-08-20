// R9h -- the fallback, for where there is genuinely nothing.
//
// Past the last node and the last region record, the answer the marcher leaves today is whatever
// happens to be in the record. This file is the other answer: **the analytic sky for the sky
// direction, and the coarsest folded colour available on the path for anything else.** Never black,
// never a stall, and never a request.
//
// Include AFTER node.glsl's `node_face_hit`, and call `node_far_stopped` where the march gives up
// on a cell the world claims and the pool has not built. Nothing here reads anything node.glsl does
// not already declare, and nothing here writes to a buffer.
//
// # The rule this sits next to, and why it is a rule rather than a preference
//
//   A light path may name the one cell that STOPPED it and the one face it LANDED on. Never what it
//   crossed, and never more than one entry per node per window.
//
// That is `21-renderer-rewrite.md`'s correction of the older absolute ("no light path may cause
// streaming"), and D589 is why it was corrected: the code deliberately breaks the absolute in two
// places -- R9a reports the face a gathering ray landed on, and R9i lets a shadow ray say it is
// using the cell that stopped it (D341-D343, D430) -- and both are bounded by `node_seen` (D431).
// Measured over a settled run, the pool built **1 node** with the light rules on against 4 with
// `--no-light-keeps-geometry`.
//
// **This file adds no third exception, and that is the property it is built around.** It:
//
//   - writes to no buffer at all. Every function here is a read of the node pool plus arithmetic;
//   - makes no feedback entry. `node_face_hit` is called with `report` FALSE, so the key is
//     recorded in the hit and nothing is sent;
//   - asks for no node. `node_locate` is a lookup. The R9i report for the cell that stopped the ray
//     has ALREADY gone out four lines above the call site and is untouched -- widening it or
//     narrowing it are both changes to a mechanism that is measured and correct;
//   - names, at most, the face of a node THE POOL ALREADY HOLDS. R9a's exception is about the face
//     a light ray landed on; a coarse node with a folded colour is a surface at that scale, the ray
//     did land in it, and its key is produced by `node_face_hit` -- the same arithmetic in the same
//     place as every other face key, so the pass that looks a face up and the pass that claims one
//     cannot disagree about which face it is (see the note over `node_face_hit`).
//
// The counted gate is R11e's line in `src/app/main.cpp` -- *"light paths: N requests offered to the
// sampler (A a stopped cell, R9i; B a landed face, R9a), M sample jobs caused"*. **M is the number
// the rule is judged on and it must stay at nought.** A and B are what makes a nought mean
// something. Nothing in this file can move M: it causes no offer, because it makes no report.
//
// # What the fallback actually is
//
// The march stops on a cell the world says has matter in it and the pool has not built. `NodeHit`
// comes back `hit = true, unknown = true`, with a colour of nought and -- this is the part that
// matters -- **no face key at all**: `node_face_hit` is called at the leaf hit and nowhere else, so
// `face_level` is `kNoFaceLevel` and there is nothing for a gathering ray to walk up from. That
// sentence is `shade_faces.comp`'s own, written where it returns nought for these:
//
//   *"Any rule that reads light where the pool has not built has to change the marcher first, and
//   that is a bigger change than this one."*
//
// This is that change to the marcher, and it is four lookups. The cell that stopped the ray has
// ancestors, and one of them is built: walk up from the level the descent stopped at until a node
// answers `node_folded_colour`, and report THAT node as what the ray landed on -- its folded
// colour, its level, and its face key. The ray did land in it. The pool holds it. The colour was
// folded from its own children by the same ladder that folds every other coarse node.
//
// # Why this is not the light floor two entries of the decision log exist to have removed
//
// D589 measured this third of R9h and declined to build it, for a reason that is still exactly
// right: *"Giving them a folded colour would need an irradiance to multiply it by, and there isn't
// one -- inventing it is D541-D543's deleted light floor arriving through a third door."*
//
// **The answer is that nothing here invents one.** A folded colour is an ALBEDO, and this file
// hands back an albedo and a face key and stops. What multiplies it is `bounce_radiance`, which
// reads the light some face actually MEASURED -- and refuses to answer at all when no face has:
// `bounce_face_ready` requires `kFaceSettled` samples before a record may be read, and the search
// ends at `vec3(0.0)`. So in a sealed dark room, where every correct answer is nought and D302
// measured 93,741 of 93,745 faces fully shadowed, the coarse faces this walk reaches have measured
// nought and hand back nought. There is no term in this file that is not either a read of the pool
// or a multiplication by something a ray measured. `tools\darkroom.ps1` is the gate, and the reason
// it stays black is structural rather than lucky.
//
// # Why `unknown` is cleared, and why that is the whole point rather than a side effect
//
// `unknown` means *"I have no answer"*. Once this walk has one, the ray does. Clearing it is what
// turns three separate stalls into answers, and each of them is a clause of R9h:
//
//   - **never black.** `sky_cast` returns nought for an ignorance stop; with an answer it calls
//     `bounce_radiance`, which reads the coarse face's measured light or nought;
//   - **never a stall.** `measured_ignorance` holds a face short of convergence for as long as any
//     of its rays stops on ignorance. A face at the edge of the built world never finishes and
//     re-casts for ever -- that is the red in `--debug-mode 19`;
//   - **never a request.** Unchanged. The R9i report above the call site is the one entry this ray
//     is allowed, it is already made, and this file adds nothing to it.
//
// And it errs in the direction this renderer is required to err in, on every path that reads it:
//
//   - a SHADOW ray still comes back `hit = true`, so the sun is still blocked. What changes is only
//     that the face believes its own shadowed answer instead of resting on it -- and believing
//     "shadowed" errs dark. D713's asymmetry is intact and is the reason this direction is safe:
//     *"Only the shadowed extreme rests -- a lit face that stopped is the sealed room filling with
//     sunlight."* Nothing here can make a stopped ray read as lit;
//   - a REFLECTION segment drops its sample on ignorance and takes it here, out of a coarse face;
//   - a PRIMARY ray never reaches this at all. `occlude_unknown` is false at every call site in
//     `visibility.comp` and in `reflect.glsl`, so the only callers are the face pass's light rays
//     and `beam.comp`'s bound -- and the bound reads `t` and `hit`, neither of which moves.
//
// # Why the walk is bounded, and bounded by the RAY rather than by a constant
//
// `kNodeFarSteps` levels up from where the marcher stopped, which is R9f's `kFaceAncestorStep` and
// the same argument: the level the march stopped at is the one this ray's own footprint chose, so
// "three levels up" is three levels up from whatever detail this ray was entitled to. A surface an
// arm's length away is read at 25 cm and one across the valley at whatever its footprint already
// was. Unbounded, this would walk to `kEntryLevel` and read a 512 m block's average for a ray that
// wanted a brick, which is D151's blob in your face arriving through the light instead of through
// the picture.
//
// The walk takes the FINEST ancestor that answers rather than the coarsest one available, which
// reads against the plan's own wording and is the same choice `bounce_radiance` already makes three
// lines of its own comment further down: *"the probes stop at the first ancestor with an answer,
// and indoors that is nearly always the first."* Coarser is never more information.
//
// # What it costs
//
// At most `kNodeFarSteps + 1` descents, on rays that had already given up, and only on the rays
// that stopped on ignorance at all -- which D589 measured at **3 gathering rays of 482,773** on the
// close camera under continuous editing, the state that produces the most unbuilt geometry this
// engine can be put in. It is not free where the world is genuinely behind, which is exactly the
// state it exists for: at the edge of a loaded region, or the frame after a teleport.

#ifndef WS_NODE_FAR_GLSL
#define WS_NODE_FAR_GLSL

// ---- the arm ------------------------------------------------------------------------------------
//
// The compile-time half, defaulting to the NEW behaviour. `WS_NODE_FAR_FALLBACK 0` builds the call
// site out entirely, which is the arm to reach for when the question is what the fallback COSTS.
//
// It is not the control arm for what the fallback DOES, and that distinction is D731 and trap 32:
// an A/B tells you what the FLAG does and nothing at all about what the diff around it does. The
// control that decides anything is this file absent from the tree -- `git stash`, rebuild, the same
// command -- because both arms of a switch inside the change are still inside the change.
#ifndef WS_NODE_FAR_FALLBACK
#define WS_NODE_FAR_FALLBACK 1
#endif

// ...and the runtime half, so both arms are one binary (D407).
//
// The bit is SET to turn the fallback OFF, which is the way round that makes a host that has never
// heard of this flag do the new thing rather than the old one -- the dial word is zeroed before
// anything writes it, so an unset bit has to mean the default. `--no-far-fallback` sets it; see the
// hunk for `src/app/main.cpp` in this stage's report.
//
// 1 << 15 is the first free bit in the dial word. Bits 0-13 are `kProbeOn` through
// `kProbeSunConfidence` in node.glsl.
//
// **Bit 14 is taken TWICE and that is a fault, found while checking this one.**
// `kProbeRefractStack` (node.glsl, and `src/gpu/render_params.hpp` line 444) and
// `kProbeCoverageBins` (`shaders/face_worklist.comp`, and render_params.hpp line 592) are the same
// bit, and `main.cpp` ORs both into this word -- so `--no-refract-stack` silently clears the
// coverage-bins dial and `--coverage-bins` silently switches the refraction stack on. Two control
// arms that are meant to be independent are one bit, so an A/B of either measures both. It is
// D713's *"two agents named one field, and the size assert was satisfied"* one door along, with no
// assert at all this time: the two constants sit 150 lines apart in one header and each is mirrored
// into a shader that cannot see the other. Not fixed here -- neither file is this stage's.
// **BIT 18, NOT 15.** This file was written against a header in which 15 looked free; it was not
// free by the time the wave was integrated, and 15 was not free when this was written either -- the
// dial word had `kProbeRefractStack` and `kProbeCoverageBins` BOTH at bit 14 (D736), so every bit
// above the collision read as available when the next one along had in fact been claimed. The
// collision is fixed and CoverageBins holds 15; 16 is unused, 17 is `--no-through-fallback`.
// Must match `kProbeFarFallbackOff` in src/gpu/render_params.hpp.
const uint kProbeFarFallbackOff = 1u << 18;

// How many levels the walk may climb above where the march stopped. R9f's `kFaceAncestorStep`, by
// name rather than by value, because the two are the same rule read from two ends: this one finds
// the coarse face and that one reads it, and a walk that climbed further than the reader would
// follow it would answer out of a record nothing else in the renderer agrees is the right one.
const int kNodeFarSteps = kFaceAncestorStep;

bool node_far_enabled() {
#if WS_NODE_FAR_FALLBACK
    return (light_probe.words[0] & kProbeFarFallbackOff) == 0u;
#else
    return false;
#endif
}

// ---- the fallback -------------------------------------------------------------------------------

// The answer for a ray the march gave up on: the coarsest folded colour available on the path.
//
// `voxel` is the absolute voxel coordinate of the cell that stopped the ray -- the same `voxel` the
// R9i report four lines above the call site is built from, so the cell this answers about and the
// cell the pool is being told to build cannot drift apart. `normal` is the face it came in through.
//
// `result.level` on the way in is where the descent stopped, and on the way out is the level the
// answer is at. Returns true when it found one; leaves `result` exactly as it was when it did not,
// which is the arm every build before this one ran and is what "ignorance errs dark" already means.
bool node_far_stopped(inout NodeHit result, ivec3 voxel, ivec3 normal) {
    if (!node_far_enabled()) return false;

    // A ray that has not taken a step has no face to name. `last_normal` is nought until the first
    // DDA step or the first analytic skip, and a zero normal through `node_face_hit`'s arithmetic
    // comes out as the -z face by default -- which is a key for a face the ray never touched, and
    // "I could not find out" must never become an answer (trap 7). There is no such thing as a
    // cheap wrong face key here: the store would be asked about a surface on the far side of the
    // cell and would answer, confidently, about somewhere else.
    if (normal == ivec3(0)) return false;

    const int from = clamp(result.level, kLeafLevel, kEntryLevel);

    for (int up = 0; up <= kNodeFarSteps; ++up) {
        const int want = from + up;
        if (want > kEntryLevel) break;

        // A lookup, not a request. `node_locate` walks the tree the pool already holds and returns
        // what is there; the invocation's descent cache is block-checked on every entry, so a walk
        // that ends above where the ray was is the same node a walk from the root would have found
        // and leaves the cache exact for whatever ray this invocation marches next.
        const Found found = node_locate(voxel, want);

        // Three refusals, and every one of them is `node_folded_colour`'s rather than this file's:
        // a descent that did not reach the level asked for is answering about a differently sized
        // cell, a shell has never been folded from anything so its colour is nought, and a node the
        // fold gave up on carries no coverage. The immediate parent of an ignorance stop is very
        // often the shell itself and is refused here for exactly that reason -- painting a building
        // black out of a shell is the fault the stand-in branch reintroduced once and was reported
        // from a screenshot: the facility in silhouette, every column solid black.
        uint colour = 0u;
        if (!node_folded_colour(found, want, colour)) continue;

        // Answered. What the ray landed on is this node, at this level, through this face.
        result.unknown = false;
        result.colour = colour;
        result.level = want;

        // `report` FALSE. The key is recorded in the hit for whoever reads it and NOTHING is sent:
        // the feedback buffer is where a request lives, and R9h's third clause is that this path
        // does not make one. Whether the face is worth CLAIMING is R9a's decision, taken by the
        // caller against R9a's own throttle, and it is not this file's to take.
        node_face_hit(result, false, voxel, want, normal);
        return true;
    }

    return false;
}

#endif  // WS_NODE_FAR_GLSL
