#include "ui/logo.hpp"

#include <algorithm>
#include <cmath>

#include "core/hash.hpp"
#include "ui/logo_marks.hpp"

namespace ws::ui {
namespace {

// The same 32-bit mix the card uses for its own choices, so a seed's picks here and its picks there
// come out of one family of numbers rather than two.
u32 logo_hash(u32 x) {
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x;
}

u32 logo_pick(u32 seed, u32 salt, u32 bound) {
    return logo_hash(seed ^ (salt * 0x9E3779B9u)) % std::max(bound, 1u);
}

f32 logo_unit(u32 seed, u32 salt) {
    return static_cast<f32>(logo_hash(seed ^ (salt * 0x85EBCA6Bu)) & 0xFFFFFFu) /
           static_cast<f32>(0xFFFFFF);
}

f32 logo_range(u32 seed, u32 salt, f32 low, f32 high) {
    return low + (high - low) * logo_unit(seed, salt);
}

// What the slices are being sorted by. Six keys, and every one is a property of the SLICE — which is
// what lets the sort happen once rather than once a pixel.
f32 lane_key(u32 which, u32 seed, u32 cycle, u32 lane, bool along_x) {
    const f32 u = (static_cast<f32>(lane) + 0.5f) / static_cast<f32>(kLogoLanes);
    switch (which) {
        case 0: return u;   // its own place, so sorting puts the drawing back exactly as it was
        case 1: return logo_unit(seed ^ (cycle * 0x2545F491u), 900u + lane);
        case 2:
            // How much mark the slice holds, out of the generated table (ui/logo_marks.hpp). The
            // heaviest slice of this drawing is the trunk, so sorting on it stacks the tree's matter
            // to one side and its air to the other.
            return static_cast<f32>(along_x ? kLogoColumnMark[lane] : kLogoRowMark[lane]) /
                   static_cast<f32>(kLogoLanes);
        case 3: return std::abs(u - 0.5f);   // out from the middle
        case 4: return logo_unit(seed ^ (cycle * 0x9E3779B9u), 400u + (lane / 8u));   // in bands
        default: return 1.0f - u;            // to a mirror of itself
    }
}

// Where the slices start each cycle. Multiply-and-offset modulo the lane count is a bijection when
// the multiplier shares no factor with it — 160 is 2^5 x 5, so an odd multiplier that is not a
// multiple of five is coprime with it. Every slice has to land somewhere and no two may land in the
// same place, or the drawing loses a stripe and gains a copy of another one.
u32 lane_start(u32 seed, u32 cycle, u32 lane) {
    u32 step = (logo_hash(seed ^ (cycle * 0x2545F491u)) % 32u) * 2u + 1u;   // odd
    if (step % 5u == 0u) step += 2u;                                        // and not a fifth
    const u32 offset = logo_hash(seed ^ (cycle * 0x9E3779B9u) ^ 0x5BF03635u) % kLogoLanes;
    return (lane * step + offset) % kLogoLanes;
}

// One exchange, metered. Everything below counts its swaps through here, which is what makes
// `budget` mean the same thing to all ten of them.
struct Run {
    std::array<f32, kLogoLanes>& key;
    std::array<u32, kLogoLanes>& at;
    u32 budget = 0;
    u32 spent = 0;

