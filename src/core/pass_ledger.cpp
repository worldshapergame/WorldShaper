#include "core/pass_ledger.hpp"

namespace ws {

void PassLedger::reset() {
    count_ = 0;
    depth_ = 0;
    opens_ = 0;
    closes_ = 0;
    bytes_.fill(0);
}

i32 PassLedger::open(const char* name, f64 budget_ms) {
    ++opens_;
    // Out of slots, or nested past what the stack holds. A level is still pushed, so the
    // matching close() pops this call's level rather than an enclosing pass's.
    if (count_ >= kMaxPasses || depth_ >= kMaxPasses) {
        if (depth_ < kMaxPasses) stack_[depth_++] = -1;
        return -1;
    }

    // The slot is claimed HERE, not in close(). That is the whole of it: a pass opened before an
    // enclosing one closes must not be handed the enclosing one's slot.
    const u32 slot = count_++;
    names_[slot] = name;
    budgets_[slot] = budget_ms;
    depths_[slot] = depth_;
    stack_[depth_++] = static_cast<i32>(slot);
    return static_cast<i32>(slot);
}

i32 PassLedger::close() {
    if (depth_ == 0) return -1;
    ++closes_;
    return stack_[--depth_];
}

void PassLedger::add_bytes(u64 bytes) {
    if (depth_ == 0) return;
    const i32 slot = stack_[depth_ - 1];   // the innermost open pass owns the traffic
    if (slot < 0) return;
    bytes_[static_cast<usize>(slot)] += bytes;
}

}  // namespace ws
