#pragma once
// Undo and redo — unlimited, per player.
//
// Every edit is stored as the ops it was made of plus the ops that put things back. Both
// halves are ordinary ops, so undo needs no privileged path into the world: it replays
// through apply_op like everything else, appends to the op log like everything else, and
// will travel over the network in Stage 16 like everything else.
//
// Unlimited depth is affordable because of world/region.cpp: what is remembered is the
// *description* of the prior state, not a copy of it. Carving a tunnel through solid rock
// costs one op to undo no matter how long the tunnel is. Painting every voxel of a wall a
// different colour costs one op per voxel, which is the honest price of having destroyed
// that much information.
//
// Per player, because Stage 16 has several: undoing your own last edit must not undo
// someone else's. Two players editing the same voxels can still produce an undo that no
// longer describes what is there — that is a conflict question, and it belongs with the
// rest of reconciliation rather than here.

#include <unordered_map>
#include <vector>

#include "core/types.hpp"
#include "world/op.hpp"

namespace ws {

class World;

struct EditRecord {
    std::vector<Op> forward;   // what the player asked for
    std::vector<Op> inverse;   // what restores the world to before it
    u64 voxels_changed = 0;

    u64 bytes() const {
        return static_cast<u64>((forward.capacity() + inverse.capacity()) * sizeof(Op));
    }
};

class EditHistory {
public:
    // Applies the op, capturing what it overwrote first. This is the entry point for
    // anything a player does; nothing else should call apply_op directly.
    OpResult apply(World& world, MatterLedger& ledger, OpLog& log, const Op& op);

    // Several ops as one undoable action — a stamped clip, or a brush stroke.
    OpResult apply_group(World& world, MatterLedger& ledger, OpLog& log,
                         const std::vector<Op>& ops);

    bool undo(World& world, MatterLedger& ledger, OpLog& log, u64 tick, u32 player);
    bool redo(World& world, MatterLedger& ledger, OpLog& log, u64 tick, u32 player);

    bool can_undo(u32 player) const { return undo_depth(player) > 0; }
    bool can_redo(u32 player) const { return redo_depth(player) > 0; }
    usize undo_depth(u32 player) const;
    usize redo_depth(u32 player) const;

    // What the whole history costs, for the HUD. Unlimited depth is a promise about
    // behaviour, not a claim that it is free, so the number is kept visible.
    u64 bytes() const { return bytes_; }
    u64 record_count() const { return records_; }

    // The last edit, split into the two halves that can each be slow for different reasons:
    // capture walks the prior contents, apply writes the new ones. Without the split, a
    // slow edit gives no clue which one to look at.
    f64 last_capture_ms() const { return last_capture_ms_; }
    f64 last_apply_ms() const { return last_apply_ms_; }
    u64 last_inverse_ops() const { return last_inverse_ops_; }

    void clear();

private:
    struct Stack {
        std::vector<EditRecord> past;
        std::vector<EditRecord> future;
    };

    Stack& stack_for(u32 player) { return players_[player]; }
    void account(const EditRecord& record, i64 sign);
    void drop_future(Stack& stack);

    std::unordered_map<u32, Stack> players_;
    u64 bytes_ = 0;
    u64 records_ = 0;
    f64 last_capture_ms_ = 0.0;
    f64 last_apply_ms_ = 0.0;
    u64 last_inverse_ops_ = 0;
};

}  // namespace ws
