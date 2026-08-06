#pragma once
// The matter ledger.
//
// documentation/05-simulation.md §3: conservation of matter is an invariant, not a goal.
// The transfer protocol makes it structural inside the simulation; this ledger is the
// independent audit that proves it, and the place where deliberate sources and sinks —
// creative placement, a drill, a printer (answer I6) — are accounted for rather than
// silently leaking.
//
// It also carries the per-player accounting survival will need (answer O6, decision D53).
// Building the hooks now costs almost nothing; retrofitting them after a hundred systems
// mutate the world would cost a great deal.

#include <unordered_map>
#include <vector>

#include "core/types.hpp"
#include "world/voxel_type.hpp"

namespace ws {

class World;

// Why a change happened. The distinction is what separates "matter moved" from "matter
// appeared", and only the second kind is allowed to change a total.
enum class MatterReason : u8 {
    Simulation,   // a transfer: something left one place and arrived in another
    PlayerPlace,  // creative placement — a source
    PlayerBreak,  // removal — a sink
    Generation,   // terrain being created for the first time
    Machine,      // a drill or printer (answer I6)
    Load,         // deserialising an existing world; not a real change
};

struct PlayerMatter {
    i64 placed = 0;
    i64 removed = 0;
};

class MatterLedger {
public:
    // Records one voxel changing from `from` to `to`. Air on either side is a source or a
    // sink; anything else is a substitution.
    void record(VoxelTypeId from, VoxelTypeId to, MatterReason reason, u32 player = 0);

    // The same change, `count` times over. A chisel that fills a brick of uniform rock with
    // air changes 512 voxels identically, and charging the hash map 512 times for that is
    // the difference between a large edit landing in a frame and stalling for a second.
    void record_bulk(VoxelTypeId from, VoxelTypeId to, u64 count, MatterReason reason,
                     u32 player = 0);

    i64 total(VoxelTypeId type) const;
    const std::unordered_map<VoxelTypeId, i64>& totals() const { return totals_; }

    const PlayerMatter& player(u32 id) const;
    u64 change_count() const { return changes_; }

    void clear();

    // Counts what is actually in the world, from scratch. The audit compares this against
    // the running totals — if they ever disagree, something mutated the world without
    // going through an Op, which is the bug this whole design exists to prevent.
    static std::unordered_map<VoxelTypeId, i64> recount(const World& world);

    // True when the running totals exactly match a fresh recount.
    bool audit(const World& world) const;

    // Every type whose running total disagrees with the recount, for the error message.
    std::vector<VoxelTypeId> audit_failures(const World& world) const;

private:
    std::unordered_map<VoxelTypeId, i64> totals_;
    std::unordered_map<u32, PlayerMatter> players_;
    PlayerMatter no_player_;
    u64 changes_ = 0;
};

}  // namespace ws
