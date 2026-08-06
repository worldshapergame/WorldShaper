#pragma once
// Ops — the only way world state is ever allowed to change.
//
// documentation/02-architecture-overview.md, "The central idea": every mutation is a
// small, deterministic, serialisable record carrying the tick it applies to. Nothing —
// not gameplay, not simulation, not a Lua mod — writes voxels directly.
//
// That discipline starts here, fifteen stages before networking exists, because it is
// what turns Stage 16 from a rewrite into an integration. A world is
//
//     generate(seed) + replay(orderedIntentOps)
//
// and that identity only holds if there is no second path into the data.

#include <vector>

#include "core/types.hpp"
#include "world/ledger.hpp"
#include "world/voxel_type.hpp"
#include "world/write_mask.hpp"

namespace ws {

class World;

enum class OpKind : u8 {
    SetVoxel = 0,   // one voxel
    FillBox,        // the chisel: an inclusive box between two corners
};

// Fixed layout for now. Stage 16 varint-encodes this for the wire, where a typical op is
// 16–48 bytes (documentation/06-multiplayer.md §1); in memory it stays a plain struct so
// the op log is a flat array.
struct Op {
    u64 tick = 0;
    u32 player = 0;
    OpKind kind = OpKind::SetVoxel;
    MatterReason reason = MatterReason::PlayerPlace;
    WriteMask mask = WriteMask::All;
    u8 reserved = 0;
    VoxelTypeId type = kAir;
    i64 x0 = 0, y0 = 0, z0 = 0;
    i64 x1 = 0, y1 = 0, z1 = 0;

    static Op set_voxel(u64 tick, u32 player, i64 x, i64 y, i64 z, VoxelTypeId type,
                        MatterReason reason);
    static Op fill_box(u64 tick, u32 player, i64 x0, i64 y0, i64 z0, i64 x1, i64 y1, i64 z1,
                       VoxelTypeId type, MatterReason reason,
                       WriteMask mask = WriteMask::All);

    // Corners in either order; normalised so two peers that describe the same box the
    // other way round produce identical results.
    void normalise();

    u64 volume() const;
    u64 content_hash() const;
    bool operator==(const Op&) const = default;
};

struct OpResult {
    u64 voxels_changed = 0;
    u64 voxels_visited = 0;
};

// Applies the op and records every change in the ledger. This is the only function in the
// engine that writes voxels.
OpResult apply_op(World& world, const Op& op, MatterLedger& ledger);

// Replays a sequence in order. The result of replaying the same log onto the same
// starting state is bit-identical on every machine — which is the entire multiplayer
// design in one sentence.
OpResult apply_ops(World& world, const std::vector<Op>& ops, MatterLedger& ledger);

// An append-only log with a rolling hash, so two peers can compare where they diverged
// without exchanging the ops themselves.
class OpLog {
public:
    void append(const Op& op);
    const std::vector<Op>& ops() const { return ops_; }
    usize size() const { return ops_.size(); }
    u64 rolling_hash() const { return hash_; }
    void clear();

private:
    std::vector<Op> ops_;
    u64 hash_ = 0x6A09E667ull;
};

}  // namespace ws
