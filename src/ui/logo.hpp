#pragma once
// When the mark changes, and what it changes out of.
//
// `shaders/logo.glsl` decides what the mark LOOKS like: it takes a seed and draws animations,
// patterns, palettes, the arrangement its blocks fly into and the sorting algorithm that re-orders
// its slices, two or more of each at a time. This decides *when a new seed happens*, which is a
// question about presses and about nobody being there — so it belongs on the host, where it can be
// tested without a card.
//
// # Why the seed lives here and not in the shader
//
// The mark is the one surface in this interface allowed a hue of its own, because it is a picture
// rather than furniture (documentation/14-ui-style.md, the five permitted colours). A picture that
// is identical on every launch is furniture again. So it is drawn from a seed, and the seed changes
//
//   - when somebody presses it, which is what makes pressing it worth doing twice, and
//   - on its own after `kLogoIdleSeconds` with nobody touching anything, so a title left up is
//     never the same picture for long.
//
// # Why two seeds and not one
//
// A change that cuts is a change that reads as a glitch. So the previous seed is kept and both
// combinations are drawn at once for `kLogoMorphSeconds` — the outgoing one dissolving into voxels
// as the arriving one gathers out of them. `morph` is how far through that is, and it is 1 almost
// always, which is what lets the shader skip the second evaluation entirely.
//
// # Why it is pinnable
//
// `--logo-seed N` fixes it, because `--title-shot` is how anybody ever looks at this without a hand
// on the keyboard (D460) and a photograph of a thing that is different every time is a photograph
// that can never be compared with the last one. A pinned seed never changes, on a press or
// otherwise, and its morph is over before the first frame.

#include <array>

#include "core/types.hpp"

