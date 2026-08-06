#pragma once
// Wall-clock timing.
//
// IMPORTANT (documentation/05-simulation.md §12 rule 4): nothing here may be used by the
// simulation. Simulation time is measured in ticks, which are identical on every machine.
// These functions exist for the frame loop, profiling and the debug HUD only.

#include "core/types.hpp"

namespace ws {

// Monotonic nanoseconds since process start.
u64 now_ns();

inline f64 ns_to_ms(u64 ns) { return static_cast<f64>(ns) * 1e-6; }

// Rolling frame-time statistics for the HUD. We report the 99th percentile, not the
// average — documentation/09-performance-budgets.md §9: a locked 30 beats an average 45
// with hitches.
class FrameStats {
public:
    static constexpr usize kWindow = 240;

    void push(f64 frame_ms);

    f64 last_ms() const { return last_ms_; }
    f64 average_ms() const;
    f64 percentile_ms(f64 fraction) const;
    f64 max_ms() const;
    u64 frame_index() const { return frame_index_; }

private:
    f64 samples_[kWindow]{};
    usize count_ = 0;
    usize cursor_ = 0;
    f64 last_ms_ = 0.0;
    u64 frame_index_ = 0;
};

// Simple scoped CPU timer that writes its result into a destination on scope exit.
class ScopedTimer {
public:
    explicit ScopedTimer(f64& destination_ms) : dest_(destination_ms), start_(now_ns()) {}
    ~ScopedTimer() { dest_ = ns_to_ms(now_ns() - start_); }
    ScopedTimer(const ScopedTimer&) = delete;
    ScopedTimer& operator=(const ScopedTimer&) = delete;

private:
    f64& dest_;
    u64 start_;
};

}  // namespace ws
