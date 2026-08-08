#pragma once
// What the game is doing before it can be played, and how far through it is.
//
// # Why this is not a spinner
//
// A spinner says "something is happening". A bar says "this much is done and this long is left",
// and the moment it says that it can be wrong, which is why most of them are. The two ways they
// go wrong are worth naming because avoiding both is the whole design here:
//
//   It fills quickly and then sits at ninety-nine per cent. That happens when the bar measures
//   the work somebody thought of and not the work that remains — the sampling is counted and the
//   upload to the card is not, so the last step is invisible and infinitely long.
//
//   It fills evenly and finishes early or late. That happens when the stages are weighted by
//   guess. Sampling a building is a hundred times the work of pasting it; a bar that gives them
//   a half each crawls and then jumps.
//
// So: every stage that stands between launching and playing is listed here, including the ones
// on the GPU, and each carries a WEIGHT — its share of the total, measured from the last run of
// this same clip rather than assumed. The bar cannot reach a hundred until the frame that can
// actually be shown is ready, because the last stage in the list is exactly that.
//
// # Why it costs nothing
//
// The build runs on its own thread and the main thread draws. Everything below is written by the
// build and read by the drawing, so it is all atomic and all relaxed: no lock, no barrier, and a
// reader that sees a value one frame stale is showing a bar one frame stale, which nobody can
// see. The build's own hot loops touch this once per region, never per voxel.

#include <atomic>
#include <string>
#include <vector>

#include "core/types.hpp"

namespace ws {

// The stages, in the order they happen. The order is the bar's order, so adding one here adds it
// to the bar and to the estimate without anything else changing.
enum class LoadStage : u32 {
    Reading,      // the clip file and everything it includes
    Sampling,     // asking the field where the matter is — nearly always the long one
    Varying,      // giving every voxel its own version of its material
    Stamping,     // writing it into the world's bricks
    Caching,      // writing the built world beside the clip, so this never happens again
    Uploading,    // handing the bricks to the card
    Settling,     // the summary tree and the first frame that can actually be shown
    Count,
};

const char* stage_name(LoadStage stage);

// How long each stage took last time, so the next run can weight the bar by what things actually
// cost rather than by what somebody guessed they would.
//
// Kept beside the clip's cache and keyed the same way. A first run with no history falls back to
// nominal weights that are roughly right for a large clip, and corrects itself as it goes.
struct LoadHistory {
    f64 seconds[static_cast<usize>(LoadStage::Count)]{};
    bool known = false;

    static LoadHistory read(const std::string& path);
    void write(const std::string& path) const;
};

// The live state of a load, written by the build thread and read by the drawing.
class LoadProgress {
public:
    // `from_cache` picks which set of nominal weights to fall back on. It has to be decided here,
    // before any work starts, and the honest answer is that nobody knows yet — the cache cannot be
    // asked until the clip has been read and keyed. Guessing from the last run is what `likely`
    // below is for; changing the weights afterwards is not an option, because they are read by the
    // drawing thread while the build thread runs and there is no lock between them.
    void begin(const LoadHistory& history, bool from_cache);

    // Whether the last run was a cache hit, judged by how little of it was spent sampling. A
    // first run has no history and guesses cold, which is the safe way round: a bar weighted for a
    // long build that turns out to be short finishes early, and a bar weighted for a short one
    // that turns out to be long sticks near the end and lies about the time.
    static bool likely_cached(const LoadHistory& history);

    // Entering a stage. Everything before it is now complete, whatever it claimed.
    void enter(LoadStage stage);

    // How far through the current stage, 0 to 1. Called once per region of work, not per voxel —
    // a store on a shared line from eight threads at voxel rate would be its own performance
    // problem.
    void within(f64 fraction);

    // A number the player can watch move: voxels so far, and how many are expected. Expected is
    // allowed to be wrong; it is only used to fill in a stage's own progress when nothing better
    // is known.
    void count(u64 done, u64 expected);

    void finish();

    // --- what the drawing reads -----------------------------------------------------------
    struct Snapshot {
        LoadStage stage = LoadStage::Reading;
        f64 fraction = 0.0;      // of the whole load, 0 to 1
        f64 seconds_left = -1.0; // negative when it is too early to say
        u64 done = 0;
        u64 expected = 0;
        bool complete = false;
    };
    Snapshot look() const;

    // What actually happened, for the next run to weight itself by.
    LoadHistory history() const;

private:
    f64 weight_before(LoadStage stage) const;
    f64 weight_of(LoadStage stage) const;

    std::atomic<u32> stage_{0};
    std::atomic<u64> within_bits_{0};   // an f64, bit-cast, because there is no atomic<f64>
    std::atomic<u64> done_{0};
    std::atomic<u64> expected_{0};
    std::atomic<bool> complete_{false};

    // How fast the load has been going LATELY, kept as a trailing pair of readings.
    //
    // Touched only by `look`, which only the drawing thread calls, so these are plain values with
    // no atomics: the build thread never sees them.
    mutable u64 rate_old_ns_ = 0;
    mutable f64 rate_old_fraction_ = 0.0;
    mutable u64 rate_new_ns_ = 0;
    mutable f64 rate_new_fraction_ = 0.0;
    mutable f64 shown_left_ = -1.0;

    f64 weights_[static_cast<usize>(LoadStage::Count)]{};
    f64 total_weight_ = 1.0;
    u64 began_ns_ = 0;
    u64 stage_began_ns_[static_cast<usize>(LoadStage::Count)]{};
    std::atomic<u64> stage_done_ns_[static_cast<usize>(LoadStage::Count)]{};
};

}  // namespace ws
