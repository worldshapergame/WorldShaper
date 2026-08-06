#include "world/ledger.hpp"

#include <algorithm>

#include "world/world.hpp"

namespace ws {

void MatterLedger::record(VoxelTypeId from, VoxelTypeId to, MatterReason reason,
                          u32 player) {
    if (from == to) return;
    ++changes_;

    if (from != kAir) {
        i64& value = totals_[from];
        --value;
        if (value == 0) totals_.erase(from);
    }
    if (to != kAir) {
        ++totals_[to];
    }

    if (reason == MatterReason::PlayerPlace || reason == MatterReason::PlayerBreak) {
        PlayerMatter& account = players_[player];
        if (to != kAir) ++account.placed;
        if (from != kAir) ++account.removed;
    }
}

void MatterLedger::record_bulk(VoxelTypeId from, VoxelTypeId to, u64 count,
                               MatterReason reason, u32 player) {
    if (from == to || count == 0) return;
    const i64 n = static_cast<i64>(count);
    changes_ += count;

    if (from != kAir) {
        i64& value = totals_[from];
        value -= n;
        if (value == 0) totals_.erase(from);
    }
    if (to != kAir) {
        totals_[to] += n;
    }

    if (reason == MatterReason::PlayerPlace || reason == MatterReason::PlayerBreak) {
        PlayerMatter& account = players_[player];
        if (to != kAir) account.placed += n;
        if (from != kAir) account.removed += n;
    }
}

i64 MatterLedger::total(VoxelTypeId type) const {
    const auto found = totals_.find(type);
    return (found == totals_.end()) ? 0 : found->second;
}

const PlayerMatter& MatterLedger::player(u32 id) const {
    const auto found = players_.find(id);
    return (found == players_.end()) ? no_player_ : found->second;
}

void MatterLedger::clear() {
    totals_.clear();
    players_.clear();
    changes_ = 0;
}

std::unordered_map<VoxelTypeId, i64> MatterLedger::recount(const World& world) {
    std::unordered_map<VoxelTypeId, i64> counts;
    world.for_each_chunk([&counts](const ChunkCoord&, const Chunk& chunk) {
        for (u32 bz = 0; bz < static_cast<u32>(kChunkBricks); ++bz) {
            for (u32 by = 0; by < static_cast<u32>(kChunkBricks); ++by) {
                for (u32 bx = 0; bx < static_cast<u32>(kChunkBricks); ++bx) {
                    const Brick* brick = chunk.brick(bx, by, bz);
                    if (brick == nullptr || brick->empty()) continue;
                    for (u32 voxel = 0; voxel < static_cast<u32>(kBrickVoxels); ++voxel) {
                        const VoxelTypeId type = brick->get(voxel);
                        if (type != kAir) ++counts[type];
                    }
                }
            }
        }
    });
    return counts;
}

bool MatterLedger::audit(const World& world) const {
    return audit_failures(world).empty();
}

std::vector<VoxelTypeId> MatterLedger::audit_failures(const World& world) const {
    const std::unordered_map<VoxelTypeId, i64> counted = recount(world);
    std::vector<VoxelTypeId> failures;

    for (const auto& [type, expected] : totals_) {
        const auto found = counted.find(type);
        const i64 actual = (found == counted.end()) ? 0 : found->second;
        if (actual != expected) failures.push_back(type);
    }
    for (const auto& [type, actual] : counted) {
        if (totals_.find(type) == totals_.end() && actual != 0) failures.push_back(type);
    }

    std::sort(failures.begin(), failures.end());
    failures.erase(std::unique(failures.begin(), failures.end()), failures.end());
    return failures;
}

}  // namespace ws
