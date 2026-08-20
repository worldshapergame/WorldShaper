#include "world/op.hpp"

#include <algorithm>

#include "core/hash.hpp"
#include "world/world.hpp"

namespace ws {

Op Op::set_voxel(u64 tick, u32 player, i64 x, i64 y, i64 z, VoxelTypeId type,
                 MatterReason reason) {
    Op op;
    op.tick = tick;
    op.player = player;
    op.kind = OpKind::SetVoxel;
    op.reason = reason;
    op.type = type;
    op.x0 = op.x1 = x;
    op.y0 = op.y1 = y;
    op.z0 = op.z1 = z;
    return op;
}

Op Op::fill_box(u64 tick, u32 player, i64 x0, i64 y0, i64 z0, i64 x1, i64 y1, i64 z1,
                VoxelTypeId type, MatterReason reason, WriteMask mask) {
    Op op;
    op.tick = tick;
    op.player = player;
    op.kind = OpKind::FillBox;
    op.reason = reason;
    op.mask = mask;
    op.type = type;
    op.x0 = x0;
    op.y0 = y0;
    op.z0 = z0;
    op.x1 = x1;
    op.y1 = y1;
    op.z1 = z1;
    op.normalise();
    return op;
}

void Op::normalise() {
    if (x0 > x1) std::swap(x0, x1);
    if (y0 > y1) std::swap(y0, y1);
    if (z0 > z1) std::swap(z0, z1);
}

u64 Op::volume() const {
    const u64 dx = static_cast<u64>(x1 - x0) + 1;
    const u64 dy = static_cast<u64>(y1 - y0) + 1;
    const u64 dz = static_cast<u64>(z1 - z0) + 1;
    return dx * dy * dz;
}

u64 Op::content_hash() const {
    u64 h = hash_mix(tick);
    h = hash_combine(h, player);
    h = hash_combine(h, static_cast<u64>(kind));
    h = hash_combine(h, static_cast<u64>(reason));
    h = hash_combine(h, static_cast<u64>(mask));
    h = hash_combine(h, type);
    h = hash_combine(h, static_cast<u64>(x0));
    h = hash_combine(h, static_cast<u64>(y0));
    h = hash_combine(h, static_cast<u64>(z0));
    h = hash_combine(h, static_cast<u64>(x1));
    h = hash_combine(h, static_cast<u64>(y1));
    return hash_combine(h, static_cast<u64>(z1));
}

// R12d — AND THIS IS WHERE THE FLAG IS SET, on all three of the paths below.
//
// **An op is not always a person, and assuming it was is a mistake this made and the suite
// caught.** The obvious reading is that `apply_op` IS the edit path — the chisel, a paste, an undo
// — while the ladder's re-sample goes through `paste_clip` and the cache through
// `read_world_cache`. That is
// true of the game and false of the machinery: the same op record is how terrain is created in the
// first place, and marking those `Edit` claimed **1,204 bricks of 1,204** in a world nobody had
// touched. A world entirely claimed is a world with nothing left to derive, which is R12's payoff
// deleted in one line, and it is silent — every voxel is right, every hash agrees, and only the
// file size says anything at all.
//
// So it is taken from the op's own `reason` rather than from where the call came from.
// `Generation` is terrain being made for the first time and `Load` is a world being deserialised;
// both are the world's own description restating itself, and both are recoverable without a
// record. Everything else — a player, a machine, the simulation moving matter about — is not, and
// nothing outside this world can put it back.
//
// There are three ways a brick is written below (whole from air, whole over something, and a
// sub-box) and every one of them has to say so, because a counter set on some of the paths and not
// the rest reads as success on the rest — trap 27, and here it would read as "nobody edited this"
// over somebody's building.
constexpr WriteOrigin origin_of(MatterReason reason) {
    switch (reason) {
        case MatterReason::Generation:
        case MatterReason::Load:
            return WriteOrigin::Field;
        default:
            return WriteOrigin::Edit;
    }
}

OpResult apply_op(World& world, const Op& op, MatterLedger& ledger) {
    OpResult result;

    Op box = op;
    box.normalise();
    const WriteOrigin origin = origin_of(box.reason);

    std::vector<Brick::TypeCount> histogram;   // reused across bricks

    // Walked chunk by chunk and then brick by brick, so the hash lookup and the octree
    // descent are paid once per 512 voxels rather than once per voxel — and a brick the box
    // covers completely is handled without touching individual voxels at all.
    //
    // The chisel makes multi-million-voxel edits ordinary, and per-voxel World::get was
    // roughly two orders of magnitude short of doing one inside a frame.
    for (i64 cz = chunk_of(box.z0); cz <= chunk_of(box.z1); ++cz) {
        for (i64 cy = chunk_of(box.y0); cy <= chunk_of(box.y1); ++cy) {
            for (i64 cx = chunk_of(box.x0); cx <= chunk_of(box.x1); ++cx) {
                const ChunkCoord coord{cx, cy, cz};
                const i64 base[3] = {cx << 8, cy << 8, cz << 8};
                const i64 lo[3] = {std::max(box.x0, base[0]), std::max(box.y0, base[1]),
                                   std::max(box.z0, base[2])};
                const i64 hi[3] = {std::min(box.x1, base[0] + 255),
                                   std::min(box.y1, base[1] + 255),
                                   std::min(box.z1, base[2] + 255)};

                const u64 span = static_cast<u64>(hi[0] - lo[0] + 1) *
                                 static_cast<u64>(hi[1] - lo[1] + 1) *
                                 static_cast<u64>(hi[2] - lo[2] + 1);

                // Erasing from a chunk that does not exist must not bring it into being, and
                // neither must a write that only lands on existing matter.
                if ((box.type == kAir || box.mask == WriteMask::OntoSolid) &&
                    !world.has_chunk(coord)) {
                    result.voxels_visited += span;
                    continue;
                }

                Chunk& chunk = world.chunk_for_write(coord);
                bool touched = false;

                for (i64 bz = (lo[2] - base[2]) >> 3; bz <= (hi[2] - base[2]) >> 3; ++bz) {
                    for (i64 by = (lo[1] - base[1]) >> 3; by <= (hi[1] - base[1]) >> 3; ++by) {
                        for (i64 bx = (lo[0] - base[0]) >> 3; bx <= (hi[0] - base[0]) >> 3;
                             ++bx) {
                            const i64 brick_base[3] = {base[0] + (bx << 3), base[1] + (by << 3),
                                                       base[2] + (bz << 3)};
                            const i64 blo[3] = {std::max(lo[0], brick_base[0]),
                                                std::max(lo[1], brick_base[1]),
                                                std::max(lo[2], brick_base[2])};
                            const i64 bhi[3] = {std::min(hi[0], brick_base[0] + 7),
                                                std::min(hi[1], brick_base[1] + 7),
                                                std::min(hi[2], brick_base[2] + 7)};

                            const u64 brick_span = static_cast<u64>(bhi[0] - blo[0] + 1) *
                                                   static_cast<u64>(bhi[1] - blo[1] + 1) *
                                                   static_cast<u64>(bhi[2] - blo[2] + 1);
                            result.voxels_visited += brick_span;

                            const u32 bux = static_cast<u32>(bx);
                            const u32 buy = static_cast<u32>(by);
                            const u32 buz = static_cast<u32>(bz);
                            const Brick* existing = chunk.brick(bux, buy, buz);
                            const bool whole_brick =
                                brick_span == static_cast<u64>(kBrickVoxels);

                            if (existing == nullptr) {
                                if (box.type == kAir) continue;   // air into air
                                // Nothing here is solid, so a solid-only write has no work.
                                if (box.mask == WriteMask::OntoSolid) continue;
                                if (whole_brick) {
                                    chunk.brick_for_write(bux, buy, buz, origin)
                                        .fill(box.type);
                                    ledger.record_bulk(kAir, box.type, kBrickVoxels, box.reason,
                                                       box.player);
                                    result.voxels_changed += kBrickVoxels;
                                    touched = true;
                                    continue;
                                }
                            } else if (whole_brick &&
                                       (box.mask == WriteMask::All || existing->uniform())) {
                                // The box covers this brick entirely, so whatever was in it
                                // is going regardless of how it was encoded. The only thing
                                // the old contents are still needed for is the ledger, and
                                // a histogram answers that in one pass instead of 512
                                // read-modify-writes through the palette.
                                //
                                // A masked write can only come this way when the brick is
                                // uniform, because then the mask gives the same answer for
                                // all 512 voxels; otherwise it has to go voxel by voxel.
                                if (existing->uniform() &&
                                    (existing->uniform_value() == box.type ||
                                     !mask_allows(box.mask, existing->uniform_value()))) {
                                    continue;
                                }
                                existing->type_histogram(histogram);
                                for (const Brick::TypeCount& entry : histogram) {
                                    if (entry.type == box.type) continue;
                                    ledger.record_bulk(entry.type, box.type, entry.count,
                                                       box.reason, box.player);
                                    result.voxels_changed += entry.count;
                                }
                                chunk.brick_for_write(bux, buy, buz, origin)
                                    .fill(box.type);
                                // Erasing a whole brick is the commonest way one empties,
                                // and it is the one the chunk cannot see for itself: this
                                // goes around set() to avoid 512 read-modify-writes. An
                                // empty brick left allocated keeps casting the shadow of
                                // what was just deleted (D348).
                                if (box.type == kAir) chunk.drop_brick_if_empty(bux, buy, buz);
                                touched = true;
                                continue;
                            }

                            Brick& brick = chunk.brick_for_write(bux, buy, buz, origin);
                            const u32 changed = brick.fill_range(
                                static_cast<u32>(blo[0] & 7), static_cast<u32>(blo[1] & 7),
                                static_cast<u32>(blo[2] & 7), static_cast<u32>(bhi[0] & 7),
                                static_cast<u32>(bhi[1] & 7), static_cast<u32>(bhi[2] & 7),
                                box.type, box.mask, histogram);
                            if (changed > 0) {
                                for (const Brick::TypeCount& entry : histogram) {
                                    ledger.record_bulk(entry.type, box.type, entry.count,
                                                       box.reason, box.player);
                                }
                                // A partial erase can take the last voxel with it, the
                                // same as the whole-brick case above.
                                if (box.type == kAir) {
                                    chunk.drop_brick_if_empty(bux, buy, buz);
                                }
                                result.voxels_changed += changed;
                                touched = true;
                            }
                        }
                    }
                }

                if (touched) chunk.mark_modified();

                // And if that was the last of it, the chunk goes too. The reference above is dead
                // after this line, which is why it is the last thing done with it.
                //
                // Only worth asking when the edit could have emptied it, and only affordable at
                // all because the bricks were freed as they emptied: `empty()` is a counter.
                if (touched && box.type == kAir) world.drop_chunk_if_empty(coord);
            }
        }
    }
    return result;
}

OpResult apply_ops(World& world, const std::vector<Op>& ops, MatterLedger& ledger) {
    OpResult total;
    for (const Op& op : ops) {
        const OpResult one = apply_op(world, op, ledger);
        total.voxels_changed += one.voxels_changed;
        total.voxels_visited += one.voxels_visited;
    }
    return total;
}

void OpLog::append(const Op& op) {
    ops_.push_back(op);
    hash_ = hash_combine(hash_, op.content_hash());
}

void OpLog::clear() {
    ops_.clear();
    hash_ = 0x6A09E667ull;
}

}  // namespace ws
