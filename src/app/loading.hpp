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
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "core/types.hpp"

namespace ws {

// ---- where the way in is ------------------------------------------------------------------
//
// The loading screen's `play now` button, in WINDOW pixels. It is here, in a header that includes
// nothing but the type names, for two reasons.
//
// The first is that three pieces of code need this rectangle and they must never disagree: the
// shader draws it, `Application::draw_loading` asks whether the pointer is inside it, and a test
// checks it is somewhere a person can reach. A button that lights up a few pixels from where it
// can be clicked is a fault nobody reports clearly and everybody notices.
//
// The second is that a rectangle needs no device. Put in `gpu/loading_screen.hpp` it would drag
// Vulkan into any test that wanted to look at it, which in practice means no test would.
//
// `shaders/loading.comp` carries the same three constants and says so; they are the layout, in
// interface pixels from the centre of the column, and the bar's own width is written there the
// same way.
constexpr f32 kLoadingButtonHalfWidth = 46.0f;
constexpr f32 kLoadingButtonTop = 54.0f;      // clear of the bar at 0 and the last text row at 32
constexpr f32 kLoadingButtonBottom = 76.0f;

struct LoadingButtonRect {
    f32 x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
    bool contains(f32 x, f32 y) const { return x >= x0 && x < x1 && y >= y0 && y < y1; }
    f32 width() const { return x1 - x0; }
    f32 height() const { return y1 - y0; }
};

// Interface pixels are sized from the window's SHORT side, so the column keeps its proportions on
// a wide monitor instead of growing until it runs off the sides.
inline f32 loading_ui_scale(u32 width, u32 height) {
    const f32 across = static_cast<f32>((width < height) ? width : height);
    const f32 steps = (across < 420.0f) ? 1.0f : static_cast<f32>(static_cast<u32>(across / 420.0f));
    return (steps < 1.0f) ? 1.0f : steps;
}

inline LoadingButtonRect loading_button_rect(u32 width, u32 height) {
    const f32 scale = loading_ui_scale(width, height);
    const f32 cx = static_cast<f32>(width) * 0.5f;
    const f32 cy = static_cast<f32>(height) * 0.5f;
    LoadingButtonRect out;
    out.x0 = cx - kLoadingButtonHalfWidth * scale;
    out.x1 = cx + kLoadingButtonHalfWidth * scale;
    out.y0 = cy + kLoadingButtonTop * scale;
    out.y1 = cy + kLoadingButtonBottom * scale;
    return out;
}

// A finer record than the seven stages below, and the reason it exists is a sixteen-second hole.
//
// `load stages:` reports the seven, and six of them are honest because each is one thing. The
// seventh, `settling`, is everything between the world being built and the first frame being
// drawable: the pool, nine device buffers, two descriptor layouts, seven compute pipelines, the
// render target and the HUD. On a cold driver cache that bucket has been measured at **16,395 ms
// of a 16,545 ms load** while the four timings printed beside it added to under one second — so
// the line named the stage that was slow and said nothing whatever about what in it was slow.
//
// So every step of the load says what it cost, by name, in the order it happened. It is a
// measurement instrument that ships: a player's "it takes ages to load" arrives with the log, and
// the log has to be able to answer it without a special build.
//
// Written from the build thread and the drawing thread both, so it takes a lock. It is entered a
// couple of dozen times in a whole load, which is a cost nothing can measure.
class LoadSteps {
public:
    void begin();
    void add(const char* name, f64 seconds);
    // One line, longest first would hide the order, so: the order they happened in.
    std::string line() const;
    f64 total() const;

private:
    mutable std::mutex mutex_;
    std::vector<std::pair<const char*, f64>> steps_;
};

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
// Two shapes, kept apart, because a load is one of two entirely different things.
//
// A cold build spends nearly all of itself sampling. A cache hit does no sampling whatsoever and
// spends itself decoding bricks. Recording one set of times for both means every cache hit teaches
// the next cold build that sampling is free — and a stage weighted at nothing cannot move the bar
// however long it runs. That is not a hypothetical: it pinned the bar at eight per cent for the
// whole of a hundred-and-forty-second build, because the run before it had come from the cache.
struct LoadHistory {
    static constexpr usize kBuilt = 0;
    static constexpr usize kCached = 1;
    static constexpr usize kShapes = 2;

