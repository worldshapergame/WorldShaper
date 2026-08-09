#include "world/node_pool.hpp"

#include <algorithm>
#include <cstring>

#include "core/assert.hpp"
#include "world/voxel_type.hpp"

namespace ws {

namespace {

// Round up to a power of two, so the entry table's modulo is a mask.
u32 next_power_of_two(u32 value) {
    u32 result = 1;
    while (result < value) result <<= 1;
    return result;
}

// One byte per face direction: +x -x +y -y in the first word, +z -z in the second. Nothing else
// shares these words — see the note on GpuNode::coverage_z for what happened when something did.
void pack_coverage(const u32 faces[6], u32& xy, u32& z) {
    xy = faces[0] | (faces[1] << 8) | (faces[2] << 16) | (faces[3] << 24);
    z = faces[4] | (faces[5] << 8);
}

void unpack_coverage(u32 xy, u32 z, u32 faces[6]) {
    faces[0] = xy & 0xFFu;
    faces[1] = (xy >> 8) & 0xFFu;
    faces[2] = (xy >> 16) & 0xFFu;
    faces[3] = (xy >> 24) & 0xFFu;
    faces[4] = z & 0xFFu;
    faces[5] = (z >> 8) & 0xFFu;
}

// The coordinates are truncated to 32 bits here exactly as they are in the node record, so the
// shader — which has no 64-bit arithmetic in its inner loop — computes the same bucket from the
// same numbers. A false match still needs two roots 2^32 apart at one level, which at the entry
// level is 137 billion kilometres, and the probe compares the stored coordinates anyway.
usize entry_bucket(const NodeKey& key, usize capacity) {
    return static_cast<usize>(entry_hash32(static_cast<i32>(key.x), static_cast<i32>(key.y),
                                           static_cast<i32>(key.z), key.level)) &
           (capacity - 1);
}

}  // namespace

void NodeUploadBatch::clear() {
    nodes.clear();
    leaves.clear();
    payload_from.clear();
    payload_size.clear();
    payload_bytes = 0;
    built = 0;
    evicted = 0;
    deferred = 0;
    out_of_memory = false;
}

void NodePool::create(const NodePoolBudget& budget, const VoxelTypeTable& types) {
    budget_ = budget;
    types_ = &types;

    nodes_.assign(budget_.max_nodes, GpuNode{});
    occupancy_.assign(static_cast<usize>(budget_.max_occupancy_leaves) * kBrickWords, 0);
    payload_.assign(static_cast<usize>(budget_.payload_bytes), 0);
    payload_pool_.create(budget_.payload_bytes);

    leaves_.assign(budget_.max_occupancy_leaves, GpuBrickHeader{});
    leaf_payload_size_.assign(budget_.max_occupancy_leaves, 0);
    free_leaves_.clear();
    next_leaf_ = 0;
    payload_high_ = 0;

    // A quarter of the node count, rounded up to a power of two. Entry-level nodes are a small
    // fraction of the tree — everything below them is reached by pointer — so this is generous.
    entries_.assign(next_power_of_two(std::max<u32>(budget_.max_nodes / 4, 1024)), kNoNode);

    // Sized to the arrays they track. Nothing is marked: both sides start empty, and everything
    // built from here marks itself on the way in.
    dirty_nodes_.create(budget_.max_nodes);
    dirty_leaves_.create(budget_.max_occupancy_leaves);
    dirty_entries_.create(entries_.size());
    dirty_payload_.clear();
    // The one array whose empty value is not zero: a free bucket is kNoNode, which is all ones,
    // and a device buffer starts at all zeroes. Copying whole prefixes hid that; sending only
    // what changed does not, so the initial state is a change like any other.
    dirty_entries_.mark_range(0, entries_.size());

    next_free_ = 0;
    free_singles_.clear();
    free_runs_.clear();
    out_of_room_ = false;
    occupied_.clear();
    indexed_chunks_ = 0;

    live_.clear();
    requested_.clear();
    dirty_.clear();
    batch_.clear();
    builds_ = 0;
    evictions_ = 0;
    requests_ = 0;
    hits_ = 0;
}

// ---- allocation ----------------------------------------------------------------------------
//
// A bump pointer and two free lists, and never a search for a contiguous run.
//
// The first version carved runs off the tail of one free list and checked that the eight it found
// happened to be consecutive. They are — exactly until something is freed, after which the check
// fails, allocation returns kNoNode, and the caller reads that as an empty region. A tree that
// stops building because it ran out of memory must not look like a tree that stopped because the
// world is empty. `out_of_room_` is how the two are told apart.

u32 NodePool::allocate_node() {
    if (!free_singles_.empty()) {
        const u32 slot = free_singles_.back();
        free_singles_.pop_back();
        nodes_[slot] = GpuNode{};
        dirty_nodes_.mark(slot);
        return slot;
    }
    if (next_free_ >= budget_.max_nodes) {
        out_of_room_ = true;
        return kNoNode;
    }
    const u32 slot = next_free_++;
    nodes_[slot] = GpuNode{};
    dirty_nodes_.mark(slot);
    return slot;
}

u32 NodePool::allocate_children() {
    if (!free_runs_.empty()) {
        const u32 base = free_runs_.back();
        free_runs_.pop_back();
        for (u32 i = 0; i < 8; ++i) nodes_[base + i] = GpuNode{};
        dirty_nodes_.mark_range(base, 8);
        return base;
    }
    if (next_free_ + 8 > budget_.max_nodes) {
        out_of_room_ = true;
        return kNoNode;
    }
    const u32 base = next_free_;
    next_free_ += 8;
    for (u32 i = 0; i < 8; ++i) nodes_[base + i] = GpuNode{};
    dirty_nodes_.mark_range(base, 8);
    return base;
}

// ---- what the world actually holds ------------------------------------------------------------

void NodePool::index_world(const World& world) {
    occupied_.clear();
    world.for_each_chunk([this](const ChunkCoord& coord, const Chunk& chunk) {
        if (chunk.empty()) return;
        // A chunk is 256 voxels, which is level 8. Every level above it records the block that
        // contains this chunk, so a descent dismisses an empty branch with one lookup instead of
        // eight recursions that each do the same.
        for (u32 level = 8; level <= kMaxNodeLevel; ++level) {
            const u32 shift = level - 8;
            occupied_.insert(NodeKey{coord.x >> shift, coord.y >> shift, coord.z >> shift, level});
        }
    });
    indexed_chunks_ = world.chunk_count();
}

bool NodePool::world_has(const World& world, const NodeKey& key) const {
    if (key.level >= 8) return occupied_.find(key) != occupied_.end();

    // Inside a chunk the octree answers directly, and the walk below level 8 is bounded by the
    // brick positions the node covers — which is only reached when the chunk exists at all.
    const i64 vx = key.x << key.level;
    const i64 vy = key.y << key.level;
    const i64 vz = key.z << key.level;
    const Chunk* chunk = world.chunk(chunk_coord_of(vx, vy, vz));
    if (chunk == nullptr) return false;

    const u32 span = 1u << (key.level - kLeafLevel);
    const u32 bx0 = static_cast<u32>((vx >> 3) & 31);
    const u32 by0 = static_cast<u32>((vy >> 3) & 31);
    const u32 bz0 = static_cast<u32>((vz >> 3) & 31);
    for (u32 bz = 0; bz < span; ++bz) {
        for (u32 by = 0; by < span; ++by) {
            for (u32 bx = 0; bx < span; ++bx) {
                if (chunk->brick(bx0 + bx, by0 + by, bz0 + bz) != nullptr) return true;
            }
        }
    }
    return false;
}

// ---- the entry table ---------------------------------------------------------------------------
//
// Open addressing with linear probing over nodes at or above kEntryLevel. Everything below is
// reached by descending from one of these, so this table holds a small fraction of the tree and a
// probe stays short.

u32 NodePool::find(const NodeKey& key) const {
    if (entries_.empty()) return kNoNode;

    // The ancestor that lives in the table: the node itself when it is coarse enough, otherwise
    // the one at kEntryLevel containing it.
    const u32 enter_level = std::max(key.level, kEntryLevel);
    const u32 up = enter_level - key.level;
    const NodeKey enter{key.x >> up, key.y >> up, key.z >> up, enter_level};

    const usize capacity = entries_.size();
    const usize bucket = entry_bucket(enter, capacity);
    u32 slot = kNoNode;
    for (usize probe = 0; probe < capacity; ++probe) {
        const u32 candidate = entries_[(bucket + probe) & (capacity - 1)];
        if (candidate == kNoNode) return kNoNode;   // an empty bucket ends the run
        const GpuNode& node = nodes_[candidate];
        if (node_level(node) == enter.level && node.x == static_cast<i32>(enter.x) &&
            node.y == static_cast<i32>(enter.y) && node.z == static_cast<i32>(enter.z)) {
            slot = candidate;
            break;
        }
    }
    if (slot == kNoNode || key.level == enter_level) return slot;

    // Then straight down by octant. No hash, no search: the child index is arithmetic.
    for (u32 level = enter_level; level > key.level; --level) {
        const GpuNode& node = nodes_[slot];
        // A leaf's `children` is a leaf id, not a slot. Descending into it would read some
        // unrelated node and call it geometry.
        if ((node_flags(node) & kNodeLeaf) != 0) return kNoNode;
        if (node.children == kNoNode) return kNoNode;
        const u32 shift = level - 1 - key.level;
        const u32 octant = octant_of(key.x >> shift, key.y >> shift, key.z >> shift);
        if ((node_child_mask(node) & (1u << octant)) == 0) return kNoNode;   // empty
        slot = node.children + octant;
        // The mask says the world has it; a level of nought says the pool has not built it. Those
        // are different answers and only one of them means "nothing to draw here" — the other
        // means "ask for it". A descent that conflated them would report empty space over an
        // unstreamed building and never request it.
        if (node_level(nodes_[slot]) == 0) return kNoNode;
    }
    return slot;
}

// Where a descent stops, and why. The shader needs the same three-way answer the CPU has, because
// "empty", "here" and "wanted but not built" drive three different behaviours: skip, draw, report.
NodeFind NodePool::locate(const NodeKey& key) const {
    NodeFind result;
    if (entries_.empty()) return result;

    const u32 enter_level = std::max(key.level, kEntryLevel);
    const u32 up = enter_level - key.level;
    const NodeKey enter{key.x >> up, key.y >> up, key.z >> up, enter_level};

    const usize capacity = entries_.size();
    const usize bucket = entry_bucket(enter, capacity);
    u32 slot = kNoNode;
    for (usize probe = 0; probe < capacity; ++probe) {
        const u32 candidate = entries_[(bucket + probe) & (capacity - 1)];
        if (candidate == kNoNode) break;
        const GpuNode& node = nodes_[candidate];
        if (node_level(node) == enter.level && node.x == static_cast<i32>(enter.x) &&
            node.y == static_cast<i32>(enter.y) && node.z == static_cast<i32>(enter.z)) {
            slot = candidate;
            break;
        }
    }
    if (slot == kNoNode) {
        // Nothing at the entry level. The world may still have something here — the pool simply
        // has no root for it — so this is a request, not empty space.
        result.level = enter_level;
        result.wanted = true;
        return result;
    }

    result.slot = slot;
    result.level = enter_level;
    for (u32 level = enter_level; level > key.level; --level) {
        const GpuNode& node = nodes_[result.slot];
        if ((node_flags(node) & kNodeLeaf) != 0) return result;   // as fine as it gets
        const u32 shift = level - 1 - key.level;
        const u32 octant = octant_of(key.x >> shift, key.y >> shift, key.z >> shift);
        if ((node_child_mask(node) & (1u << octant)) == 0) {
            // Empty, and empty at a known size: the whole cell one level down holds nothing, so a
            // ray can jump the width of it. This is the coarse skip, and it falls out of the
            // descent rather than needing five separate occupancy grids to carry it.
            result.empty_below = true;
            return result;
        }
        // A shell — the world has children here and the pool has no run for them. Wanted, not
        // here, and tested before the arithmetic that would otherwise index off kNoNode.
        if (node.children == kNoNode) {
            result.wanted = true;
            return result;
        }
        const u32 child = node.children + octant;
        if (node_level(nodes_[child]) == 0) {
            result.wanted = true;   // the world has it; ask for it
            return result;
        }
        result.slot = child;
        result.level = level - 1;
    }
    return result;
}

VoxelTypeId NodePool::mirror_voxel(i64 x, i64 y, i64 z) const {
    const u32 slot = find(node_key_of(x, y, z, kLeafLevel));
    if (slot == kNoNode) return kAir;
    const GpuNode& node = nodes_[slot];
    if ((node_flags(node) & kNodeLeaf) == 0) return kAir;

    const u32 leaf = node.children;
    if (leaf == kNoNode || leaf >= leaves_.size()) return kAir;

    const u32 lx = static_cast<u32>(x & (kBrickEdge - 1));
    const u32 ly = static_cast<u32>(y & (kBrickEdge - 1));
    const u32 lz = static_cast<u32>(z & (kBrickEdge - 1));
    const u32 index = brick_index(lx, ly, lz);

    // Occupancy first, because an unset bit means air whatever the payload says — and it is the
    // one 64-byte read a ray makes for every brick it touches.
    const usize word = static_cast<usize>(leaf) * kBrickWords + (index >> 6);
    if (word >= occupancy_.size()) return kAir;
    if (((occupancy_[word] >> (index & 63)) & 1ull) == 0) return kAir;

    // decode_voxel indexes from the brick's own payload, not from the pool — residency has always
    // passed it a pre-offset pointer. Passing the pool base instead works for exactly the one
    // brick that landed at offset zero and reads a neighbour's palette for every other, which is
    // why the slab decoded and the wall sharing its brick did not.
    const GpuBrickHeader& header = leaves_[leaf];
    const u8* base = (header.index_bits == 0 || header.payload_offset == kNoOffset)
                         ? nullptr
                         : payload_.data() + header.payload_offset;
    return decode_voxel(header, base, index);
}

// ---- building --------------------------------------------------------------------------------

u32 NodePool::build_leaf(const World& world, const NodeKey& key, u32& budget) {
    if (budget == 0) return kNoNode;

    // A leaf is a brick, so this is where the pool meets storage. Chunks are still how the world
    // holds bricks; the pool never exposes that.
    const i64 vx = key.x << kLeafLevel;
    const i64 vy = key.y << kLeafLevel;
    const i64 vz = key.z << kLeafLevel;
    const Chunk* chunk = world.chunk(chunk_coord_of(vx, vy, vz));
    if (chunk == nullptr) return kNoNode;

    const u32 bx = static_cast<u32>((vx >> 3) & 31);
    const u32 by = static_cast<u32>((vy >> 3) & 31);
    const u32 bz = static_cast<u32>((vz >> 3) & 31);
    const Brick* brick = chunk->brick(bx, by, bz);
    if (brick == nullptr) return kNoNode;

    GpuBrickHeader header{};
    std::vector<u8> encoded;
    u64 bits[kBrickWords]{};
    encode_brick(*brick, *types_, header, encoded, bits);

    // Entirely air. The encoder still produces a record, but a node with nothing in it is not a
    // node — it is the absence of one, and the parent's child mask is where that is said.
    bool any = false;
    for (u32 i = 0; i < kBrickWords; ++i) any = any || (bits[i] != 0);
    if (!any) return kNoNode;

    const u32 slot = allocate_node();
    if (slot == kNoNode) return kNoNode;   // out_of_room_ is already set

    // A leaf id of its own, so the sixty-four bytes of occupancy and the brick header stay put
    // when the node is later moved into its parent's contiguous run.
    u32 leaf = kNoNode;
    if (!free_leaves_.empty()) {
        leaf = free_leaves_.back();
        free_leaves_.pop_back();
    } else if (next_leaf_ < budget_.max_occupancy_leaves) {
        leaf = next_leaf_++;
    }
    if (leaf == kNoNode) {
        out_of_room_ = true;
        free_singles_.push_back(slot);
        return kNoNode;
    }
    --budget;

    if (header.index_bits != 0) {
        // The encoded bytes, staged here rather than "later, by the streaming layer" — which is
        // what the first version said, and it meant every non-uniform brick decoded as air. A
        // pool that does not hold the voxel data is not a replacement for residency.
        const u32 offset = payload_pool_.allocate(static_cast<u32>(encoded.size()));
        if (offset == kNoOffset) {
            out_of_room_ = true;
            free_leaves_.push_back(leaf);
            free_singles_.push_back(slot);
            ++budget;
            return kNoNode;
        }
        std::memcpy(&payload_[offset], encoded.data(), encoded.size());
        batch_.payload_from.push_back(offset);
        batch_.payload_size.push_back(static_cast<u32>(encoded.size()));
        dirty_payload_.emplace_back(static_cast<u64>(offset), static_cast<u64>(encoded.size()));
        header.payload_offset = offset;
        leaf_payload_size_[leaf] = static_cast<u32>(encoded.size());
        payload_high_ = std::max<u64>(payload_high_, offset + encoded.size());
    } else {
        header.payload_offset = kNoOffset;
        leaf_payload_size_[leaf] = 0;
    }

    leaves_[leaf] = header;
    std::memcpy(&occupancy_[static_cast<usize>(leaf) * kBrickWords], bits,
                sizeof(u64) * kBrickWords);
    // Occupancy follows its leaf: kBrickWords words at leaf * kBrickWords, so one mark covers
    // both arrays and they can never disagree about which leaf is stale.
    dirty_leaves_.mark(leaf);

    // Coverage per face direction, from the occupancy bits: for each direction, how much of the
    // 8x8 face-on projection has any matter behind it. This is what an edge is anti-aliased
    // against, and it is exact.
    u32 faces[6]{};
    for (u32 axis = 0; axis < 3; ++axis) {
        u32 covered = 0;
        for (u32 a = 0; a < kBrickEdge; ++a) {
            for (u32 b = 0; b < kBrickEdge; ++b) {
                bool hit = false;
                for (u32 c = 0; c < kBrickEdge && !hit; ++c) {
                    const u32 x = (axis == 0) ? c : a;
                    const u32 y = (axis == 1) ? c : ((axis == 0) ? a : b);
                    const u32 z = (axis == 2) ? c : b;
                    const u32 index = brick_index(x, y, z);
                    hit = ((bits[index >> 6] >> (index & 63)) & 1ull) != 0;
                }
                if (hit) ++covered;
            }
        }
        // The same both ways along an axis: a projection has no sense. Never rounds to nothing —
        // one solid voxel reports "present, however faintly" (D139).
        const u32 value = (covered == 0) ? 0 : std::max<u32>(1, covered * 255 / 64);
        faces[axis * 2] = value;
        faces[axis * 2 + 1] = value;
    }

    const u32 flags = kNodeLeaf | ((header.index_bits == 0) ? kNodeUniform : 0u);
    pack_coverage(faces, nodes_[slot].coverage_xy, nodes_[slot].coverage_z);
    nodes_[slot].x = static_cast<i32>(key.x);
    nodes_[slot].y = static_cast<i32>(key.y);
    nodes_[slot].z = static_cast<i32>(key.z);
    nodes_[slot].children = leaf;
    nodes_[slot].colour = header.average_colour;
    nodes_[slot].packed = pack_node(kLeafLevel, flags, 0);
    dirty_nodes_.mark(slot);
    ++builds_;
    return slot;
}

// Folded from its eight children, never summarised from the world (D152).
//
// The alternative — sample the region and average — was built, measured and thrown away once
// already. Sampling misses thin structure, and thin structure is most of a world: a floor one
// brick thick sampled at stride four is simply not there, and the tier drew empty sky over ground
// that was plainly visible. Folding is exact: every voxel contributes to exactly one cell at every
// level, so a railing thins as it should and never disappears.
void NodePool::fold_children(u32 slot) {
    GpuNode& node = nodes_[slot];
    if ((node_flags(node) & kNodeLeaf) != 0 || node.children == kNoNode) return;
    // Every path out of here rewrites the node's colour, coverage and flags.
    dirty_nodes_.mark(slot);

    u64 r = 0, g = 0, b = 0, cover = 0;
    u32 present = 0;
    u32 flags = node_flags(node);
    u32 faces[6]{};

    for (u32 octant = 0; octant < 8; ++octant) {
        if ((node_child_mask(node) & (1u << octant)) == 0) continue;
        const GpuNode& child = nodes_[node.children + octant];
        // A mask bit with nothing behind it is a child the world has and the pool has not built.
        // It contributes no colour — inventing one would be summarising from a guess — and the
        // node is re-folded when the child arrives.
        if (node_level(child) == 0) continue;
        const u32 alpha = (child.colour >> 24) & 0xFFu;
        r += static_cast<u64>(child.colour & 0xFFu) * alpha;
        g += static_cast<u64>((child.colour >> 8) & 0xFFu) * alpha;
        b += static_cast<u64>((child.colour >> 16) & 0xFFu) * alpha;
        cover += alpha;
        ++present;
        flags |= node_flags(child) & (kNodeEmissive | kNodeTransmissive);

        u32 child_faces[6];
        unpack_coverage(child.coverage_xy, child.coverage_z, child_faces);
        // Along a direction two children in line share the same projected area, so the parent
        // takes the largest of what its children present. That errs towards *present*, which is
        // the same choice as the coverage floor and made for the same reason: a thing slightly
        // too solid at a distance is still the thing, and a thing that rounded away is not.
        for (u32 f = 0; f < 6; ++f) faces[f] = std::max(faces[f], child_faces[f]);
    }

    if (present == 0 || cover == 0) {
        node.colour = 0;
    } else {
        const u32 red = static_cast<u32>(r / cover);
        const u32 green = static_cast<u32>(g / cover);
        const u32 blue = static_cast<u32>(b / cover);
        // Coverage halves as the cells double, and floors at "present" rather than at zero.
        const u32 alpha = std::max<u32>(1, static_cast<u32>(cover / 8));
        node.colour = red | (green << 8) | (blue << 16) | (alpha << 24);
    }
    pack_coverage(faces, node.coverage_xy, node.coverage_z);
    node.packed = pack_node(node_level(node), flags, node_child_mask(node));
}

// A node on its own, with a child mask taken from the world and no children built yet.
//
// This is what makes the pool pixel-driven rather than exhaustive. The first version built a
// root's entire subtree down to leaves, so one entry node — 512 m across — tried to build the
// whole scene at voxel resolution: 262,144 leaves, the budget saturated, and eleven milliseconds
// of marching against a tree that was mostly the wrong size. Depth has to come from what the
// pixels asked for, and a request carries its level.
//
// A shell therefore starts with no colour. It gains one when its children arrive and it is
// folded from them (D152), which is the only exact way — sampling a region to summarise it is
// what D149 measured and threw away. Until then it draws nothing and is reported as wanted, so
// the picture fills in from coarse to fine as the requests are served.
u32 NodePool::build_shell(const World& world, const NodeKey& key, u32& budget) {
    if (key.level == kLeafLevel) return build_leaf(world, key, budget);
    if (budget == 0) return kNoNode;

    const u32 slot = allocate_node();
    if (slot == kNoNode) return kNoNode;
    --budget;

    // The mask says what the WORLD has, so a ray can tell empty space from something it has not
    // been given yet. Without that distinction nothing is ever reported and nothing is streamed.
    u32 mask = 0;
    for (u32 octant = 0; octant < 8; ++octant) {
        const NodeKey child{(key.x << 1) | static_cast<i64>(octant & 1),
                            (key.y << 1) | static_cast<i64>((octant >> 1) & 1),
                            (key.z << 1) | static_cast<i64>((octant >> 2) & 1), key.level - 1};
        if (world_has(world, child)) mask |= (1u << octant);
    }
    if (mask == 0) {
        free_singles_.push_back(slot);
        ++budget;
        return kNoNode;
    }

    nodes_[slot].x = static_cast<i32>(key.x);
    nodes_[slot].y = static_cast<i32>(key.y);
    nodes_[slot].z = static_cast<i32>(key.z);
    nodes_[slot].children = kNoNode;
    nodes_[slot].colour = 0;
    nodes_[slot].packed = pack_node(key.level, 0, mask);
    dirty_nodes_.mark(slot);
    ++builds_;
    return slot;
}

// Walks from the root down to the level that was asked for, creating whatever is missing on the
// way and folding the chain back up so every ancestor learns what arrived beneath it.
//
// Bounded by `key.level`: a node smaller than the pixel that asked for it is never created, which
// is the whole of "if you cannot see it, it does not exist" (D190).
u32 NodePool::refine(const World& world, const NodeKey& key, u32 root_slot, u32& budget) {
    u32 chain[kMaxNodeLevel + 1];
    u32 depth = 0;

    u32 slot = root_slot;
    chain[depth++] = slot;

    for (u32 level = kEntryLevel; level > key.level; --level) {
        const GpuNode& node = nodes_[slot];
        if ((node_flags(node) & kNodeLeaf) != 0) break;

        const u32 shift = level - 1 - key.level;
        const u32 octant = octant_of(key.x >> shift, key.y >> shift, key.z >> shift);
        if ((node_child_mask(node) & (1u << octant)) == 0) break;   // genuinely empty

        if (nodes_[slot].children == kNoNode) {
            const u32 run = allocate_children();
            if (run == kNoNode) break;
            nodes_[slot].children = run;
            dirty_nodes_.mark(slot);
        }

        const u32 child = nodes_[slot].children + octant;
        if (node_level(nodes_[child]) == 0) {
            const NodeKey child_key{key.x >> shift, key.y >> shift, key.z >> shift, level - 1};
            const u32 built = build_shell(world, child_key, budget);
            if (built == kNoNode) break;
            // Built into a slot of its own and then MOVED here, so both ends change: the
            // destination gains the record, and the source is cleared before going back on the
            // free list. Marking only the build (which build_shell does) leaves the card holding
            // whatever was in `child` before -- caught by NodeBuffers::audit at byte 18,240,
            // which is precisely the failure that check exists for.
            nodes_[child] = nodes_[built];
            nodes_[built] = GpuNode{};
            dirty_nodes_.mark(child);
            dirty_nodes_.mark(built);
            free_singles_.push_back(built);
        }

        slot = child;
        if (depth <= kMaxNodeLevel) chain[depth++] = slot;
    }

    // Back up the chain, so a node that gained a child gains its share of the colour with it.
    // Folding on the way down would fold from children that did not exist yet.
    for (u32 i = depth; i > 0; --i) fold_children(chain[i - 1]);
    return slot;
}

// Frees what a node owns — its leaf, or its run of children and everything under them — but not
// its own slot, because who owns that depends on where the node lives.
//
// The distinction is not pedantry. A node inside a run is not separately allocated: the run is,
// eight at a time, and giving a run member back to the singles list while also giving the run
// back to the runs list hands the same slot out twice. `allocate_node` then returns a slot that
// is part of a live run, and two different nodes write to it.
//
// The first version did exactly that, and it was invisible while every child of a run was built:
// the double-freed slots were re-used in the same order they were freed and happened to line up.
// Shells broke the symmetry — a run can now hold children that were never built — and the fault
// surfaced as a leaf that simply failed to appear after an edit.
void NodePool::release_contents(u32 slot) {
    GpuNode& node = nodes_[slot];

    if ((node_flags(node) & kNodeLeaf) != 0) {
        const u32 leaf = node.children;
        if (leaf != kNoNode && leaf < leaves_.size()) {
            if (leaf_payload_size_[leaf] > 0) {
                payload_pool_.release(leaves_[leaf].payload_offset, leaf_payload_size_[leaf]);
                leaf_payload_size_[leaf] = 0;
            }
            leaves_[leaf] = GpuBrickHeader{};
            dirty_leaves_.mark(leaf);
            free_leaves_.push_back(leaf);
        }
    } else if (node.children != kNoNode) {
        for (u32 octant = 0; octant < 8; ++octant) {
            const u32 child = node.children + octant;
            // A mask bit with a level of nought is a child the world has and the pool never
            // built. There is nothing under it to give back.
            if (node_level(nodes_[child]) == 0) continue;
            release_contents(child);
            nodes_[child] = GpuNode{};
            dirty_nodes_.mark(child);
        }
        free_runs_.push_back(node.children);
    }

    node = GpuNode{};
    dirty_nodes_.mark(slot);
    ++evictions_;
}

// A root: its slot came from the singles list, so that is where it goes back.
void NodePool::release(u32 slot) {
    if (slot == kNoNode || slot >= nodes_.size()) return;
    release_contents(slot);
    free_singles_.push_back(slot);
}

// ---- the frame -------------------------------------------------------------------------------

void NodePool::request(const NodeKey& key) {
    ++requests_;
    requested_.push_back(key);
}

// Read by a ray, so it is wanted. Deliberately NOT a request: there is nothing to build, and
// putting it through `requested_` would make every visible root re-descend its whole path every
// frame for a tree that is already complete.
void NodePool::touch(const NodeKey& key) {
    const u32 enter_level = std::max(key.level, kEntryLevel);
    const u32 up = enter_level - key.level;
    const NodeKey root{key.x >> up, key.y >> up, key.z >> up, enter_level};
    const auto it = live_.find(root);
    if (it != live_.end()) it->second.last_wanted = touch_frame_;
}

void NodePool::invalidate(i64 x, i64 y, i64 z) {
    dirty_.push_back(node_key_of(x, y, z, kLeafLevel));
}

const NodeUploadBatch& NodePool::update(const World& world, const f64 camera_voxel[3],
                                        u64 frame) {
    // What `touch` stamps. It is called from the feedback loop before this runs, so it needs the
    // frame from somewhere; keeping it here rather than passing it through every call site means
    // a hit reported this frame is stamped with the frame it was reported in.
    touch_frame_ = frame;
    batch_.clear();
    out_of_room_ = false;
    u32 budget = budget_.max_builds_per_frame;

    // The index follows the set of chunks, not their contents. Contents changing is what dirty_
    // is for, and rebuilding this on every edit would walk the world on every mouse-down frame.
    if (indexed_chunks_ != world.chunk_count() || occupied_.empty()) {
        index_world(world);
        // Every entry block the world occupies gets a root, whether anything has looked at it or
        // not. That is what lets "no root here" mean "the world is empty here" — and without it a
        // ray crossing open sky finds nothing at the entry level, cannot tell absent-from-the-pool
        // from absent-from-the-world, and reports a phantom every frame for ever.
        //
        // This is D133 exactly, reproduced in a new structure: measured at 3.7 million requests
        // against 367,000 hits, nothing built, and the tree frozen at ten thousand nodes while
        // the marcher drew a scene it could not find. The old system needed a whole second
        // occupancy grid to answer this question; here the answer is a handful of shells,
        // because a root is 512 m across and a world has very few of them.
        for (const NodeKey& key : occupied_) {
            if (key.level != kEntryLevel) continue;
            if (live_.find(key) != live_.end()) continue;
            u32 seed_budget = 1;
            const u32 slot = build_shell(world, key, seed_budget);
            if (slot == kNoNode) continue;
            const usize capacity = entries_.size();
            const usize bucket = entry_bucket(key, capacity);
            bool placed = false;
            for (usize probe = 0; probe < capacity && !placed; ++probe) {
                const usize index = (bucket + probe) & (capacity - 1);
                u32& cell = entries_[index];
                if (cell == kNoNode) { cell = slot; placed = true; dirty_entries_.mark(index); }
            }
            if (!placed) { release(slot); continue; }
            live_.emplace(key, Resident{slot, frame, 0});
            batch_.nodes.push_back(slot);
        }
    }

    // Anything an edit touched is dropped so it is rebuilt from the world.
    //
    // Dropped at the ROOT, once per distinct root, and that is a deliberate retreat from what
    // this used to do. It walked every level from the leaf to kMaxNodeLevel for every dirty
    // brick, released each ancestor it found -- and releasing a root frees its whole subtree
    // anyway -- then scanned the entire 262,144-entry table looking for the slot. A carve of
    // 49.9 million voxels is 97,500 dirty bricks, so that was some twenty million full-table
    // scans in one frame, each of them freeing a subtree that the previous one had already
    // freed. Measured from the player's side: 4 FPS at 276 ms a frame with the GPU at 7 ms.
    //
    // One release per root does the same work once. It is conservative -- a single carved voxel
    // costs the root its whole subtree, which then rebuilds at the rate pixels ask for it -- and
    // refreshing a leaf in place while re-folding its ancestors is the better answer, which
    // needs the parent's child mask to be refreshed from the world as well and belongs with the
    // rest of R2.
    if (!dirty_.empty()) {
        std::unordered_set<NodeKey, NodeKeyHash> roots;
        const u32 up = kEntryLevel - kLeafLevel;
        for (const NodeKey& key : dirty_) {
            roots.insert(NodeKey{key.x >> up, key.y >> up, key.z >> up, kEntryLevel});
        }
        for (const NodeKey& root : roots) {
            const auto it = live_.find(root);
            if (it == live_.end()) continue;
            const u32 slot = it->second.slot;
            release(slot);
            for (usize index = 0; index < entries_.size(); ++index) {
                if (entries_[index] == slot) {
                    entries_[index] = kNoNode;
                    dirty_entries_.mark(index);
                    break;   // a slot appears once; the scan used to run to the end regardless
                }
            }
            live_.erase(it);
        }
    }
    dirty_.clear();

    // The proximity radius, added as ordinary requests so there is one path that builds things
    // and one policy that decides what to build. Zero means off, and off has to mean nothing at
    // all: a loop that still runs once at radius zero holds the camera's own node resident for
    // ever, which is not "off" and took a failing eviction test to notice.
    if (budget_.proximity_voxels > 0) {
        const i64 near = budget_.proximity_voxels;
        const i64 step = static_cast<i64>(1) << kEntryLevel;
        for (i64 dz = -near; dz <= near; dz += step) {
            for (i64 dy = -near; dy <= near; dy += step) {
                for (i64 dx = -near; dx <= near; dx += step) {
                    requested_.push_back(node_key_of(static_cast<i64>(camera_voxel[0]) + dx,
                                                     static_cast<i64>(camera_voxel[1]) + dy,
                                                     static_cast<i64>(camera_voxel[2]) + dz,
                                                     kEntryLevel));
                }
            }
        }
    }

    // Asked once each, however many times it was asked for.
    //
    // Feedback saturates at its capacity the moment a large edit drops the tree - 131,072 entries
    // - and stream() dilates every one of them to its six face neighbours, so 917,504 requests
    // arrive for a handful of distinct nodes. Each was walked from its root, and the walk is the
    // whole of `refine`: eleven levels of dependent loads to discover that everything is already
    // built. Measured after a 49.9-million-voxel carve: `built 914333` in one frame and 252.8 ms
    // of CPU inside this function, against a 7 ms GPU. That is the whole of the 275 ms frame the
    // player reported, and it was invisible because the node path had never been timed.
    //
    // A set costs one hash per request against eleven dependent loads, and the duplicates are the
    // overwhelming majority rather than an edge case.
    seen_requests_.clear();
    for (const NodeKey& key : requested_) {
        if (!seen_requests_.insert(key).second) continue;

        const u32 enter_level = std::max(key.level, kEntryLevel);
        const u32 up = enter_level - key.level;
        const NodeKey root{key.x >> up, key.y >> up, key.z >> up, enter_level};

        const auto it = live_.find(root);
        if (it != live_.end()) {
            it->second.last_wanted = frame;
            ++hits_;
            // The root existing is not the same as the request being served. The first version
            // stopped here, so a tree that had been budget-limited on the frame it was created
            // never filled in — 15,470 reports a frame at frame 120, for ever, with `built` at
            // nought because nothing new was being rooted. A request is for a NODE, not a root.
            if (key.level < kEntryLevel) {
                if (budget > 0) {
                    refine(world, key, it->second.slot, budget);
                    ++batch_.built;
                } else {
                    // Counted, because the alternative is the fault this pool was built to make
                    // unrepresentable, arriving through the instruments instead of the structure.
                    //
                    // A request whose root is live but whose frame has run out of budget used to
                    // fall off the end of this branch and be counted as nothing at all — so
                    // `deferred` read nought while the tree quietly failed to fill in, which is
                    // the same silence D136 put a number next to. It is what made "the pool is
                    // still building 385 nodes a frame three thousand frames after the world
                    // stopped changing" impossible to tell from a pool that was simply busy.
                    ++batch_.deferred;
                }
            }
            continue;
        }
        if (budget == 0) {
            ++batch_.deferred;
            continue;
        }

        const u32 slot = build_shell(world, root, budget);
        if (slot == kNoNode) {
            // Told apart deliberately: nothing there is a fact about the world, no room is a fact
            // about this pool. Reporting the second as the first is how a tree stops building and
            // the renderer draws open sky over a building.
            if (out_of_room_) {
                batch_.out_of_memory = true;
                ++batch_.deferred;
            }
            continue;
        }

        // Publish it. The entry table is what a ray probes to get into the tree, and a node that
        // is not in it might as well not exist.
        const usize capacity = entries_.size();
        const usize bucket = entry_bucket(root, capacity);
        bool placed = false;
        for (usize probe = 0; probe < capacity && !placed; ++probe) {
            const usize index = (bucket + probe) & (capacity - 1);
            u32& cell = entries_[index];
            if (cell == kNoNode) {
                cell = slot;
                placed = true;
                dirty_entries_.mark(index);
            }
        }
        if (!placed) {
            release(slot);
            batch_.out_of_memory = true;
            continue;
        }
        live_.emplace(root, Resident{slot, frame, 0});
        if (key.level < kEntryLevel) refine(world, key, slot, budget);
        batch_.nodes.push_back(slot);
        ++batch_.built;
    }
    requested_.clear();

    // And what has gone cold -- but only when there is something to gain by it.
    //
    // `last_wanted` is refreshed in exactly one place: the request loop above. Requests come from
    // feedback, and feedback reports MISSES. So the moment the tree is built and the rays stop
    // missing, no timestamp advances again, every node goes cold on the same frame, and six
    // hundred frames later the pool evicts the entire scene -- including every node the rays are
    // using right now. Then they miss, it rebuilds, converges, goes quiet, and does it again.
    //
    // That churn is one bug wearing four disguises, all of them recorded as separate mysteries
    // before it was found: a pool "still building 385 nodes a frame three thousand frames after
    // the world stopped changing" (D233), two runs of one binary ending at 89,560 against 81,464
    // nodes, a sky camera whose timing was bimodal because the ray clip follows the resident set
    // (D234), and a converged frame that disagreed with the chunk marcher on 767,526 pixels of
    // 1,024,000 because it had been caught with the tree mostly evicted.
    //
    // The honest fix is for a node to be marked wanted when a ray USES it, which needs the
    // marcher to report hits and not only misses. That is residency policy and belongs to R2.
    // Until then: evict only under pressure. A pool holding ten megabytes of a five-hundred
    // megabyte budget has nothing to gain by throwing away what it just built, and "nothing to
    // gain" is the whole of the argument -- this is not a tuning choice that could go either way.
    // Age decides again, because it finally means something: a ray reading a node refreshes it
    // through `touch`, so a root that has gone cold is one nothing has looked at for
    // `cold_frames` — which is what this loop always believed and was never true before D247.
    //
    // The pressure test stays as a floor under it. A pool holding ten megabytes of a five-hundred
    // megabyte budget gains nothing by evicting, and if the reporting is ever incomplete again
    // this is what stops that becoming a churn instead of a bug report.
    const bool node_pressure = next_free_ > budget_.max_nodes / 4;
    const bool leaf_pressure = next_leaf_ > budget_.max_occupancy_leaves / 4;
    const bool payload_pressure = payload_high_ > budget_.payload_bytes / 4;
    const bool worth_evicting = node_pressure || leaf_pressure || payload_pressure;

    for (auto it = live_.begin(); worth_evicting && it != live_.end();) {
        if (frame - it->second.last_wanted <= budget_.cold_frames) {
            ++it;
            continue;
        }
        const u32 slot = it->second.slot;
        release(slot);
        for (usize index = 0; index < entries_.size(); ++index) {
            if (entries_[index] == slot) {
                entries_[index] = kNoNode;
                dirty_entries_.mark(index);
            }
        }
        ++batch_.evicted;
        it = live_.erase(it);
    }

    return batch_;
}

NodePoolStats NodePool::stats() const {
    NodePoolStats s;
    s.nodes = next_free_ - static_cast<u32>(free_singles_.size()) -
              static_cast<u32>(free_runs_.size()) * 8;
    s.leaves = next_leaf_ - static_cast<u32>(free_leaves_.size());
    s.payload_capacity = budget_.payload_bytes;
    s.node_bytes = static_cast<u64>(s.nodes) * sizeof(GpuNode);
    s.occupancy_bytes = static_cast<u64>(s.leaves) * kBrickWords * sizeof(u64);
    s.total_bytes = s.node_bytes + s.occupancy_bytes + s.payload_in_use +
                    entries_.size() * sizeof(u32);
    s.builds = builds_;
    s.evictions = evictions_;
    s.requests = requests_;
    s.hits = hits_;
    return s;
}

bool NodePool::validate() const {
    for (const auto& [key, resident] : live_) {
        if (resident.slot >= nodes_.size()) return false;
        const GpuNode& node = nodes_[resident.slot];
        if (node_level(node) != key.level) return false;
        if (node.x != static_cast<i32>(key.x)) return false;
        if (node.y != static_cast<i32>(key.y)) return false;
        if (node.z != static_cast<i32>(key.z)) return false;

        // A node's child mask and its children must agree: a bit set with no child under it is a
        // descent into an empty slot, which reads as whatever the previous occupant left there.
        const bool leaf = (node_flags(node) & kNodeLeaf) != 0;
        // A shell — mask set, children not yet allocated — is the ordinary state of a node the
        // pixels have asked about but not asked *into*. It is not a broken node; it is a node
        // whose depth nobody has wanted yet, which is what a depth-bounded build produces.
        if (!leaf && node.children != kNoNode) {
            for (u32 octant = 0; octant < 8; ++octant) {
                if ((node_child_mask(node) & (1u << octant)) == 0) continue;
                const GpuNode& child = nodes_[node.children + octant];
                // Level nought is legitimate here and means "the world has this child and the
                // pool has not built it yet". Anything else must be exactly one level finer.
                if (node_level(child) == 0) continue;
                if (node_level(child) != node_level(node) - 1) return false;
            }
        }
    }
    return true;
}

}  // namespace ws
