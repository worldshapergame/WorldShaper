#pragma once
// Which timestamp slot each profiled pass writes to, and nothing else.
//
// # Why this is in core rather than beside the profiler
//
// It is pure bookkeeping — a nesting-aware slot allocator with a name, a budget and a byte
// counter per slot — and it has no idea what a GPU is. Keeping it free of Vulkan is what lets it
// be tested, and it needs testing more than most things here, because the fault it replaces was
// invisible from outside.
//
// The profiler kept one open index and did not claim a slot until the pass closed. When the
// cloud dispatch was moved *inside* the path tracer's pass, the inner open took the outer one's
// slot and overwrote its name, the inner close cleared the open index, and the outer close found
// nothing open and wrote no end timestamp at all. Everything after the inner pass — the entire
// tracer — fell outside every timestamp in the frame. The pass list still looked plausible, the
// totals still added up, and one pass had simply stopped existing: 0.253 ms of measured GPU work
// against 37.7 ms of wall clock, for months.
//
// So the rule this class exists to enforce is: **a pass claims its slot when it opens**, and
// every open is matched by exactly one close.

#include <array>
#include <string>

#include "core/types.hpp"

namespace ws {

class PassLedger {
public:
    static constexpr u32 kMaxPasses = 32;

    void reset();

    // The slot to write a begin timestamp at, or -1 when there is no room for another pass or
    // the nesting is deeper than the stack holds. A refused open still occupies a level of the
    // stack, so the matching close() pops the level this call pushed and not an enclosing pass's.
    i32 open(const char* name, f64 budget_ms);

    // The slot to write an end timestamp at, or -1 when nothing is open or the matching open()
    // was refused a slot.
    i32 close();

    // Bytes moved, charged to the innermost open pass. Systems report their own traffic; there
    // is no portable driver counter for it.
    void add_bytes(u64 bytes);

    u32 count() const { return count_; }
    u32 depth() const { return depth_; }

    // Every pass that was opened has been closed. Checked at the end of a frame: the old
    // bookkeeping violated this while still looking balanced from outside, because the opens and
    // the closes were counted by different mechanisms.
    bool balanced() const { return depth_ == 0 && opens_ == closes_; }

    const std::string& name(u32 slot) const { return names_[slot]; }
    f64 budget(u32 slot) const { return budgets_[slot]; }
    u32 pass_depth(u32 slot) const { return depths_[slot]; }
    u64 bytes(u32 slot) const { return bytes_[slot]; }

private:
    std::array<std::string, kMaxPasses> names_;
    std::array<f64, kMaxPasses> budgets_{};
    std::array<u64, kMaxPasses> bytes_{};
    std::array<u32, kMaxPasses> depths_{};
    std::array<i32, kMaxPasses> stack_{};
    u32 count_ = 0;
    u32 depth_ = 0;
    u32 opens_ = 0;
    u32 closes_ = 0;
};

}  // namespace ws