    f64 seconds[kShapes][static_cast<usize>(LoadStage::Count)]{};
    bool known[kShapes]{};

    static LoadHistory read(const std::string& path);
    void write(const std::string& path) const;

    // Which shape a set of times describes, judged by whether any sampling happened. Written down
    // rather than passed in, so a run always files itself under what it actually did.
    static usize shape_of(const f64* seconds);
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

    // --- leaving the loading screen before it has finished ---------------------------------
    //
    // Asked for directly: *"you can click a button to enter the world now which will keep what is
    // already loaded but will put you on the world"*. Two atomics and no lock, for the same reason
    // everything else here is: one thread writes each one and the other reads it, and a reader a
    // frame stale is a button that appears a frame late.
    //
    // `offer` is the build saying there is now a way out that costs the player nothing they cannot
    // get back — the ladder rebuilds whatever is not there, so the only thing an early entry can
    // lose is time. It is deliberately NOT offered over work whose result cannot be rebuilt: a
    // world coming back off the disk carries somebody's carvings, and half of it is not a
    // half-loaded world, it is a lost one.
    //
    // `ask` is the button. The build polls it at its own boundaries and stops at the next one,
    // keeping every voxel it has already put in the world — nothing is dropped and nothing is
    // re-derived, which is the whole promise of the button.
    void offer_early_entry();
    bool offered_early_entry() const { return offer_.load(std::memory_order_acquire); }
    void ask_to_enter();
    bool asked_to_enter() const { return asked_.load(std::memory_order_acquire); }

    // Every stage after this one is skipped, and the bar says so rather than pretending the
    // remaining weight was completed. Called by whoever stops.
    void note_entered_early() { entered_.store(true, std::memory_order_release); }
    bool entered_early() const { return entered_.load(std::memory_order_acquire); }

    // --- what the drawing reads -----------------------------------------------------------
    struct Snapshot {
        LoadStage stage = LoadStage::Reading;
        f64 fraction = 0.0;      // of the whole load, 0 to 1
        f64 seconds_left = -1.0; // negative when it is too early to say
        u64 done = 0;
        u64 expected = 0;
        bool complete = false;
        // Whether the screen should be offering a way in, and whether somebody has taken it.
        bool may_enter = false;
        bool entering = false;
    };
    Snapshot look() const;

    // What actually happened, filed under the shape this run turned out to be and keeping what is
    // already known about the other shape — so a cache hit never erases what a cold build
    // measured, which is precisely the record the next cold build needs.
    // `this_shape`, when given, comes back holding which of the two shapes THIS RUN turned out to
    // be — and it exists because reading the wrong one printed a previous run's numbers as though
    // they were this one's.
    //
    // The returned history is the merge: this run filed under its own shape, keeping whatever was
    // already known about the other, because a cache hit must not erase what the last cold build
    // measured. Anything reporting on THIS run therefore has to be told which half it is, and the
    // caller that guessed instead — with `likely_cached`, whose whole job is guessing before a run
    // rather than reading after one — printed `cutting the shape 9727ms` identically to the
    // millisecond on two consecutive runs, four lines under `everything ready [t+337 ms]`. It was
    // very nearly reported to the user as a ten-second launch stage. D682.
    LoadHistory history(const LoadHistory& previous_runs, usize* this_shape = nullptr) const;

private:
    f64 weight_before(LoadStage stage) const;
    f64 weight_of(LoadStage stage) const;

    std::atomic<u32> stage_{0};
    std::atomic<u64> within_bits_{0};   // an f64, bit-cast, because there is no atomic<f64>
    std::atomic<u64> done_{0};
    std::atomic<u64> expected_{0};
    std::atomic<bool> complete_{false};
    std::atomic<bool> offer_{false};
    std::atomic<bool> asked_{false};
    std::atomic<bool> entered_{false};

    // How fast the load has been going LATELY, kept as a trailing pair of readings.
    //
    // Touched only by `look`, which only the drawing thread calls, so these are plain values with
    // no atomics: the build thread never sees them.
    mutable u32 seen_stage_ = 0xFFFFFFFFu;   // which stage the drawing last saw, and since when
    mutable u64 seen_stage_ns_ = 0;

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
