#include "world/chunk.hpp"

#include <algorithm>

#include "core/assert.hpp"
#include "core/hash.hpp"

namespace ws {
namespace {

constexpr u32 kBricksPerAxis = static_cast<u32>(kChunkBricks);   // 32
constexpr u32 kEdge = static_cast<u32>(kBrickEdge);              // 8

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
        return index;
    }
    const u32 index = static_cast<u32>(bricks_.size());
    bricks_.emplace_back();
    return index;
}

void Chunk::free_brick(u32 index) {
    bricks_[index].fill(kAir);
    free_bricks_.push_back(index);
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
    free_brick(index);
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

Brick& Chunk::brick_for_write(u32 bx, u32 by, u32 bz) {
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

bool Chunk::set(u32 x, u32 y, u32 z, VoxelTypeId type) {
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
        const bool changed = bricks_[index].set(x % kEdge, y % kEdge, z % kEdge, kAir);
        if (!changed) return false;
        ++revision_;
        // The last voxel going is what frees the brick. See drop_brick_if_empty: an empty
        // brick left allocated is the renderer being told the world still holds matter
        // here, which is a shadow that outlives what cast it (D348).
        if (bricks_[index].empty()) unlink_brick(bx, by, bz, path, index);
        return true;
    }

    const bool changed = brick_for_write(bx, by, bz).set(x % kEdge, y % kEdge, z % kEdge, type);
    if (changed) ++revision_;
    return changed;
}

u32 Chunk::node_count() const {
    return static_cast<u32>(nodes_.size() - free_nodes_.size());
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

bool Chunk::prune(u32 node_index, u32 depth) {
    Node& node = nodes_[node_index];
    bool any_child = false;

    for (u32 slot = 0; slot < 8; ++slot) {
        u32& child = node.child[slot];
        if (child == kNoChild) continue;

        if (depth == kChunkDepth - 1) {
            Brick& b = bricks_[child];
            b.compact();
            if (b.empty()) {
                free_brick(child);
                child = kNoChild;
                --brick_count_;
                continue;
            }
        } else if (prune(child, depth + 1)) {
            free_node(child);
            child = kNoChild;
            continue;
        }
        any_child = true;
    }
    return !any_child;
}

void Chunk::compact() {
    const u32 before = brick_count_;
    prune(0, 0);   // the root is kept even when empty; a chunk always has one
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
                if (!bricks_[child].validate()) { ok = false; break; }
            } else {
                if (child >= nodes_.size()) { ok = false; break; }
                if (node_seen[child] != 0) { ok = false; break; }
                node_seen[child] = 1;
                stack.emplace_back(child, depth + 1);
            }
        }
    }

    return ok && live_bricks == brick_count_;
}

}  // namespace ws