namespace ws::ui {

// How many of each the card has. These MIRROR `shaders/logo.glsl`, and `tests/test_logo.cpp` reads
// that file and checks the mirror — because a count that disagrees is a combination nobody can reach
// or a branch nobody wrote, and neither says anything when it happens.
inline constexpr u32 kLogoAnimations = 24;    // how the mark moves
inline constexpr u32 kLogoPatterns = 20;      // what colours it
inline constexpr u32 kLogoPalettes = 12;      // which colours those are
inline constexpr u32 kLogoArrangements = 12;  // where its voxels go when they leave their places
inline constexpr u32 kLogoOrders = 10;        // the sorting algorithm that re-orders its slices
inline constexpr u32 kLogoKeys = 6;           // and what that algorithm is sorting them by
inline constexpr u32 kLogoAxes = 2;           // across the mark, or up it
inline constexpr u32 kLogoTransitions = 24;   // how one combination becomes the next

// What a seed chooses, counted. It picks two animations and two patterns — in order, because the
// first is the one an arrival plays and the second is blended into it — one palette, one
// arrangement, one sorting algorithm, one key for it and one axis for it to work along.
//
// Four billion is not the interesting number and neither was 720 (this was three choices once). The
// interesting one is that a seed has no final state at all: the sort's cycle number is part of its
// own hash, so each cycle shuffles the slices differently from the last and the clock is not bounded.
inline constexpr u64 kLogoCombinations = static_cast<u64>(kLogoAnimations) * kLogoAnimations *
                                        kLogoPatterns * kLogoPatterns * kLogoPalettes *
                                        kLogoArrangements * kLogoOrders * kLogoKeys * kLogoAxes;

// How much room the mark is drawn into, as a multiple of the square the DRAWING occupies.
//
// The mark has no bounding box: blocks fly into a ring wider than the drawing, voxels scatter
// outward as a combination leaves, and the rain animation lifts the whole thing most of its own
// height. All of that used to be cut off at the edge of the cell — which is the one thing that makes
// a mark read as a picture in a frame rather than as a thing in the room.
//
// It is a multiple rather than "the whole screen" because it is paid for in pixels evaluated: the
// mark is a fifth of the short side, so at four times across it is sixteen times the area. That was
// the reason to keep it small when an arrangement was a loop over 144 blocks; the arrangements are
// invertible maps now and cost a handful of multiplies, so the room is worth more than the pixels.
// The drawing itself does not change size — this is only the room it is allowed to move in.
//
// The card mirrors this number and `tests/test_logo.cpp` checks the two agree, because a host that
// bins one rectangle while the shader draws another is a mark clipped to a box that appears nowhere
// in either of them.
inline constexpr f32 kLogoReach = 4.0f;

// Long enough to be a change you watch rather than a frame you miss, short enough that a player
// pressing it repeatedly is not waiting for the last press to finish.
inline constexpr f64 kLogoMorphSeconds = 1.6;
// And how long the title sits untouched before the mark changes on its own.
inline constexpr f64 kLogoIdleSeconds = 24.0;

// --- the sort, which is the host's because it cannot be anything else ---------------------------
//
// The drawing is cut into **one slice per voxel row or column** — 160 of them, which is the drawing's
// own resolution and not a coarser grid laid over it — and the slices are put in whatever order a
// sorting algorithm has reached this instant.
//
// # Why this is on the host and not in the shader
//
// It was in the shader first, over sixteen slices, and sixteen is where that approach ends. A pixel
// cannot ask what any other pixel decided, so a shader can only sort an array it can hold in
// registers and re-sort from scratch for every pixel — which bounds the slice count to about sixteen
// and bounds the algorithms to the ones whose comparisons are local, because the state of an
// insertion sort at step k depends on the whole array. Here it is 160 slices, ten real algorithms,
// exactly stepped, computed **once a frame** instead of once a pixel, and — the part that matters
// most for this project — **testable**: `tests/test_logo.cpp` runs every one of them to completion
// and checks it sorted, checks a partial run is a prefix of a full one, and checks that what comes
// out is always a permutation. None of that can be asserted about a shader.
//
// The card reads the finished permutation out of the same buffer the text lives in, one word a slice.
inline constexpr u32 kLogoLanes = 160;

// How the mark's own timing reaches the card, per seed: three words of header and then the slices.
//
//   0   the lane weight — how far the slices are from where they belong — as float bits
//   1   the arrangement weight, the same way
//   2   1 if the slices run across the mark, 0 if they run up it
//   3.. `kLogoLanes` words: which slice of the drawing stands at each position
//
// The two weights are here rather than in the shader because *when* a thing happens is exactly the
// kind of decision this project keeps on the host and covers with tests, and because the sort's own
// clock and the weight that reveals it have to be the same clock or the mark comes apart while the
// algorithm is not running.
// (`kLogoWords` is taken on the card by the packed drawing, so this is kLogoFrameWords
// there and here.)
inline constexpr u32 kLogoFrameWords = 3 + kLogoLanes;

// How long a sort takes to run, whatever algorithm it is. Selection sort exchanges 159 times and an
// odd-even network exchanges six thousand, so pacing them by exchanges a second would make one of
// them a flicker and the other a wait — the exchange count is normalised out and each seed scales
// this by 0.6 to 1.8. What differs between the algorithms is the SHAPE of the run, which is the only
// reason to have ten of them.
inline constexpr f64 kLogoSortSeconds = 2.6;
// And how long the mark then stands whole before the next cycle shuffles it again.
inline constexpr f64 kLogoHoldSeconds = 13.0;

// What one frame of the mark needs, all of it decided here.
struct LogoFrame {
    f32 lane_weight = 0.0f;       // 0 is every slice where the drawing put it
    f32 arrange_weight = 0.0f;    // 0 is the drawing in its own square
    bool along_x = true;          // whether the slices run across the mark or up it
    u32 algorithm = 0;            // which of `kLogoOrders`, for the audit line and the tests
    u32 exchanges = 0;            // how many it has applied so far
    u32 total = 0;                // and how many the whole run takes
    // Whether the keys are now in the order this algorithm sorts them. The one claim about a sort
    // that is worth making and cannot be made about a shader, so it is made here and checked in
    // `tests/test_logo.cpp` at the end of every run of every algorithm at four hundred seeds.
    bool in_order = false;
    std::array<u32, kLogoLanes> at{};   // at[position] = which slice of the drawing is there
};

// The mark's state at a moment, for one seed. Deterministic in its two arguments and free of any
// state of its own, so a test can ask for any instant in any order.
LogoFrame logo_frame(u32 seed, f64 seconds);

// The next seed after `from`. Deterministic in its arguments, so a scripted run that pins the clock
// draws the same picture twice, and never equal to `from` — a press that chose the same seed again
// would read as a press that did nothing.
//
// It does not check that the CHOICES differ, only that the seeds do. Two distinct seeds land on the
// same combination about once in every 2.8 million, and the only way to rule it out would be a copy
// of the card's hash on the host — a second copy of a drawing rule, which is the thing this
// interface goes out of its way not to have.
u32 logo_seed_next(u32 from, f64 now, u64 counter);

class LogoSeed {
public:
    // The combination being drawn, the one before it, and how far between them. `previous` equals
    // `seed` until something has actually changed, which is how the shader knows to skip the morph.
    u32 seed() const { return seed_; }
    u32 previous() const { return previous_; }

    // Seconds since this seed was chosen. An animation with a beginning starts here.
    f32 age(f64 now) const;
    // 0 on the frame of a change, 1 when the morph out of the seed before it is over. Eased here
    // rather than on the card, because it is one number a frame and the card would do it a million
    // times.
    f32 morph(f64 now) const;

    // `--logo-seed N`. Nothing changes it after this.
    void pin(u32 seed);
    bool pinned() const { return pinned_; }

    // Once a frame, before the mark is drawn. `touched` is whether the player did anything at all —
    // not whether they clicked anything, because what the idle clock is asking is whether somebody
    // is there.
    void tick(f64 now, bool touched);

    // The mark was pressed. Ignored on a pinned seed.
    void change(f64 now);

    // `--logo-change N`: change even while pinned, and stay pinned. This exists so the morph itself
    // is photographable — it is a shape nothing could otherwise take a picture of, and
    // documentation/14-ui-style.md's rule about the tool previews is exactly about that: a shape
    // nobody can photograph is a shape nobody notices has stopped being drawn. Both seeds are
    // derived from the pinned one, so the whole transition is the same twice.
    void force_change(f64 now);

    // How many times it has changed, which is the only thing a test needs to see the idle clock
    // fire and the only thing `morph` needs to know there is nothing behind the first one.
    u64 changes() const { return changes_; }

private:
    u32 seed_ = 0;
    u32 previous_ = 0;
    f64 chosen_ = 0.0;
    f64 touched_ = 0.0;
    u64 changes_ = 0;
    bool pinned_ = false;
    bool started_ = false;
};

}  // namespace ws::ui
