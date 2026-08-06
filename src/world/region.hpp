#pragma once
// Turning a region of the world back into ops.
//
// This is what makes undo affordable. The obvious way to remember what an edit destroyed
// is to snapshot the voxels, which costs four bytes each — carving a five metre cube would
// cost 16 MB, and the user has asked for unlimited undo, so that number would be paid over
// and over until memory ran out.
//
// Instead the prior contents are re-expressed as the ops that would recreate them. A
// chisel through uniform rock costs *one* op to undo however large it is, because the thing
// being remembered is "this box was rock", not four million copies of the word rock. Detail
// costs more, which is right: the cost tracks how much information was actually destroyed.
//
// It also keeps the invariant from documentation/02: apply_op stays the only writer, and
// undo is not a special case in the engine — it is ops, so it serialises, replays and
// travels over the network like anything else.

#include <vector>

#include "core/types.hpp"
#include "world/op.hpp"

namespace ws {

class World;

// Appends ops to `out` that, applied in order, restore the box to exactly its current
// contents. Returns how many were appended.
//
// The reason on each op is derived from what it does: restoring matter is a place,
// restoring emptiness is a break, so the ledger's per-player accounting stays honest
// through an undo.
u64 decompose_region(const World& world, i64 x0, i64 y0, i64 z0, i64 x1, i64 y1, i64 z1,
                     u64 tick, u32 player, std::vector<Op>& out);

}  // namespace ws