    bool out() const { return spent >= budget; }
    void swap(u32 a, u32 b) {
        std::swap(key[a], key[b]);
        std::swap(at[a], at[b]);
        ++spent;
    }
};

void order_transposition(Run& run, bool up) {
    for (u32 pass = 0; pass < kLogoLanes && !run.out(); ++pass) {
        bool moved = false;
        for (u32 i = pass & 1u; i + 1 < kLogoLanes && !run.out(); i += 2) {
            const bool wrong = up ? (run.key[i] > run.key[i + 1]) : (run.key[i] < run.key[i + 1]);
            if (wrong) {
                run.swap(i, i + 1);
                moved = true;
            }
        }
        // A pass over both parities that moves nothing is a sorted array, and the pass counter alone
        // would keep walking it. Two clean passes in a row are needed, hence the parity check.
        if (!moved && (pass & 1u) == 1u) return;
    }
}

void order_insertion(Run& run) {
    for (u32 i = 1; i < kLogoLanes && !run.out(); ++i) {
        for (u32 j = i; j > 0 && run.key[j - 1] > run.key[j] && !run.out(); --j) run.swap(j - 1, j);
    }
}

void order_selection(Run& run) {
    for (u32 i = 0; i + 1 < kLogoLanes && !run.out(); ++i) {
        u32 best = i;
        for (u32 j = i + 1; j < kLogoLanes; ++j) {
            if (run.key[j] < run.key[best]) best = j;
        }
        if (best != i) run.swap(i, best);
    }
}

void order_shell(Run& run) {
    // Ciura's gaps, extended down. The long gaps move a slice a long way in one exchange, which is
    // the thing that makes a shell sort look like nothing else here.
    static constexpr u32 kGaps[] = {57u, 23u, 10u, 4u, 1u};
    for (u32 gap : kGaps) {
        for (u32 i = gap; i < kLogoLanes && !run.out(); ++i) {
            for (u32 j = i; j >= gap && run.key[j - gap] > run.key[j] && !run.out(); j -= gap) {
                run.swap(j - gap, j);
            }
        }
        if (run.out()) return;
    }
}

void order_comb(Run& run) {
    u32 gap = kLogoLanes;
    bool moved = true;
    while ((gap > 1 || moved) && !run.out()) {
        gap = std::max(1u, (gap * 10u) / 13u);
        moved = false;
        for (u32 i = 0; i + gap < kLogoLanes && !run.out(); ++i) {
            if (run.key[i] > run.key[i + gap]) {
                run.swap(i, i + gap);
                moved = true;
            }
        }
    }
}

void order_cocktail(Run& run) {
    u32 low = 0;
    u32 high = kLogoLanes - 1;
    while (low < high && !run.out()) {
        for (u32 i = low; i < high && !run.out(); ++i) {
            if (run.key[i] > run.key[i + 1]) run.swap(i, i + 1);
        }
        --high;
        for (u32 i = high; i > low && !run.out(); --i) {
            if (run.key[i - 1] > run.key[i]) run.swap(i - 1, i);
        }
        ++low;
    }
}

void order_gnome(Run& run) {
    u32 at = 1;
    while (at < kLogoLanes && !run.out()) {
        if (run.key[at - 1] <= run.key[at]) {
            ++at;
            continue;
        }
        run.swap(at - 1, at);
        at = (at > 1) ? at - 1 : 1;
    }
}

void order_heap(Run& run) {
    const auto sift = [&](u32 root, u32 end) {
        while (!run.out()) {
            u32 child = root * 2 + 1;
            if (child >= end) return;
            if (child + 1 < end && run.key[child] < run.key[child + 1]) ++child;
            if (run.key[root] >= run.key[child]) return;
            run.swap(root, child);
            root = child;
        }
    };
    for (u32 i = kLogoLanes / 2; i > 0 && !run.out(); --i) sift(i - 1, kLogoLanes);
    for (u32 end = kLogoLanes; end > 1 && !run.out(); --end) {
        run.swap(0, end - 1);
        sift(0, end - 1);
    }
}

// The algorithm, to a budget of exchanges. Returns how many it actually applied, which is how the
// caller learns the length of a full run without being told.
u32 run_order(u32 algorithm, std::array<f32, kLogoLanes>& key, std::array<u32, kLogoLanes>& at,
              u32 budget) {
    Run run{key, at, budget, 0};
    switch (algorithm) {
        case 0: break;   // no algorithm at all: the shuffle, held for the whole cycle
        case 1: order_transposition(run, true); break;
        case 2: order_transposition(run, false); break;
        case 3: order_insertion(run); break;
        case 4: order_selection(run); break;
        case 5: order_shell(run); break;
        case 6: order_comb(run); break;
        case 7: order_cocktail(run); break;
        case 8: order_gnome(run); break;
        default: order_heap(run); break;
    }
    return run.spent;
}

// A weight that holds at nought for most of its cycle and then goes out and comes back.
//
// The hold is a legibility rule rather than a taste: a mark that is never whole is a mark nobody
// recognises, and the first thing this interface owes a player is a title they know they are looking
// at. It is also what makes the two expensive paths on the card free most of the time, because both
// are skipped while their weight is nought.
f32 sweep(f64 through, f32 swing) {
    const f64 wave = 0.5 - 0.5 * std::cos(through * 6.283185307179586);
    const f64 eased = std::clamp((wave - 0.80) / 0.20, 0.0, 1.0);
    return static_cast<f32>(swing * eased * eased * (3.0 - 2.0 * eased));
}

}  // namespace

LogoFrame logo_frame(u32 seed, f64 seconds) {
    LogoFrame frame;
    const f64 t = std::max(0.0, seconds);
    frame.algorithm = logo_pick(seed, 71u, kLogoOrders);
    const u32 key_which = logo_pick(seed, 72u, kLogoKeys);
    frame.along_x = logo_pick(seed, 73u, kLogoAxes) == 0u;

    // Every algorithm takes the same time to run, scaled by the seed. See kLogoSortSeconds.
    const f64 sorting = kLogoSortSeconds * static_cast<f64>(logo_range(seed, 74u, 0.6f, 1.8f));
    const f64 cycle_seconds = sorting + kLogoHoldSeconds;
    // Where in its own cycle this seed starts, so two marks pressed a second apart are not in step.
    const f64 own = t + static_cast<f64>(logo_unit(seed, 89u)) * cycle_seconds;
    const u32 cycle = static_cast<u32>(own / cycle_seconds);
    const f64 into = own - static_cast<f64>(cycle) * cycle_seconds;

    std::array<f32, kLogoLanes> key{};
    for (u32 lane = 0; lane < kLogoLanes; ++lane) {
        frame.at[lane] = lane_start(seed, cycle, lane);
        key[lane] = lane_key(key_which, seed, cycle, frame.at[lane], frame.along_x);
    }

    // How long the run is, MEASURED by running it, because the answer depends on the shuffle as well
    // as on the algorithm and a guess would pace nine of the ten wrongly.
    {
        std::array<f32, kLogoLanes> probe_key = key;
        std::array<u32, kLogoLanes> probe_at = frame.at;
        frame.total = run_order(frame.algorithm, probe_key, probe_at, 0xFFFFFFFFu);
    }
    const f64 progress = (sorting > 0.0) ? std::clamp(into / sorting, 0.0, 1.0) : 1.0;
    const u32 budget = static_cast<u32>(progress * static_cast<f64>(frame.total) + 0.5);
    frame.exchanges = run_order(frame.algorithm, key, frame.at, budget);

    // Did it finish the job? Only algorithm 2 sorts the other way; the one that is not an algorithm
    // at all sorts nothing and says so by leaving this false with a total of nought.
    frame.in_order = frame.algorithm != 0;
    for (u32 lane = 0; lane + 1 < kLogoLanes && frame.in_order; ++lane) {
        const bool wrong = (frame.algorithm == 2) ? (key[lane] < key[lane + 1])
                                                  : (key[lane] > key[lane + 1]);
        if (wrong) frame.in_order = false;
    }

    // The slices spread out, the sort runs where it can be seen, and they come back as it finishes.
    // Weight nought is the identity map, so the drawing is exactly itself for the whole hold
    // whatever order the sort left the slices in.
    if (into < sorting) {
        const auto ease = [](f64 low, f64 high, f64 x) {
            const f64 s = std::clamp((x - low) / (high - low), 0.0, 1.0);
            return s * s * (3.0 - 2.0 * s);
        };
        const f64 s = into / sorting;
        const f64 shape = std::min(ease(0.0, 0.16, s), 1.0 - ease(0.80, 1.0, s));
        frame.lane_weight = static_cast<f32>(logo_range(seed, 75u, 0.45f, 1.0f) * shape);
    }

    // And the arrangement, on a cycle of its own, set half a turn away from the sort's so that both
    // run at once, always, but seldom both at their fullest — a mark being sorted AND folded into a
    // tesseract at the same moment is a mark you cannot see either thing in.
    const f64 arrange_rate = static_cast<f64>(logo_range(seed, 83u, 0.012f, 0.035f));
    frame.arrange_weight = sweep(t * arrange_rate + 0.5 + static_cast<f64>(logo_unit(seed, 84u)),
                                 logo_range(seed, 82u, 0.45f, 1.0f));
    return frame;
}

u32 logo_seed_next(u32 from, f64 now, u64 counter) {
    // The clock in milliseconds, so two launches a fraction of a second apart do not draw the same
    // mark, and the counter, so two changes inside one millisecond do not either. Nothing here is
    // an RNG with state: the same arguments give the same seed, which is what makes a scripted run
    // repeatable and a test possible.
    const u64 millis = static_cast<u64>(std::max(0.0, now) * 1000.0);
    u64 h = hash_combine(hash_combine(0x51ED270B'9E37D31Dull, static_cast<u64>(from)), millis);
    h = hash_combine(h, counter + 1);
    u32 seed = static_cast<u32>(h ^ (h >> 32));
    // A press that lands on the seed it already had is a press that did nothing, and the one thing
    // this has to do is answer.
    while (seed == from) {
        h = hash_combine(h, 0x9E3779B9ull);
        seed = static_cast<u32>(h ^ (h >> 32));
    }
    return seed;
}

f32 LogoSeed::age(f64 now) const { return static_cast<f32>(std::max(0.0, now - chosen_)); }

f32 LogoSeed::morph(f64 now) const {
    // The first seed has nothing behind it, so it is not morphing out of anything — it is arriving,
    // which is its animation's own beginning at age nought. Reporting 0 here instead would scatter
    // the mark into voxels on the first frame of the title and gather it out of a picture that was
    // never there.
    if (changes_ == 0) return 1.0f;
    const f64 through = std::clamp((now - chosen_) / kLogoMorphSeconds, 0.0, 1.0);
    return static_cast<f32>(through * through * (3.0 - 2.0 * through));
}

void LogoSeed::pin(u32 seed) {
    seed_ = seed;
    previous_ = seed;
    pinned_ = true;
}

void LogoSeed::tick(f64 now, bool touched) {
    if (!started_) {
        started_ = true;
        chosen_ = now;
        touched_ = now;
        if (!pinned_) seed_ = logo_seed_next(0, now, changes_);
        previous_ = seed_;
        return;
    }
    if (touched) touched_ = now;
    if (pinned_) return;
    // Both clocks, and they are different questions: `touched_` is whether anybody is here, and
    // `chosen_` is whether the picture they are not looking at is old. Without the second, walking
    // away right after a press would change the mark again a moment later.
    if (now - touched_ >= kLogoIdleSeconds && now - chosen_ >= kLogoIdleSeconds) change(now);
}

void LogoSeed::change(f64 now) {
    if (pinned_) return;
    force_change(now);
}

void LogoSeed::force_change(f64 now) {
    previous_ = seed_;
    seed_ = logo_seed_next(seed_, now, ++changes_);
    chosen_ = now;
}

}  // namespace ws::ui
