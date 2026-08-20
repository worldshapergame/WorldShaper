#include "world/chunk.hpp"

#include <algorithm>

#include "core/assert.hpp"
#include "core/hash.hpp"

namespace ws {
namespace {

constexpr u32 kBricksPerAxis = static_cast<u32>(kChunkBricks);   // 32
constexpr u32 kEdge = static_cast<u32>(kBrickEdge);              // 8

// One bit per brick slot: 32 x 32 x 32 = 32,768 bits = 512 words = 4 KB.
constexpr u32 kErasedWords = (kBricksPerAxis * kBricksPerAxis * kBricksPerAxis) / 64;

constexpr u32 erased_bit(u32 bx, u32 by, u32 bz) {
    return bx + by * kBricksPerAxis + bz * kBricksPerAxis * kBricksPerAxis;
}

}  // namespace

Chunk::Chunk() {
    nodes_.push_back(Node{});
    for (u32& child : nodes_[0].child) child = kNoChild;
}

u32 Chunk::child_index(u32 bx, u32 by, u32 bz, u32 depth) {
    // Depth 0 splits on the top bit of a 5-bit brick coordinate, depth 4 on the bottom.
    const u32 shift = kChunkDepth - 1 - depth;
    return ((bx >> shift) & 1u) | (((by >> shift) & 1u) << 1) | (((bz >> shift) & 1u) << 2);
}

u32 Chunk::allocate_node() {
    if (!free_nodes_.empty()) {
        const u32 index = free_nodes_.back();
        free_nodes_.pop_back();
        for (u32& child : nodes_[index].child) child = kNoChild;
        return index;
    }
    const u32 index = static_cast<u32>(nodes_.size());
    nodes_.push_back(Node{});
    for (u32& child : nodes_[index].child) child = kNoChild;
    return index;
}

u32 Chunk::allocate_brick() {
    if (!free_bricks_.empty()) {
        const u32 index = free_bricks_.back();
        free_bricks_.pop_back();
        bricks_[index].fill(kAir);
        // A recycled brick starts as nobody's. `free_brick` already cleared it; this is the second
        // half of the pair, so a slot handed out again cannot inherit the last owner's claim
        // however it came back.
        bricks_[index].set_edited(false);
        return index;
    }
    const u32 index = static_cast<u32>(bricks_.size());
    bricks_.emplace_back();
    return index;
}

// The one funnel every brick leaves by. Two things happen here and nowhere else: the count of
// edited bricks comes down, and a slot a person had emptied is remembered — see erased_bricks().
void Chunk::free_brick(u32 index, u32 bx, u32 by, u32 bz) {
    if (bricks_[index].edited()) {
        WS_ASSERT(edited_bricks_ > 0, "an edited brick was freed against a count of nought");
        --edited_bricks_;
        bricks_[index].set_edited(false);
        // It is going because its last voxel went, and its last voxel was a person's. That is a
        // carve, and the hole it leaves is the thing the field must not fill back in.
        set_erased(bx, by, bz, true);
    }
    bricks_[index].fill(kAir);
    free_bricks_.push_back(index);
}

bool Chunk::brick_erased(u32 bx, u32 by, u32 bz) const {
    if (erased_.empty()) return false;
    const u32 bit = erased_bit(bx, by, bz);
    return (erased_[bit / 64] & (u64{1} << (bit % 64))) != 0;
}

void Chunk::set_erased(u32 bx, u32 by, u32 bz, bool value) {
    if (!edit_tracking()) return;
    if (erased_.empty()) {
        if (!value) return;   // nothing to clear, and no reason to pay four kilobytes to say so
        erased_.assign(kErasedWords, 0ull);
    }
    const u32 bit = erased_bit(bx, by, bz);
    const u64 mask = u64{1} << (bit % 64);
    const bool was = (erased_[bit / 64] & mask) != 0;
    if (was == value) return;
    if (value) {
        erased_[bit / 64] |= mask;
        ++erased_count_;
    } else {
        erased_[bit / 64] &= ~mask;
        --erased_count_;
    }
}

void Chunk::mark_brick_erased(u32 bx, u32 by, u32 bz) {
    WS_ASSERT(bx < kBricksPerAxis && by < kBricksPerAxis && bz < kBricksPerAxis,
              "brick coordinate out of range");
    // A slot with a brick in it carries its own flag and must not also be in the erased set — that
    // is the invariant `validate()` refuses to be without, so it is enforced here rather than left
    // to a caller getting the order right. A live brick's claim is recorded on the brick.
    if (brick(bx, by, bz) != nullptr) {
        Brick& b = brick_for_write(bx, by, bz);
        if (edit_tracking() && !b.edited()) {
            b.set_edited(true);
            ++edited_bricks_;
        }
        return;
    }
    set_erased(bx, by, bz, true);
}

bool Chunk::brick_edited(u32 bx, u32 by, u32 bz) const {
    WS_ASSERT(bx < kBricksPerAxis && by < kBricksPerAxis && bz < kBricksPerAxis,
              "brick coordinate out of range");
    if (!any_edits()) return false;   // the whole-chunk answer, and the one that is nearly always
                                      // taken
    const Brick* b = brick(bx, by, bz);
    if (b != nullptr) return b->edited();
    return brick_erased(bx, by, bz);
}

void Chunk::free_node(u32 index) {
    WS_ASSERT(index != 0, "the root node is never freed");
    for (u32& child : nodes_[index].child) child = kNoChild;
    free_nodes_.push_back(index);
}

u32 Chunk::find_brick(u32 bx, u32 by, u32 bz) const {
    u32 node = 0;
    for (u32 depth = 0; depth < kChunkDepth - 1; ++depth) {
        const u32 slot = child_index(bx, by, bz, depth);
        const u32 next = nodes_[node].child[slot];
        if (next == kNoChild) return kNoChild;
        node = next;
    }
    return nodes_[node].child[child_index(bx, by, bz, kChunkDepth - 1)];
}

u32 Chunk::find_brick_path(u32 bx, u32 by, u32 bz, u32 path[kChunkDepth]) const {
    u32 node = 0;
    for (u32 depth = 0; depth < kChunkDepth - 1; ++depth) {
        path[depth] = node;
        const u32 slot = child_index(bx, by, bz, depth);
        const u32 next = nodes_[node].child[slot];
        if (next == kNoChild) return kNoChild;
        node = next;
    }
    path[kChunkDepth - 1] = node;
    return nodes_[node].child[child_index(bx, by, bz, kChunkDepth - 1)];
}

// Unlinks a brick that is known to be empty, and then every ancestor that has just lost
// its last child. The root always stands: a chunk has one whether it holds anything or not.
void Chunk::unlink_brick(u32 bx, u32 by, u32 bz, const u32 path[kChunkDepth], u32 index) {
    free_brick(index, bx, by, bz);
    --brick_count_;

    for (u32 depth = kChunkDepth; depth-- > 0;) {
        const u32 node = path[depth];
        nodes_[node].child[child_index(bx, by, bz, depth)] = kNoChild;
        if (depth == 0) break;

        bool any_child = false;
        for (u32 slot = 0; slot < 8 && !any_child; ++slot) {
            any_child = nodes_[node].child[slot] != kNoChild;
        }
        if (any_child) break;
        free_node(node);
    }
}

bool Chunk::drop_brick_if_empty(u32 bx, u32 by, u32 bz) {
    WS_ASSERT(bx < kBricksPerAxis && by < kBricksPerAxis && bz < kBricksPerAxis,
              "brick coordinate out of range");
    u32 path[kChunkDepth];
    const u32 index = find_brick_path(bx, by, bz, path);
    if (index == kNoChild || !bricks_[index].empty()) return false;
    unlink_brick(bx, by, bz, path, index);
    return true;
}

const Brick* Chunk::brick(u32 bx, u32 by, u32 bz) const {
    WS_ASSERT(bx < kBricksPerAxis && by < kBricksPerAxis && bz < kBricksPerAxis,
              "brick coordinate out of range");
    const u32 index = find_brick(bx, by, bz);
    return (index == kNoChild) ? nullptr : &bricks_[index];
}

Brick& Chunk::brick_for_write(u32 bx, u32 by, u32 bz, WriteOrigin origin) {
    WS_ASSERT(bx < kBricksPerAxis && by < kBricksPerAxis && bz < kBricksPerAxis,
              "brick coordinate out of range");
    u32 node = 0;
    for (u32 depth = 0; depth < kChunkDepth - 1; ++depth) {
        const u32 slot = child_index(bx, by, bz, depth);
        u32 next = nodes_[node].child[slot];
        if (next == kNoChild) {
            next = allocate_node();
            nodes_[node].child[slot] = next;
        }
        node = next;
    }

    const u32 slot = child_index(bx, by, bz, kChunkDepth - 1);
    u32 index = nodes_[node].child[slot];
    if (index == kNoChild) {
        index = allocate_brick();
        nodes_[node].child[slot] = index;
        ++brick_count_;
    }
    // R12d. The count and the flag move together and only here, so `edited_bricks_` cannot drift
    // from what the bricks say — which `validate()` checks rather than trusts.
    //
    // Two reasons a brick ends up claimed. The obvious one is that this write IS an edit. The other
    // is that the slot was one a person had emptied: a doorway they cut, now being written into
    // again. Their claim on it does not lapse because somebody else got there first, so it moves
    // out of the erased set and onto the brick — which is the better record, since it travels with
    // the contents through a save, a reload and a re-encode.
    //
    // That second clause is also what keeps the invariant simple: a slot is EITHER a live brick
    // carrying its own flag OR a bit in the erased set, and never both. `validate()` refuses the
    // overlap outright, which it could not if the two were allowed to describe the same slot.
    // It errs towards claiming: a brick the field wrote into a hole a person cut comes back marked
    // as theirs, which costs storage. The other way round costs the hole.
    if (edit_tracking() && (origin == WriteOrigin::Edit || brick_erased(bx, by, bz))) {
        if (!bricks_[index].edited()) {
            bricks_[index].set_edited(true);
            ++edited_bricks_;
        }
        set_erased(bx, by, bz, false);
    }
    return bricks_[index];
}

VoxelTypeId Chunk::get(u32 x, u32 y, u32 z) const {
    WS_ASSERT(x < static_cast<u32>(kChunkEdge) && y < static_cast<u32>(kChunkEdge) &&
                  z < static_cast<u32>(kChunkEdge),
              "voxel coordinate out of range");
    const Brick* b = brick(x / kEdge, y / kEdge, z / kEdge);
    if (b == nullptr) return kAir;
    return b->get(x % kEdge, y % kEdge, z % kEdge);
}

bool Chunk::set(u32 x, u32 y, u32 z, VoxelTypeId type, WriteOrigin origin) {
    WS_ASSERT(x < static_cast<u32>(kChunkEdge) && y < static_cast<u32>(kChunkEdge) &&
                  z < static_cast<u32>(kChunkEdge),
              "voxel coordinate out of range");

    const u32 bx = x / kEdge;
    const u32 by = y / kEdge;
    const u32 bz = z / kEdge;

    if (type == kAir) {
        // Carving where nothing is allocated is free and must not allocate anything —
        // otherwise digging through open air would materialise a brick per step.
        u32 path[kChunkDepth];
        const u32 index = find_brick_path(bx, by, bz, path);
        if (index == kNoChild) return false;
        // Marked on the WRITE and not on the change, which is the same rule `edit_boxes_from_ops`
        // is written to (world_cache.hpp): somebody's hands being here is a fact about the op, not
        // about its result. A swing that met air inside a brick they built still says the brick is
        // theirs. And marked before the write, because a write that takes the last voxel unlinks
        // the brick and it is the flag that turns into the erased bit.
        if (edit_tracking() && origin == WriteOrigin::Edit && !bricks_[index].edited()) {
            bricks_[index].set_edited(true);
            ++edited_bricks_;
            set_erased(bx, by, bz, false);
        }
        const bool changed = bricks_[index].set(x % kEdge, y % kEdge, z % kEdge, kAir);
        if (!changed) return false;
        ++revision_;
        // The last voxel going is what frees the brick. See drop_brick_if_empty: an empty
        // brick left allocated is the renderer being told the world still holds matter
        // here, which is a shadow that outlives what cast it (D348).
        if (bricks_[index].empty()) unlink_brick(bx, by, bz, path, index);
        return true;
    }

    const bool changed =
        brick_for_write(bx, by, bz, origin).set(x % kEdge, y % kEdge, z % kEdge, type);
    if (changed) ++revision_;
    return changed;
}

u32 Chunk::node_count() const {
    return static_cast<u32>(nodes_.size() - free_nodes_.size());
}

u32 Chunk::empty_bricks() const {
    // Over the storage rather than over the octree: 32,768 descents to visit a few hundred
    // bricks is the shape of walk this exists to catch rather than to imitate. The free list is
    // short — it is the bricks freed since the chunk was made — so marking it costs nothing.
    if (bricks_.empty()) return 0;
    std::vector<u8> freed(bricks_.size(), 0);
    for (u32 index : free_bricks_) {
        if (index < freed.size()) freed[index] = 1;
    }
    u32 count = 0;
    for (usize index = 0; index < bricks_.size(); ++index) {
        if (freed[index] == 0 && bricks_[index].empty()) ++count;
    }
    return count;
}

u64 Chunk::solid_voxels() const {
    u64 total = 0;
    for (u32 bz = 0; bz < kBricksPerAxis; ++bz) {
        for (u32 by = 0; by < kBricksPerAxis; ++by) {
            for (u32 bx = 0; bx < kBricksPerAxis; ++bx) {
                const Brick* b = brick(bx, by, bz);
                if (b != nullptr) total += b->solid_count();
            }
        }
    }
    return total;
}

usize Chunk::bytes() const {
    usize total = nodes_.size() * sizeof(Node);
    for (u32 bz = 0; bz < kBricksPerAxis; ++bz) {
        for (u32 by = 0; by < kBricksPerAxis; ++by) {
            for (u32 bx = 0; bx < kBricksPerAxis; ++bx) {
                const Brick* b = brick(bx, by, bz);
                if (b != nullptr) total += b->bytes();
            }
        }
    }
    return total;
}

bool Chunk::prune(u32 node_index, u32 depth, u32 bx, u32 by, u32 bz) {
    bool any_child = false;

    // Depth d decides bit (kChunkDepth - 1 - d) of each axis; see child_index, which this is the
    // inverse of. Carried down rather than looked up afterwards, because a brick freed here has to
    // be able to say where it stood and the descent is the only thing that knows.
    const u32 shift = kChunkDepth - 1 - depth;

    for (u32 slot = 0; slot < 8; ++slot) {
        u32& child = nodes_[node_index].child[slot];
        if (child == kNoChild) continue;
        const u32 cx = bx | ((slot & 1u) << shift);
        const u32 cy = by | (((slot >> 1) & 1u) << shift);
        const u32 cz = bz | (((slot >> 2) & 1u) << shift);

        if (depth == kChunkDepth - 1) {
            Brick& b = bricks_[child];
            b.compact();
            if (b.empty()) {
                const u32 index = child;
                child = kNoChild;
                free_brick(index, cx, cy, cz);
                --brick_count_;
                continue;
            }
        } else if (prune(child, depth + 1, cx, cy, cz)) {
            free_node(child);
            // `free_node` may have grown `free_nodes_` but never `nodes_`, so the reference above
            // would still stand; taken fresh anyway, because `prune` recurses and a reference held
            // across a recursion into a vector is the fault that is invisible until it is not.
            nodes_[node_index].child[slot] = kNoChild;
            continue;
        }
        any_child = true;
    }
    return !any_child;
}

void Chunk::compact() {
    const u32 before = brick_count_;
    prune(0, 0, 0, 0, 0);   // the root is kept even when empty; a chunk always has one
    // Compaction never changes contents, but it does change the encoding, and anything
    // holding a packed copy has to know.
    if (brick_count_ != before) ++revision_;
}

u64 Chunk::content_hash() const {
    // Only occupied bricks contribute, keyed by their coordinate, so a chunk built by
    // filling and then carving hashes the same as one built directly.
    u64 h = 0x243F6A88ull;
    for (u32 bz = 0; bz < kBricksPerAxis; ++bz) {
        for (u32 by = 0; by < kBricksPerAxis; ++by) {
            for (u32 bx = 0; bx < kBricksPerAxis; ++bx) {
                const Brick* b = brick(bx, by, bz);
                if (b == nullptr || b->empty()) continue;
                h = hash_combine(h, (bx << 10) | (by << 5) | bz);
                h = hash_combine(h, b->content_hash());
            }
        }
    }
    return h;
}

u64 Chunk::shape_hash() const {
    // Deliberately the same walk, the same order and the same keying as `content_hash` above, so
    // that the two answer about exactly the same set of bricks and a difference between them is
    // never a difference in what was looked at.
    u64 h = 0x243F6A88ull;
    for (u32 bz = 0; bz < kBricksPerAxis; ++bz) {
        for (u32 by = 0; by < kBricksPerAxis; ++by) {
            for (u32 bx = 0; bx < kBricksPerAxis; ++bx) {
                const Brick* b = brick(bx, by, bz);
                if (b == nullptr || b->empty()) continue;
                h = hash_combine(h, (bx << 10) | (by << 5) | bz);
                h = hash_combine(h, b->shape_hash());
            }
        }
    }
    return h;
}

bool Chunk::validate() const {
    std::vector<u8> node_seen(nodes_.size(), 0);
    std::vector<u8> brick_seen(bricks_.size(), 0);

    // Free lists must not overlap the live tree, and must not contain duplicates.
    for (u32 index : free_nodes_) {
        if (index >= nodes_.size() || index == 0) return false;
        if (node_seen[index] != 0) return false;
        node_seen[index] = 2;
    }
    for (u32 index : free_bricks_) {
        if (index >= bricks_.size()) return false;
        if (brick_seen[index] != 0) return false;
        brick_seen[index] = 2;
    }

    u32 live_bricks = 0;
    u32 live_edited = 0;
    bool ok = true;

    // Iterative walk so a corrupt tree cannot blow the stack before reporting.
    std::vector<std::pair<u32, u32>> stack;   // (node, depth)
    stack.emplace_back(0, 0);
    node_seen[0] = 1;

    while (!stack.empty() && ok) {
        const auto [node_index, depth] = stack.back();
        stack.pop_back();

        for (u32 slot = 0; slot < 8; ++slot) {
            const u32 child = nodes_[node_index].child[slot];
            if (child == kNoChild) continue;

            if (depth == kChunkDepth - 1) {
                if (child >= bricks_.size()) { ok = false; break; }
                if (brick_seen[child] != 0) { ok = false; break; }   // aliased or freed
                brick_seen[child] = 1;
                ++live_bricks;
                if (bricks_[child].edited()) ++live_edited;
                if (!bricks_[child].validate()) { ok = false; break; }
            } else {
                if (child >= nodes_.size()) { ok = false; break; }
                if (node_seen[child] != 0) { ok = false; break; }
                node_seen[child] = 1;
                stack.emplace_back(child, depth + 1);
            }
        }
    }

    if (!ok || live_bricks != brick_count_) return false;

    // R12d, and it is the whole reason a count is safe to keep. `edited_bricks_` is a second index
    // over the same population as the flags on the bricks, and a redundant pair is only as true as
    // the worse of the two — D345 and D358 are one session lost to exactly that, and there the
    // check that would have caught it did not exist. Here the count is re-derived from the tree
    // that was just walked, so nothing can set a flag behind the chunk's back and be believed.
    if (live_edited != edited_bricks_) return false;

    // And the erased set is over slots with NO brick, by construction — a live brick carries its
    // own flag, so a slot claiming both is the two records disagreeing about the same thing.
    if (!erased_.empty()) {
        u32 erased = 0;
        for (u32 bz = 0; bz < kBricksPerAxis; ++bz) {
            for (u32 by = 0; by < kBricksPerAxis; ++by) {
                for (u32 bx = 0; bx < kBricksPerAxis; ++bx) {
                    if (!brick_erased(bx, by, bz)) continue;
                    ++erased;
                    if (brick(bx, by, bz) != nullptr) return false;
                }
            }
        }
        if (erased != erased_count_) return false;
    } else if (erased_count_ != 0) {
        return false;
    }
    return true;
}

}  // namespace ws
