#include "world/history.hpp"

#include <algorithm>

#include "core/time.hpp"
#include "world/region.hpp"
#include "world/world.hpp"

namespace ws {
namespace {

// The bounding box of a group of ops, which is the region whose prior state has to be
// captured. One box for the whole group rather than one per op: overlapping ops in the same
// action would otherwise capture each other's output as if it were the original state.
void bounds_of(const std::vector<Op>& ops, i64 lo[3], i64 hi[3]) {
    bool first = true;
    for (const Op& raw : ops) {
        Op op = raw;
        op.normalise();
        if (first) {
            lo[0] = op.x0; lo[1] = op.y0; lo[2] = op.z0;
            hi[0] = op.x1; hi[1] = op.y1; hi[2] = op.z1;
            first = false;
            continue;
        }
        lo[0] = std::min(lo[0], op.x0);
        lo[1] = std::min(lo[1], op.y0);
        lo[2] = std::min(lo[2], op.z0);
        hi[0] = std::max(hi[0], op.x1);
        hi[1] = std::max(hi[1], op.y1);
        hi[2] = std::max(hi[2], op.z1);
    }
}

}  // namespace

OpResult EditHistory::apply(World& world, MatterLedger& ledger, OpLog& log, const Op& op) {
    return apply_group(world, ledger, log, std::vector<Op>{op});
}

OpResult EditHistory::apply_group(World& world, MatterLedger& ledger, OpLog& log,
                                  const std::vector<Op>& ops) {
    OpResult result;
    if (ops.empty()) return result;

    i64 lo[3]{}, hi[3]{};
    bounds_of(ops, lo, hi);

    EditRecord record;
    record.forward = ops;
    const u64 capture_start = now_ns();
    decompose_region(world, lo[0], lo[1], lo[2], hi[0], hi[1], hi[2], ops.front().tick,
                     ops.front().player, record.inverse);
    last_capture_ms_ = ns_to_ms(now_ns() - capture_start);
    last_inverse_ops_ = record.inverse.size();

    const u64 apply_start = now_ns();
    for (const Op& op : ops) {
        const OpResult one = apply_op(world, op, ledger);
        result.voxels_changed += one.voxels_changed;
        result.voxels_visited += one.voxels_visited;
        log.append(op);
    }
    last_apply_ms_ = ns_to_ms(now_ns() - apply_start);
    record.voxels_changed = result.voxels_changed;

    // An edit that changed nothing — placing stone where stone already is — is not a step
    // the player should have to undo past.
    if (result.voxels_changed == 0) return result;

    Stack& stack = stack_for(ops.front().player);
    drop_future(stack);
    account(record, +1);
    stack.past.push_back(std::move(record));
    ++records_;
    return result;
}

bool EditHistory::undo(World& world, MatterLedger& ledger, OpLog& log, u64 tick, u32 player,
                       std::vector<Op>& applied) {
    applied.clear();
    Stack& stack = stack_for(player);
    if (stack.past.empty()) return false;

    EditRecord record = std::move(stack.past.back());
    stack.past.pop_back();

    for (Op op : record.inverse) {
        op.tick = tick;
        apply_op(world, op, ledger);
        log.append(op);
        // Reported, because an undo is an edit and everything downstream of an edit has to be
        // told which region moved. See the note on the declaration.
        applied.push_back(op);
    }

    stack.future.push_back(std::move(record));
    return true;
}

bool EditHistory::redo(World& world, MatterLedger& ledger, OpLog& log, u64 tick, u32 player,
                       std::vector<Op>& applied) {
    applied.clear();
    Stack& stack = stack_for(player);
    if (stack.future.empty()) return false;

    EditRecord record = std::move(stack.future.back());
    stack.future.pop_back();

    for (Op op : record.forward) {
        op.tick = tick;
        apply_op(world, op, ledger);
        log.append(op);
        applied.push_back(op);
    }

    stack.past.push_back(std::move(record));
    return true;
}

usize EditHistory::undo_depth(u32 player) const {
    const auto found = players_.find(player);
    return (found == players_.end()) ? 0 : found->second.past.size();
}

usize EditHistory::redo_depth(u32 player) const {
    const auto found = players_.find(player);
    return (found == players_.end()) ? 0 : found->second.future.size();
}

void EditHistory::account(const EditRecord& record, i64 sign) {
    const u64 size = record.bytes();
    if (sign > 0) {
        bytes_ += size;
    } else {
        bytes_ = (bytes_ >= size) ? bytes_ - size : 0;
    }
}

void EditHistory::drop_future(Stack& stack) {
    // A new edit makes the redo branch unreachable. Nothing in the design forbids keeping
    // it as a tree, but branching undo is a user-facing feature nobody has asked for, and
    // silently holding memory for states that can never be returned to is worse than not.
    for (const EditRecord& record : stack.future) {
        account(record, -1);
        if (records_ > 0) --records_;
    }
    stack.future.clear();
}

void EditHistory::clear() {
    players_.clear();
    bytes_ = 0;
    records_ = 0;
}

}  // namespace ws
