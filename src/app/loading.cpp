#include "app/loading.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>

#include "core/time.hpp"

namespace ws {
namespace {

// What a stage costs when nothing is known about it yet.
//
// These are the facility's own shares, rounded, because it is the largest thing anybody builds
// and a bar that is right for it is close enough for everything smaller. They are only ever used
// on the very first run of a clip that has never been built; the second run uses what the first
// one measured.
//
// Sampling dominates by two orders of magnitude and the numbers say so. A bar that gave the seven
// stages a seventh each would spend six sevenths of its travel in the first few per cent of the
// work and then stop.
constexpr f64 kNominal[static_cast<usize>(LoadStage::Count)] = {
    0.002,   // Reading
    0.780,   // Sampling
    0.090,   // Varying
    0.040,   // Stamping
    0.030,   // Caching
    0.048,   // Uploading
    0.010,   // Settling
};

// And when the world comes back from the cache instead of being built, which is the ordinary
// case: there is no sampling at all, and reading the file off a disk is most of it.
constexpr f64 kFromCache[static_cast<usize>(LoadStage::Count)] = {
    0.020,   // Reading — the clip text, to work out whether the cache is still good
    0.000,
    0.000,
    0.640,   // Stamping — decoding a third of a gigabyte of bricks back into the world
    0.000,
    0.300,   // Uploading
    0.040,   // Settling
};

f64 from_bits(u64 bits) {
    f64 value = 0.0;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

u64 to_bits(f64 value) {
    u64 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

}  // namespace

const char* stage_name(LoadStage stage) {
    switch (stage) {
        case LoadStage::Reading:   return "reading the clip";
        case LoadStage::Sampling:  return "cutting the shape";
        case LoadStage::Varying:   return "colouring every voxel";
        case LoadStage::Stamping:  return "building the world";
        case LoadStage::Caching:   return "keeping it for next time";
        case LoadStage::Uploading: return "handing it to the card";
        case LoadStage::Settling:  return "settling";
        default:                   return "";
    }
}

LoadHistory LoadHistory::read(const std::string& path) {
    LoadHistory out;
    std::ifstream file(path, std::ios::binary);
    if (!file) return out;
    f64 values[static_cast<usize>(LoadStage::Count)]{};
    if (!file.read(reinterpret_cast<char*>(values), sizeof(values))) return out;
    f64 total = 0.0;
    for (f64 v : values) {
        if (!(v >= 0.0) || v > 3600.0) return out;   // nonsense, or from another machine entirely
        total += v;
    }
    if (total <= 0.0) return out;
    std::memcpy(out.seconds, values, sizeof(values));
    out.known = true;
    return out;
}

void LoadHistory::write(const std::string& path) const {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) return;
    file.write(reinterpret_cast<const char*>(seconds), sizeof(seconds));
}

bool LoadProgress::likely_cached(const LoadHistory& history) {
    if (!history.known) return false;
    f64 total = 0.0;
    for (usize i = 0; i < static_cast<usize>(LoadStage::Count); ++i) total += history.seconds[i];
    if (total <= 0.0) return false;
    return history.seconds[static_cast<usize>(LoadStage::Sampling)] < total * 0.05;
}

void LoadProgress::begin(const LoadHistory& history, bool from_cache) {
    const f64* fallback = from_cache ? kFromCache : kNominal;
    total_weight_ = 0.0;
    for (usize i = 0; i < static_cast<usize>(LoadStage::Count); ++i) {
        // A stage that took no time last run still gets its nominal floor, because "it was
        // instant last time" and "it will be instant this time" are different claims — the cache
        // may have been hot then and cold now.
        const f64 measured = history.known ? history.seconds[i] : 0.0;
        weights_[i] = history.known ? (measured + fallback[i] * 0.05) : fallback[i];
        total_weight_ += weights_[i];
        stage_began_ns_[i] = 0;
        stage_done_ns_[i].store(0, std::memory_order_relaxed);
    }
    if (total_weight_ <= 0.0) total_weight_ = 1.0;

    began_ns_ = now_ns();
    rate_old_ns_ = rate_new_ns_ = 0;
    rate_old_fraction_ = rate_new_fraction_ = 0.0;
    shown_left_ = -1.0;
    stage_.store(0, std::memory_order_relaxed);
    within_bits_.store(to_bits(0.0), std::memory_order_relaxed);
    done_.store(0, std::memory_order_relaxed);
    expected_.store(0, std::memory_order_relaxed);
    complete_.store(false, std::memory_order_relaxed);
    stage_began_ns_[0] = began_ns_;
}

void LoadProgress::enter(LoadStage stage) {
    const u32 index = static_cast<u32>(stage);
    const u32 was = stage_.load(std::memory_order_relaxed);
    const u64 at = now_ns();
    // Everything up to here is finished, whatever it last reported. A stage that never called
    // `within` still counts as done the moment the next one starts, which is what keeps the bar
    // from leaving gaps behind it.
    for (u32 i = was; i < index && i < static_cast<u32>(LoadStage::Count); ++i) {
        if (stage_done_ns_[i].load(std::memory_order_relaxed) == 0) {
            stage_done_ns_[i].store(at, std::memory_order_relaxed);
        }
    }
    if (index < static_cast<u32>(LoadStage::Count)) stage_began_ns_[index] = at;
    within_bits_.store(to_bits(0.0), std::memory_order_relaxed);
    stage_.store(index, std::memory_order_release);
}

void LoadProgress::within(f64 fraction) {
    within_bits_.store(to_bits((fraction < 0.0) ? 0.0 : ((fraction > 1.0) ? 1.0 : fraction)),
                       std::memory_order_relaxed);
}

void LoadProgress::count(u64 done, u64 expected) {
    done_.store(done, std::memory_order_relaxed);
    expected_.store(expected, std::memory_order_relaxed);
}

void LoadProgress::finish() {
    const u64 at = now_ns();
    for (u32 i = 0; i < static_cast<u32>(LoadStage::Count); ++i) {
        if (stage_done_ns_[i].load(std::memory_order_relaxed) == 0) {
            stage_done_ns_[i].store(at, std::memory_order_relaxed);
        }
    }
    within_bits_.store(to_bits(1.0), std::memory_order_relaxed);
    complete_.store(true, std::memory_order_release);
}

f64 LoadProgress::weight_before(LoadStage stage) const {
    f64 sum = 0.0;
    for (u32 i = 0; i < static_cast<u32>(stage); ++i) sum += weights_[i];
    return sum;
}

f64 LoadProgress::weight_of(LoadStage stage) const {
    return weights_[static_cast<usize>(stage)];
}

LoadProgress::Snapshot LoadProgress::look() const {
    Snapshot out;
    out.complete = complete_.load(std::memory_order_acquire);
    if (out.complete) {
        out.stage = LoadStage::Settling;
        out.fraction = 1.0;
        out.seconds_left = 0.0;
        return out;
    }

    const u32 index = stage_.load(std::memory_order_acquire);
    out.stage = static_cast<LoadStage>(index < static_cast<u32>(LoadStage::Count) ? index : 0);
    out.done = done_.load(std::memory_order_relaxed);
    out.expected = expected_.load(std::memory_order_relaxed);

    const f64 inside = from_bits(within_bits_.load(std::memory_order_relaxed));
    out.fraction = (weight_before(out.stage) + weight_of(out.stage) * inside) / total_weight_;
    if (out.fraction < 0.0) out.fraction = 0.0;
    if (out.fraction > 0.999) out.fraction = 0.999;   // a hundred means done, and it is not

    // How long is left, from how fast this run has been going LATELY.
    //
    // The obvious estimate is the average since launch: this machine has spent a known time
    // reaching a known fraction, so the rest of the weight takes the rest of the time at the same
    // rate. That is only a fair predictor if the work goes at a constant rate, and this work does
    // not go at a constant rate at all.
    //
    // Sampling is the clearest case. Empty space is settled a whole block at a time and matter is
    // not, so the sky goes past in moments and the building crawls. An average taken while the sky
    // was flying by says two minutes; a minute later it says six; a minute after that, eleven. A
    // countdown that counts up is worse than no countdown — it does not merely fail to inform, it
    // tells the player, once a second, that the game does not know what it is doing.
    //
    // So the rate is measured over a trailing window: a pair of readings, the older of which is
    // between one and two windows back, and the slope between them. When the cheap work runs out
    // the slope drops and the estimate rises ONCE and then tracks, instead of climbing forever.
    const u64 at = now_ns();

    constexpr f64 kWindow = 5.0;
    if (rate_new_ns_ == 0) {
        rate_old_ns_ = rate_new_ns_ = began_ns_;
        rate_old_fraction_ = rate_new_fraction_ = 0.0;
    }
    if (static_cast<f64>(at - rate_new_ns_) * 1e-9 >= kWindow) {
        rate_old_ns_ = rate_new_ns_;
        rate_old_fraction_ = rate_new_fraction_;
        rate_new_ns_ = at;
        rate_new_fraction_ = out.fraction;
    }

    const f64 over = static_cast<f64>(at - rate_old_ns_) * 1e-9;
    const f64 gained = out.fraction - rate_old_fraction_;

    // Held back until there is a window's worth of evidence and the bar has actually moved. The
    // rate over the first moments is mostly the cost of starting, and predicting from it gives a
    // wild number that then visibly settles, which reads as the estimate being untrustworthy. It
    // is better to say nothing for a few seconds than to say something silly.
    if (over >= kWindow && gained > 1e-4 && out.fraction > 0.01) {
        const f64 raw = (1.0 - out.fraction) * over / gained;
        // Eased towards, so the reading does not jitter by tens of seconds between frames as the
        // slope wobbles. Downward quickly and upward slowly: a countdown that drops is a pleasant
        // surprise and one that climbs is the complaint this is here to answer.
        if (shown_left_ < 0.0) {
            shown_left_ = raw;
        } else {
            const f64 ease = (raw < shown_left_) ? 0.25 : 0.05;
            shown_left_ += (raw - shown_left_) * ease;
        }
        // And never longer than a second ago, while nothing has gone wrong. Time passing is itself
        // progress, so the number comes down between windows rather than standing still.
        out.seconds_left = shown_left_;
    } else if (shown_left_ >= 0.0) {
        out.seconds_left = shown_left_;
    }
    return out;
}

LoadHistory LoadProgress::history() const {
    LoadHistory out;
    u64 previous = began_ns_;
    for (u32 i = 0; i < static_cast<u32>(LoadStage::Count); ++i) {
        const u64 done = stage_done_ns_[i].load(std::memory_order_relaxed);
        const u64 began = (stage_began_ns_[i] != 0) ? stage_began_ns_[i] : previous;
        out.seconds[i] = (done > began) ? static_cast<f64>(done - began) * 1e-9 : 0.0;
        previous = (done != 0) ? done : previous;
    }
    out.known = true;
    return out;
}

}  // namespace ws
