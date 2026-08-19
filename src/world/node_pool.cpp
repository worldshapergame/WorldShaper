#include "world/node_pool.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "core/assert.hpp"
#include "core/log.hpp"
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
    evicted_nodes = 0;
    evicted_on_screen = 0;
    churned = 0;
    deferred = 0;
    no_room = 0;
    refused_subpixel = 0;
    evicted_subpixel = 0;
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
    node_last_read_.assign(budget_.max_nodes, 0);
    node_reads_.assign(budget_.max_nodes, 0);
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

    view_ = NodeView{};
    evicted_at_.clear();
    evictions_on_screen_ = 0;
    churn_ = 0;
    for (u32& count : churn_per_level_) count = 0;
    churn_fill_sum_ = 0;
    churn_leaves_ = 0;
    evicted_fill_sum_ = 0;
    evicted_leaves_ = 0;
    evicted_never_read_ = 0;
    churn_never_read_ = 0;
    for (u64& count : churn_by_source_) count = 0;
    requested_source_.clear();

    // R2b's second half, and its arm.
    //
    // Read ONCE, at create, and logged: a policy that can be switched has to say which way it was
    // switched, or a table of numbers taken over two runs means nothing (D621's leaf ceiling is
    // exactly that mistake). `--subpixel-rule` is what sets it; the `WS_SUBPIXEL` environment
    // stopgap this arrived with is gone, because a run's arm belongs in the run's own command line
    // where the log records it (trap 15).
    subpixel_rule_ = budget_.subpixel_rule;
    pixel_angle_ = (budget_.pixel_angle > 0.0) ? budget_.pixel_angle : kSubPixelAngle;
    subpixel_margin_ = budget_.subpixel_margin_levels;
    camera_known_ = false;
    camera_voxel_[0] = camera_voxel_[1] = camera_voxel_[2] = 0.0;
    subpixel_refused_ = 0;
    subpixel_evicted_ = 0;
    subpixel_said_ = 0;
    WS_LOG_INFO("pool",
                "R2b sub-pixel residency is {} (pixel angle {:.5f} rad, {} level{} of slack); a node "
                "finer than the marcher's own descent target is {}",
                subpixel_rule_ ? "ON" : "OFF -- control arm", pixel_angle_, subpixel_margin_,
                subpixel_margin_ == 1 ? "" : "s",
                subpixel_rule_ ? "refused and given up" : "built and kept, as before");

    // R8b's arm, on the same terms and for the same reason.
    hashed_variation_ = budget_.hashed_variation;
    variation_seed_ = budget_.variation_seed;
    variation_amount_ = variation_amount_q(budget_.variation_colour);
    variation_said_ = 0;
    WS_LOG_INFO("pool",
                "R8b hashed variation is {} (seed {:#010x}, colour +/-{}/256 of full scale); a node "
                "with no field behind it gets its children {}",
                hashed_variation_ ? "ON" : "OFF -- control arm", variation_seed_, variation_amount_,
                hashed_variation_ ? "from the hash of its own key" : "from nowhere, as before");
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

// One place, so that "the pool had nowhere to put it" is one fact with one counter rather than
// four assignments to a flag nobody printed. See NodeUploadBatch::no_room.
void NodePool::note_no_room() {
    out_of_room_ = true;
    ++batch_.no_room;
    batch_.out_of_memory = true;
}

u32 NodePool::allocate_node() {
    if (!free_singles_.empty()) {
        const u32 slot = free_singles_.back();
        free_singles_.pop_back();
        nodes_[slot] = GpuNode{};
        dirty_nodes_.mark(slot);
        node_last_read_[slot] = static_cast<u32>(touch_frame_);
        node_reads_[slot] = 0;
        return slot;
    }
    if (next_free_ >= budget_.max_nodes) {
        note_no_room();
        return kNoNode;
    }
    const u32 slot = next_free_++;
    nodes_[slot] = GpuNode{};
    dirty_nodes_.mark(slot);
    node_last_read_[slot] = static_cast<u32>(touch_frame_);
    node_reads_[slot] = 0;
    return slot;
}

u32 NodePool::allocate_children() {
    if (!free_runs_.empty()) {
        const u32 base = free_runs_.back();
        free_runs_.pop_back();
        for (u32 i = 0; i < 8; ++i) nodes_[base + i] = GpuNode{};
        dirty_nodes_.mark_range(base, 8);
        for (u32 i = 0; i < 8; ++i) node_last_read_[base + i] = static_cast<u32>(touch_frame_);
        for (u32 i = 0; i < 8; ++i) node_reads_[base + i] = 0;
        return base;
    }
    if (next_free_ + 8 > budget_.max_nodes) {
        note_no_room();
        return kNoNode;
    }
    const u32 base = next_free_;
    next_free_ += 8;
    for (u32 i = 0; i < 8; ++i) nodes_[base + i] = GpuNode{};
    dirty_nodes_.mark_range(base, 8);
    // Born fresh, not born stale. Left at nought, every node built after frame `cold_frames` is
    // already older than the threshold and is evicted before a ray has had the chance to read it
    // once -- built and thrown away in the same breath, for ever.
    for (u32 i = 0; i < 8; ++i) node_last_read_[base + i] = static_cast<u32>(touch_frame_);
    for (u32 i = 0; i < 8; ++i) node_reads_[base + i] = 0;
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

// ---- R8b: the hashed child source ---------------------------------------------------------------
//
// Four functions and one chain. The chain is the whole of it: a hash of the voxel a descent starts
// in, then one mix per octant on the way down, with the colour perturbed at every step from the
// hash of the step. There is no coordinate below a voxel anywhere in here, which is what makes the
// depth unbounded rather than large.
//
// Everything in this block is a QUERY. Nothing it computes is stored, nothing it computes is
// uploaded, and with `--hashed-variation` off the chain is not walked at all -- the colour of a
// sub-voxel cell is then its voxel's own, which is a stand-in parent and exactly what a build
// without this source draws. That is the control arm, and it is one binary.

namespace {

// A 64-bit fold, for the fingerprint and for nothing else. Deliberately not the 32-bit mix the
// source itself uses: a fingerprint that shares arithmetic with the thing it is fingerprinting can
// agree with it for the wrong reason.
constexpr u64 fold64(u64 h, u64 value) {
    h ^= value + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    return h;
}

}  // namespace

// The depth-0 sample: a voxel's own type, its own colour, and the seed its children come from.
//
// Alpha is 255 rather than a folded coverage, and the difference matters. A node's `colour` alpha
// is coverage summarised over a subtree (D139); a VOXEL is either there or it is not, and the cell
// this source is about to derive is a solid piece of a solid voxel.
VariationSample NodePool::variation_at_voxel(VoxelTypeId type, i64 x, i64 y, i64 z) const {
    VariationSample out{};
    out.type = type;
    out.matter = (type != kAir);
    out.hash = variation_root_hash(x, y, z, variation_seed_);
    if (!out.matter || types_ == nullptr) return out;
    const VisualRecord& visual = types_->visual_of(type);
    out.colour = (255u << 24) | (static_cast<u32>(visual.blue) << 16) |
                 (static_cast<u32>(visual.green) << 8) | static_cast<u32>(visual.red);
    return out;
}

VariationSample NodePool::mirror_variation(i64 sx, i64 sy, i64 sz, u32 depth) const {
    // Which voxel the cell is in. An arithmetic shift, not a division, for the reason
    // `node_key_of` states: a cell at -1 belongs to voxel -1, not to voxel 0, and getting it wrong
    // puts every negative coordinate one voxel out.
    const i64 vx = sx >> depth;
    const i64 vy = sy >> depth;
    const i64 vz = sz >> depth;

    // Read the voxel the way the shader reads it -- entry hash, descent, child mask, leaf payload.
    // This is `mirror_voxel` and not a second reader: if the two ever disagreed, the mirror would
    // be agreeing with itself about a walk nobody had checked.
    VariationSample out = variation_at_voxel(mirror_voxel(vx, vy, vz), vx, vy, vz);
    out.depth = depth;
    if (!out.matter || depth == 0) return out;

    // The control arm stops here: with the source off there are no children below a voxel, and the
    // voxel itself is what stands in for the cell. Same shape, no grain.
    if (!hashed_variation_) return out;

    for (u32 step = depth; step > 0; --step) {
        const u32 octant = octant_of(sx >> (step - 1), sy >> (step - 1), sz >> (step - 1));
        out.hash = variation_child_hash(out.hash, octant);
        out.colour = variation_child_colour(out.colour, out.hash, variation_amount_);
    }
    return out;
}

VariationChildren NodePool::variation_children_of(i64 sx, i64 sy, i64 sz, u32 depth) const {
    const VariationSample at = mirror_variation(sx, sy, sz, depth);
    return variation_children(at.hash, at.colour, at.matter,
                              hashed_variation_ ? variation_amount_ : 0u);
}

u64 NodePool::variation_fingerprint(const World& world, const i64 lo[3], const i64 hi[3],
                                    i64 stride, u32 depth, u64* cells_out) const {
    if (stride < 1) stride = 1;
    const u32 amount = hashed_variation_ ? variation_amount_ : 0u;

    // The question is folded in first, so a hash cannot be compared against one taken over a
    // different box, a different stride or a different seed without the difference showing.
    u64 h = 0xCBF29CE484222325ull;
    for (u32 axis = 0; axis < 3; ++axis) {
        h = fold64(h, static_cast<u64>(lo[axis]));
        h = fold64(h, static_cast<u64>(hi[axis]));
    }
    h = fold64(h, static_cast<u64>(stride));
    h = fold64(h, depth);
    h = fold64(h, variation_seed_);

    u64 cells = 0;
    for (i64 z = lo[2]; z <= hi[2]; z += stride) {
        for (i64 y = lo[1]; y <= hi[1]; y += stride) {
            for (i64 x = lo[0]; x <= hi[0]; x += stride) {
                // The WORLD, not the pool. Residency is a race between two runs and world content
                // is not, so a fingerprint read off the pool would differ for reasons that have
                // nothing to do with the hash -- which is the whole thing this number is for.
                const VoxelTypeId type = world.get(x, y, z);
                if (type == kAir) continue;
                ++cells;

                VariationSample at = variation_at_voxel(type, x, y, z);
                h = fold64(h, at.hash);
                h = fold64(h, at.colour);
                for (u32 level = 0; level < depth; ++level) {
                    const VariationChildren kids =
                        variation_children(at.hash, at.colour, true, amount);
                    h = fold64(h, kids.mask);
                    for (u32 octant = 0; octant < 8; ++octant) {
                        h = fold64(h, kids.colour[octant]);
                        h = fold64(h, kids.hash[octant]);
                    }
                    // One fixed path down, so the cost is `depth * 8` per voxel rather than
                    // `8^depth`. Which path does not matter and that it is FIXED does.
                    const u32 pick = (level * 3u + 1u) & 7u;
                    at.hash = kids.hash[pick];
                    at.colour = kids.colour[pick];
                }
            }
        }
    }
    h = fold64(h, cells);
    if (cells_out != nullptr) *cells_out = cells;
    return h;
}

u32 NodePool::stale_leaves(const World& world, NodeKey* first) const {
    u32 stale = 0;
    for (u32 slot = 0; slot < next_free_; ++slot) {
        const GpuNode& node = nodes_[slot];
        // A free slot is reset to GpuNode{}, whose level is nought, so this is also the live test.
        if (node_level(node) != kLeafLevel || (node_flags(node) & kNodeLeaf) == 0) continue;
        const u32 leaf = node.children;
        if (leaf == kNoNode || leaf >= leaves_.size()) continue;

        const i64 vx = static_cast<i64>(node.x) << kLeafLevel;
        const i64 vy = static_cast<i64>(node.y) << kLeafLevel;
        const i64 vz = static_cast<i64>(node.z) << kLeafLevel;
        const Chunk* chunk = world.chunk(chunk_coord_of(vx, vy, vz));
        const Brick* brick =
            (chunk == nullptr) ? nullptr
                               : chunk->brick(static_cast<u32>((vx >> kLeafLevel) & 31),
                                              static_cast<u32>((vy >> kLeafLevel) & 31),
                                              static_cast<u32>((vz >> kLeafLevel) & 31));

        bool disagrees = false;
        if (brick == nullptr) {
            // The world has given the brick up and the pool is still drawing it. This is the shape
            // of D357-D361 arriving through a writer instead of through an edit.
            disagrees = true;
        } else {
            u64 bits[kBrickWords]{};
            brick->occupancy(bits);
            for (u32 word = 0; word < kBrickWords; ++word) {
                if (occupancy_[static_cast<usize>(leaf) * kBrickWords + word] != bits[word]) {
                    disagrees = true;
                    break;
                }
            }
        }
        if (!disagrees) continue;
        if (stale == 0 && first != nullptr) {
            *first = NodeKey{node.x, node.y, node.z, kLeafLevel};
        }
        ++stale;
    }
    return stale;
}

u32 NodePool::stale_masks(const World& world, NodeKey* first) const {
    u32 stale = 0;
    for (u32 slot = 0; slot < next_free_; ++slot) {
        const GpuNode& node = nodes_[slot];
        const u32 level = node_level(node);
        // A free slot resets to level nought, and a brick's mask describes voxels rather than
        // child nodes -- `stale_leaves` is what covers those.
        if (level <= kLeafLevel) continue;
        if ((node_flags(node) & kNodeLeaf) != 0) continue;

        const NodeKey key{node.x, node.y, node.z, level};
        u32 want = 0;
        for (u32 octant = 0; octant < 8; ++octant) {
            const NodeKey child{(key.x << 1) | static_cast<i64>(octant & 1),
                                (key.y << 1) | static_cast<i64>((octant >> 1) & 1),
                                (key.z << 1) | static_cast<i64>((octant >> 2) & 1), level - 1};
            if (world_has(world, child)) want |= (1u << octant);
        }
        if (node_child_mask(node) == want) continue;
        if (stale == 0 && first != nullptr) *first = key;
        ++stale;
    }
    return stale;
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
        note_no_room();
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
            note_no_room();
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
    //
    // R5d reads it now, which it did not when this was written: `visibility.comp` casts a second
    // march past any coarse node whose byte is under full and the composite draws the two surfaces
    // in proportion (D662). So this loop is on the frame's critical path for how a silhouette
    // LOOKS, not only for how it is summarised — and `fold_children` below is where its accuracy
    // stops, because that folds by taking a maximum rather than by projecting.
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
        //
        // **This is where R5d's edge anti-aliasing stops being able to help, and it is worth
        // knowing before wondering why** (D662). A maximum is not a projection: four children of
        // which two are solid wall and two are air have a true projected coverage of a half, and
        // this hands back the solid one's 255. So a thin railing keeps its low byte all the way up
        // and gets blended, while the silhouette of a solid wall reads as fully covered at every
        // level above the brick and gets nothing. The exact fold is available and is not large —
        // each quadrant of the parent's projection is the max of the TWO children in line along the
        // axis, and the parent is the mean of the four quadrants — but it changes what the byte
        // means, so it belongs with a measurement rather than beside this comment.
        // `tests/test_node_pool.cpp` pins the behaviour as it is, both halves of it.
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

    // R2b: the finest level anything at this node's distance can address. Computed once for the
    // whole descent rather than per level -- every node on the chain contains the requested one, so
    // its box is at least as near, and a per-level answer would only ever be coarser.
    const u32 finest = subpixel_rule_ ? subpixel_finest_for(key) : kLeafLevel;
    // True when the descent stopped because of the rule rather than because it arrived, which is
    // the one case whose fold has to be skipped. See below, and `fold_children`.
    bool stopped_subpixel = false;

    for (u32 level = kEntryLevel; level > key.level; --level) {
        const GpuNode& node = nodes_[slot];
        if ((node_flags(node) & kNodeLeaf) != 0) break;

        // R2b's second half, and the whole of "never STORED".
        //
        // The child about to be built is one level finer than `level`. If that is finer than the
        // marcher's own descent target here, no ray will ever address it -- `node_march` clamps its
        // target with `max(level, kLeafLevel)` where `level` is `floor(log2(footprint)) + dither`
        // and the dither is never negative. So the node would be built, folded, uploaded, swept
        // past and never read.
        //
        // The colour test is not a hedge; it is the condition that makes this safe, and it is the
        // sentence D259 blocked this on read the other way round. A node stands in for its subtree
        // by drawing the colour it was FOLDED from, and a node that has never had a built child has
        // no colour -- so refusing its first descent would leave a shell with nought alpha, which
        // `node_march` correctly refuses to draw and correctly goes on reporting, for ever. Build it
        // once, let the fold run up the chain, and refuse every request after that. What eviction
        // "can afford to rebuild" turns out not to be the question: the pool never has to, because
        // the marcher stops here and reads this node's own colour as an ordinary coarse hit.
        if (subpixel_rule_ && level - 1 < finest && (nodes_[slot].colour >> 24) != 0) {
            ++batch_.refused_subpixel;
            ++subpixel_refused_;
            stopped_subpixel = true;
            break;
        }

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
            node_last_read_[child] = static_cast<u32>(touch_frame_);
            free_singles_.push_back(built);
        }

        slot = child;
        if (depth <= kMaxNodeLevel) chain[depth++] = slot;
    }

    // Back up the chain, so a node that gained a child gains its share of the colour with it.
    // Folding on the way down would fold from children that did not exist yet.
    //
    // And STAMPED on the way up, which is a residency fact rather than a colour one and is the
    // other half of what D247 settled. `node_last_read_` was written in exactly one place a ray
    // could reach -- `touch_slot`, from the slot a ray STOPPED on -- so a node that is wanted by
    // something other than a stopping ray aged out on a six-hundred-frame timer while it was being
    // asked for. The proximity radius is the case that makes it plain: twenty metres of world is
    // pushed through `request` every frame precisely so that collision, physics and editing can
    // touch what is behind you (D199), every one of those requests lands here, and every one of
    // those nodes was evicted anyway and rebuilt on the next sweep. A node the pool has been told
    // to keep must not be a candidate for eviction.
    //
    // It costs nothing: the chain is already in hand, and the array is the one the erode sweep
    // reads first precisely because it is four bytes and sequential.
    //
    // R2b adds one exception and it is load-bearing rather than tidy. `fold_children` sets a node's
    // colour to nought when none of its children are built, which is right for a node that has never
    // had any and WRONG for one whose children have been given up: the moment the rule takes a
    // subtree, the node standing in for it is the thing being drawn, and re-folding it from eight
    // empty slots paints it black. So the node the rule stopped at keeps the colour it already has.
    // Its ancestors still fold, and they read that colour, which is intact -- nothing above it
    // learns anything wrong.
    //
    // It is still STAMPED, because a node the marcher is drawing is a node in use, and letting the
    // erosion sweep age it out would be D247 arriving through a new door.
    const u32 fold_from = (stopped_subpixel && depth > 0) ? depth - 1 : depth;
    for (u32 i = depth; i > 0; --i) {
        if (i <= fold_from) fold_children(chain[i - 1]);
        node_last_read_[chain[i - 1]] = static_cast<u32>(touch_frame_);
    }
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

    // The instrument, here rather than at the two eviction sites, because this is the single place
    // a node's contents are given up and a root shedding its subtree frees a whole tree of them
    // through one call. Gated on `evicting_` so the edit path -- which calls this to re-derive a
    // brick in place (D422) -- is not counted as the pool throwing something away. D426.
    if (evicting_) {
        ++batch_.evicted_nodes;
        if (in_view(node)) {
            ++batch_.evicted_on_screen;
            ++evictions_on_screen_;
        }
        note_eviction(node, touch_frame_, slot);
    }

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
    } else {
        release_children(slot);
    }

    node = GpuNode{};
    dirty_nodes_.mark(slot);
    // Stamped so the empty slot is not a candidate for ever. Left with its old timestamp it reads
    // as cold on every sweep, and the whole point of testing the timestamp first is that a slot
    // which is not cold costs nothing but the timestamp.
    node_last_read_[slot] = static_cast<u32>(touch_frame_);
    if (slot < node_reads_.size()) node_reads_[slot] = 0;
    ++evictions_;
}

// Sheds the subtree and keeps the node. What is left is exactly what `build_shell` makes: a node
// at its own level, with its coordinates and its child mask, and no children. Both sides read that
// as WANTED rather than EMPTY, so a ray that crosses it stops instead of passing through, and a
// later request rebuilds it in place — `refine` allocates a run whenever `children` is kNoNode.
//
// It does not count an eviction: each child that was actually freed counted itself inside
// `release_contents`, and the node this is called on is still standing.
void NodePool::release_children(u32 slot) {
    GpuNode& node = nodes_[slot];
    // A leaf's `children` is an index into `leaves_`, not a run, and it has no subtree to shed.
    if ((node_flags(node) & kNodeLeaf) != 0 || node.children == kNoNode) return;

    for (u32 octant = 0; octant < 8; ++octant) {
        const u32 child = node.children + octant;
        // A mask bit with a level of nought is a child the world has and the pool never built.
        // There is nothing under it to give back.
        if (node_level(nodes_[child]) == 0) continue;
        release_contents(child);
        nodes_[child] = GpuNode{};
        dirty_nodes_.mark(child);
    }
    free_runs_.push_back(node.children);

    node.children = kNoNode;
    dirty_nodes_.mark(slot);
    node_last_read_[slot] = static_cast<u32>(touch_frame_);
}

// A root: its slot came from the singles list, so that is where it goes back.
void NodePool::release(u32 slot) {
    if (slot == kNoNode || slot >= nodes_.size()) return;
    release_contents(slot);
    free_singles_.push_back(slot);
}

// ---- the frame -------------------------------------------------------------------------------

void NodePool::request(const NodeKey& key, u8 source) {
    ++requests_;
    requested_.push_back(key);
    requested_source_.push_back(source);
}

// Read by a ray, so it is wanted. Deliberately NOT a request: there is nothing to build, and
// putting it through `requested_` would make every visible root re-descend its whole path every
// frame for a tree that is already complete.
void NodePool::touch_slot(u32 slot) {
    if (slot < node_last_read_.size()) node_last_read_[slot] = static_cast<u32>(touch_frame_);
    // Instrument, and the ONLY place it is written: this array counts what a ray said, where
    // `node_last_read_` above also carries builds and the proximity sweep. D426.
    if (slot < node_reads_.size() && node_reads_[slot] != 0xFFFFFFFFu) ++node_reads_[slot];
}

// ---- R2b's second half: the rule ----------------------------------------------------------------
//
// Three small functions and one distance, kept together because the whole of the policy is here and
// because the trap this stage has is two copies of one piece of arithmetic drifting apart. D674 was
// an evening spent on a rule that was working, read through a line that could not tell "correct"
// from "broken"; the defence is that there is exactly one distance test and everything asks it.

// From the camera to the nearest point of a node's box, in voxels.
//
// Nearest rather than centre, and the direction is the safety. A ray that reaches any part of this
// node has travelled at least this far, so its footprint is at least `this * pixel_angle` and its
// descent target is at least the level that implies. Measuring to the centre would let a node be
// declared unaddressable while rays are addressing its near face.
f64 NodePool::distance_to_box(i64 x, i64 y, i64 z, u32 level) const {
    if (!camera_known_) return 0.0;
    const i64 span = i64{1} << level;
    const i64 lo[3] = {x << level, y << level, z << level};
    f64 sum = 0.0;
    for (u32 axis = 0; axis < 3; ++axis) {
        const f64 low = static_cast<f64>(lo[axis]);
        const f64 high = static_cast<f64>(lo[axis] + span);
        const f64 at = camera_voxel_[axis];
        const f64 gap = (at < low) ? (low - at) : ((at > high) ? (at - high) : 0.0);
        sum += gap * gap;
    }
    return std::sqrt(sum);
}

u32 NodePool::subpixel_finest_for(const NodeKey& key) const {
    const u32 finest = subpixel_finest_level(distance_to_box(key.x, key.y, key.z, key.level),
                                             pixel_angle_);
    // The margin is slack in the KEEP direction -- more levels held, not fewer -- so it can only
    // ever make the rule quieter. A margin that could evict more would be a second policy.
    return (finest > kLeafLevel + subpixel_margin_) ? finest - subpixel_margin_ : kLeafLevel;
}

bool NodePool::node_is_subpixel(const GpuNode& node) const {
    if (!subpixel_rule_ || !camera_known_) return false;
    const u32 level = node_level(node);
    // A free slot, a root the entry table owns, and anything at or above the marcher's coarsest
    // march level are all off limits: the first is not a node, and the other two are what a descent
    // has to pass THROUGH to reach anything at all.
    if (level == 0 || level >= kEntryLevel || level >= kMarcherMaxDetail) return false;
    return level < subpixel_finest_for(NodeKey{node.x, node.y, node.z, level});
}

// ---- the eviction instrument -------------------------------------------------------------------
//
// D425 left the standing-still half of the flicker measured and unexplained, and named this as the
// first thing to build: a count of evictions of nodes that were on screen when they went. Nothing
// here decides anything -- the policy is untouched -- because the point is to price two candidate
// fixes that cost very differently, and a measurement taken from the same signal the policy uses
// would agree with the policy however wrong the policy was.
//
// Five planes and no far plane: `lens.y` is 125 km and is past anything the world contains, so a
// far test would reject nothing and cost a plane. The near plane is the camera itself, which is
// what `forward` gives.
bool NodePool::in_view(const GpuNode& node) const {
    if (!view_.valid) return false;
    const u32 level = node_level(node);
    if (level == 0 || level > kMaxNodeLevel) return false;

    // The node's box in absolute voxels, moved into camera-relative space. f64 throughout: a
    // level-14 coordinate times 16,384 is past a float's exact integer range and the whole test
    // is a set of sign comparisons that must not wobble.
    const f64 size = static_cast<f64>(1ull << level);
    const f64 lo[3] = {static_cast<f64>(node.x) * size - view_.origin[0],
                       static_cast<f64>(node.y) * size - view_.origin[1],
                       static_cast<f64>(node.z) * size - view_.origin[2]};
    const f64 hi[3] = {lo[0] + size, lo[1] + size, lo[2] + size};

    // The four side planes, derived from the ray the marcher would build at each edge of the
    // screen: dir = forward + right * uv.x * tan * aspect - up * uv.y * tan, with uv in [-1, 1].
    // A plane through the camera containing the left edge has inward normal tan*aspect*forward +
    // right, and so round. Unnormalised, because only the sign is read.
    const f64 tx = static_cast<f64>(view_.tan_half_fov) * static_cast<f64>(view_.aspect);
    const f64 ty = static_cast<f64>(view_.tan_half_fov);
    const f64 f[3] = {view_.forward[0], view_.forward[1], view_.forward[2]};
    const f64 r[3] = {view_.right[0], view_.right[1], view_.right[2]};
    const f64 u[3] = {view_.up[0], view_.up[1], view_.up[2]};

    f64 planes[5][3];
    for (u32 axis = 0; axis < 3; ++axis) {
        planes[0][axis] = f[axis];                 // in front of the camera at all
        planes[1][axis] = tx * f[axis] + r[axis];  // left edge
        planes[2][axis] = tx * f[axis] - r[axis];  // right edge
        planes[3][axis] = ty * f[axis] + u[axis];  // bottom edge
        planes[4][axis] = ty * f[axis] - u[axis];  // top edge
    }

    // The corner furthest along each normal. If even that one is behind the plane, no part of the
    // box is in front of it and the box is out.
    for (const f64* normal : planes) {
        f64 best = 0.0;
        for (u32 axis = 0; axis < 3; ++axis) {
            best += normal[axis] * ((normal[axis] > 0.0) ? hi[axis] : lo[axis]);
        }
        if (best < 0.0) return false;
    }
    return true;
}

// Keyed by the coordinate as the RECORD carries it -- 32-bit, truncated at build time -- so the
// request side has to truncate the same way or the two never match. The request loop does.
//
// The fill is carried with it because it is the discriminator: a brick a ray stops on is a wall
// and is nearly solid, and a brick a ray passes through on the way to a wall is mostly air. Read
// here rather than at the match, because by then the leaf has been freed and reused.
void NodePool::note_eviction(const GpuNode& node, u64 frame, u32 slot) {
    const u32 level = node_level(node);
    if (level == 0) return;

    Evicted record;
    record.frame = static_cast<u32>(frame);
    record.never_read = (slot < node_reads_.size()) && node_reads_[slot] == 0;
    if (record.never_read) ++evicted_never_read_;
    if ((node_flags(node) & kNodeLeaf) != 0 && node.children != kNoNode &&
        node.children < leaves_.size()) {
        u32 bits = 0;
        const usize base = static_cast<usize>(node.children) * kBrickWords;
        for (u32 word = 0; word < kBrickWords; ++word) {
            bits += static_cast<u32>(std::popcount(occupancy_[base + word]));
        }
        record.fill = static_cast<u16>(bits);
        evicted_fill_sum_ += bits;
        ++evicted_leaves_;
    }
    evicted_at_[NodeKey{node.x, node.y, node.z, level}] = record;
}

void NodePool::touch(const NodeKey& key) {
    const u32 enter_level = std::max(key.level, kEntryLevel);
    const u32 up = enter_level - key.level;
    const NodeKey root{key.x >> up, key.y >> up, key.z >> up, enter_level};
    const auto it = live_.find(root);
    if (it != live_.end()) it->second.last_wanted = touch_frame_;
}

void NodePool::invalidate(i64 x, i64 y, i64 z) {
    // One brick, expressed as the box it occupies, so there is one path below this and not two.
    const NodeKey key = node_key_of(x, y, z, kLeafLevel);
    const i64 lo[3] = {key.x << kLeafLevel, key.y << kLeafLevel, key.z << kLeafLevel};
    const i64 hi[3] = {lo[0] + (i64{1} << kLeafLevel) - 1, lo[1] + (i64{1} << kLeafLevel) - 1,
                       lo[2] + (i64{1} << kLeafLevel) - 1};
    invalidate_box(lo, hi);
}

void NodePool::invalidate_box(const i64 lo[3], const i64 hi[3]) {
    EditBox box{};
    for (u32 axis = 0; axis < 3; ++axis) {
        box.lo[axis] = std::min(lo[axis], hi[axis]);
        box.hi[axis] = std::max(lo[axis], hi[axis]);
    }
    dirty_.push_back(box);
}

// One built node. Its children first, then its own mask and fold.
//
// Only nodes that EXIST are visited, which is the whole point: the box may span a million bricks
// and the pool may hold none of them, and what has to be refreshed is what it holds.
void NodePool::refresh_box(const World& world, u32 slot, const NodeKey& key, const i64 lo[3],
                           const i64 hi[3], u32& budget, EditRefresh& done) {
    ++done.visited;

    // A brick, which is dropped and re-derived from the world into the slot it already occupies.
    // The reasoning for re-deriving here rather than waiting for the feedback round trip is D422's
    // and is unchanged; only how this node was arrived at has changed.
    if (node_level(nodes_[slot]) == kLeafLevel) {
        release_contents(slot);
        nodes_[slot] = GpuNode{};
        dirty_nodes_.mark(slot);
        if (budget > 0) {
            const u32 rebuilt = build_leaf(world, key, budget);
            if (rebuilt != kNoNode) {
                nodes_[slot] = nodes_[rebuilt];
                nodes_[rebuilt] = GpuNode{};
                dirty_nodes_.mark(slot);
                dirty_nodes_.mark(rebuilt);
                node_last_read_[slot] = static_cast<u32>(touch_frame_);
                free_singles_.push_back(rebuilt);
                ++done.leaves_rebuilt;
            }
        }
        return;
    }

    const GpuNode& node = nodes_[slot];
    const bool has_children = (node_flags(node) & kNodeLeaf) == 0 && node.children != kNoNode &&
                              key.level > kLeafLevel;
    if (has_children) {
        const u32 child_level = key.level - 1;
        const u32 children = node.children;
        for (u32 octant = 0; octant < 8; ++octant) {
            const NodeKey child_key{(key.x << 1) | static_cast<i64>(octant & 1),
                                    (key.y << 1) | static_cast<i64>((octant >> 1) & 1),
                                    (key.z << 1) | static_cast<i64>((octant >> 2) & 1),
                                    child_level};
            const u32 child = children + octant;
            if (node_level(nodes_[child]) == 0) continue;   // not built below here

            // Prune. A child covers 2^level voxels from its origin, and anything that does not
            // reach into the edited box cannot have changed.
            bool overlaps = true;
            for (u32 axis = 0; axis < 3 && overlaps; ++axis) {
                const i64 origin = ((axis == 0)   ? child_key.x
                                    : (axis == 1) ? child_key.y
                                                  : child_key.z)
                                   << child_level;
                const i64 last = origin + (i64{1} << child_level) - 1;
                overlaps = origin <= hi[axis] && last >= lo[axis];
            }
            if (!overlaps) continue;

            refresh_box(world, child, child_key, lo, hi, budget, done);
        }
    }

    // The mask is re-derived from the WORLD rather than from the children, because the edit may
    // have emptied a region the pool never built -- and a bit left set over nothing is a ray
    // reporting a node the world does not have, every frame, for ever. That is D133.
    u32 mask = 0;
    for (u32 octant = 0; octant < 8; ++octant) {
        const NodeKey child{(key.x << 1) | static_cast<i64>(octant & 1),
                            (key.y << 1) | static_cast<i64>((octant >> 1) & 1),
                            (key.z << 1) | static_cast<i64>((octant >> 2) & 1), key.level - 1};
        if (world_has(world, child)) mask |= (1u << octant);
    }
    nodes_[slot].packed = pack_node(node_level(nodes_[slot]), node_flags(nodes_[slot]), mask);
    dirty_nodes_.mark(slot);
    fold_children(slot);
    ++done.folded;
}

const NodeUploadBatch& NodePool::update(const World& world, const f64 camera_voxel[3], u64 frame,
                                        const NodeView* view) {
    // What `touch` stamps. It is called from the feedback loop before this runs, so it needs the
    // frame from somewhere; keeping it here rather than passing it through every call site means
    // a hit reported this frame is stamped with the frame it was reported in.
    touch_frame_ = frame;
    // Instrument only. A caller that passes nothing gets a view that answers "no" to everything,
    // which is what the tests want and what makes this free where it is not asked for.
    view_ = (view != nullptr) ? *view : NodeView{};
    // R2b measures from here. The same array the proximity radius uses, so there is one answer in
    // this file to "where is the camera" and the rule cannot drift from the twenty metres that are
    // being held resident regardless of it.
    camera_voxel_[0] = camera_voxel[0];
    camera_voxel_[1] = camera_voxel[1];
    camera_voxel_[2] = camera_voxel[2];
    camera_known_ = true;
    // ...and how big a pixel is, from the caller when it knows and from the budget when it does
    // not. Taken per frame rather than at create because the render extent changes on a resize and
    // a rule measured against last frame's pixel is a rule measured against nothing.
    if (view != nullptr && view->valid && view->pixel_angle > 0.0f) {
        pixel_angle_ = static_cast<f64>(view->pixel_angle);
    }
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

    // Anything an edit touched is dropped so it is rebuilt from the world -- and ONLY that.
    //
    // This released the entry-level root, which is 512 m across, so chiselling one voxel threw
    // away everything within half a kilometre and rebuilt it at the rate pixels asked for it.
    // Measured by the test beside this: 37,497 nodes before a single-voxel edit and 0 after.
    // Editing is what this game is for, so the cost of an edit is the cost of playing it.
    //
    // Now it walks down to the brick that changed, drops that brick alone, and walks back up.
    // Two things have to happen on the way up and only one of them is the colour:
    //
    //   the MASK, re-derived from the world, because the edit may have emptied the brick -- and a
    //   bit left set over nothing is a ray reporting a node the world does not have, every frame,
    //   for ever, which is D133 exactly;
    //   the FOLD, because every ancestor's colour and coverage were averaged from what changed.
    //
    // The dropped brick is left at level nought rather than removed, which is this pool's
    // spelling of "the world has this and I have not built it" -- so the next ray that wants it
    // reports it and it returns on demand, at the level that ray asked for.
    if (!dirty_.empty()) {
        // Walked from the pool's own roots and pruned to each box, so what this costs is the number
        // of built nodes an edit reaches rather than the number of bricks its box contains. D515.
        //
        // The flat version took a list of every brick in the box, put it in a set to deduplicate
        // it, and descended to each one. It was written when the announcement was per brick and it
        // is the same algorithm as this one with the tree walked upwards instead of downwards --
        // but upwards means starting from bricks that mostly do not exist. On a 36-million-voxel
        // delete: 1,573,269 bricks announced, 13,325 nodes actually refreshed, no leaves rebuilt at
        // all, and **714 ms** to work that out -- 457 of it building the set and 257 walking it,
        // against 4 ms of the folding that is the entire purpose. Downwards, the pool visits what
        // it holds and nothing else.
        //
        // Post-order, which is what makes the sort unnecessary: a fold reads its children, so every
        // child is refreshed before the parent that averages it, by construction rather than by
        // ordering a list afterwards.
        const auto refresh_began = std::chrono::steady_clock::now();
        EditRefresh done;

        for (const EditBox& box : dirty_) {
            for (const std::pair<const NodeKey, Resident>& entry : live_) {
                const NodeKey& root_key = entry.first;
                if (root_key.level != kEntryLevel) continue;
                bool overlaps = true;
                for (u32 axis = 0; axis < 3 && overlaps; ++axis) {
                    const i64 origin = ((axis == 0)   ? root_key.x
                                        : (axis == 1) ? root_key.y
                                                      : root_key.z)
                                       << kEntryLevel;
                    const i64 last = origin + (i64{1} << kEntryLevel) - 1;
                    overlaps = origin <= box.hi[axis] && last >= box.lo[axis];
                }
                if (!overlaps) continue;
                refresh_box(world, entry.second.slot, root_key, box.lo, box.hi, budget, done);
            }
        }

        const f64 refresh_ms =
            std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - refresh_began)
                .count();
        // At a threshold, because an ordinary chisel comes through here every mouse-down frame and
        // costs a fraction of a millisecond. What this is for is the one that does not.
        if (refresh_ms > 5.0) {
            WS_LOG_INFO("pool",
                        "edit refresh: {:.0f} ms over {} announced {}, {} nodes visited, {} leaves "
                        "rebuilt, {} folded",
                        refresh_ms, dirty_.size(), dirty_.size() == 1 ? "box" : "boxes",
                        done.visited, done.leaves_rebuilt, done.folded);
        }
    }

    dirty_.clear();

    // The proximity radius, added as ordinary requests so there is one path that builds things
    // and one policy that decides what to build. Zero means off, and off has to mean nothing at
    // all: a loop that still runs once at radius zero holds the camera's own node resident for
    // ever, which is not "off" and took a failing eviction test to notice.
    // Twenty metres, at brick detail, whatever is on screen (D199).
    //
    // This asked for one node at the ENTRY level, because it stepped by 1 << kEntryLevel -- 512 m
    // -- over a range of 640 voxels, so the loop ran exactly once per axis. It was not holding
    // twenty metres of anything; it was making sure the root existed. Collision, physics and
    // editing all touch what is behind you and under your feet, and none of them can be served by
    // a 512 m shell.
    //
    // Asked of the WORLD rather than of the volume. Twenty metres is eighty bricks, so the cube
    // around the camera is 4.2 million of them and the overwhelming majority are air the world has
    // no record of; iterating what the world holds instead visits the handful that exist.
    //
    // Resumable and bounded, because even that is too much to do in one frame while somebody is
    // walking. The sweep carries a cursor and does a slice a frame, and restarts when the camera
    // crosses into a new brick -- so standing still finishes it and then costs nothing at all.
    if (budget_.proximity_voxels > 0) {
        const i64 near = budget_.proximity_voxels;
        // Anchored two metres, not a brick.
        //
        // The sweep restarts when the anchor moves, and a brick is twenty-five centimetres: a
        // player walking restarts it four times a metre and it never finishes, which is the exact
        // opposite of a guarantee. kProximityAnchorLevel is 64 voxels, so it restarts every two
        // metres and the radius is generous enough to cover the slack.
        const NodeKey here = node_key_of(static_cast<i64>(camera_voxel[0]),
                                         static_cast<i64>(camera_voxel[1]),
                                         static_cast<i64>(camera_voxel[2]),
                                         kProximityAnchorLevel);
        if (!(here == proximity_at_) || indexed_chunks_ != proximity_chunks_) {
            proximity_at_ = here;
            proximity_chunks_ = indexed_chunks_;
            proximity_cursor_ = 0;
            proximity_done_ = false;
        }

        if (!proximity_done_) {
            const i64 lo_x = static_cast<i64>(camera_voxel[0]) - near;
            const i64 lo_y = static_cast<i64>(camera_voxel[1]) - near;
            const i64 lo_z = static_cast<i64>(camera_voxel[2]) - near;
            const i64 hi_x = static_cast<i64>(camera_voxel[0]) + near;
            const i64 hi_y = static_cast<i64>(camera_voxel[1]) + near;
            const i64 hi_z = static_cast<i64>(camera_voxel[2]) + near;

            const i64 bx0 = lo_x >> kLeafLevel, bx1 = hi_x >> kLeafLevel;
            const i64 by0 = lo_y >> kLeafLevel, by1 = hi_y >> kLeafLevel;
            const i64 bz0 = lo_z >> kLeafLevel, bz1 = hi_z >> kLeafLevel;
            const i64 span_x = bx1 - bx0 + 1;
            const i64 span_y = by1 - by0 + 1;
            const i64 total = span_x * span_y * (bz1 - bz0 + 1);

            // The chunk the last brick was in, kept across the sweep.
            //
            // `world_has` looks the chunk up by hash for every cell, and the sweep walks x fastest
            // -- thirty-two consecutive bricks share a chunk, and a row of a hundred and sixty
            // crosses five. Looking it up once per brick was thirty-two thousand hash lookups a
            // frame and the whole of the 29.9 ms this sweep cost.
            ChunkCoord cached{0x7FFFFFFF, 0x7FFFFFFF, 0x7FFFFFFF};
            const Chunk* cached_chunk = nullptr;

            u32 served = 0;
            while (proximity_cursor_ < static_cast<u64>(total) &&
                   served < budget_.proximity_per_frame) {
                const i64 i = static_cast<i64>(proximity_cursor_++);
                const i64 bx = bx0 + (i % span_x);
                const i64 by = by0 + ((i / span_x) % span_y);
                const i64 bz = bz0 + (i / (span_x * span_y));
                ++served;

                // Only what the world actually holds, which is the whole reason this is
                // affordable: the volume is millions of cells and almost all of them are air the
                // world has no record of.
                const i64 vx = bx << kLeafLevel;
                const i64 vy = by << kLeafLevel;
                const i64 vz = bz << kLeafLevel;
                const ChunkCoord coord = chunk_coord_of(vx, vy, vz);
                if (!(coord == cached)) {
                    cached = coord;
                    cached_chunk = world.chunk(coord);
                }
                if (cached_chunk == nullptr) continue;
                if (cached_chunk->brick(static_cast<u32>((vx >> 3) & 31),
                                        static_cast<u32>((vy >> 3) & 31),
                                        static_cast<u32>((vz >> 3) & 31)) == nullptr) {
                    continue;
                }
                request(NodeKey{bx, by, bz, kLeafLevel}, kRequestProximity);
            }
            if (proximity_cursor_ >= static_cast<u64>(total)) proximity_done_ = true;
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
    for (usize asked = 0; asked < requested_.size(); ++asked) {
        const NodeKey& key = requested_[asked];
        if (!seen_requests_.insert(key).second) continue;
        const u8 source = (asked < requested_source_.size()) ? requested_source_[asked]
                                                             : static_cast<u8>(kRequestRay);

        // Instrument: is this a node this pool threw away a moment ago?
        //
        // Truncated to 32 bits to match what the record carried when it was evicted, and erased on
        // the match so one eviction can only be counted once however many rays report it.
        if (!evicted_at_.empty()) {
            const auto churn = evicted_at_.find(NodeKey{static_cast<i32>(key.x),
                                                        static_cast<i32>(key.y),
                                                        static_cast<i32>(key.z), key.level});
            if (churn != evicted_at_.end()) {
                if (static_cast<u32>(frame) - churn->second.frame <= kChurnWindow) {
                    ++batch_.churned;
                    ++churn_;
                    if (key.level < 32) ++churn_per_level_[key.level];
                    if (key.level == kLeafLevel) {
                        churn_fill_sum_ += churn->second.fill;
                        ++churn_leaves_;
                    }
                    if (churn->second.never_read) ++churn_never_read_;
                    if (source < kRequestSourceCount) ++churn_by_source_[source];
                }
                evicted_at_.erase(churn);
            }
        }

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
    requested_source_.clear();

    // The tree erodes from the leaves, so that memory follows the screen.
    //
    // Eviction below works at the entry level, and a root is 512 m: it can drop the whole scene or
    // nothing, which is why resident bytes were a high-water mark of everything ever looked at
    // rather than a function of what is on view (D260). This is the other end of the same policy.
    //
    // Only a node with no BUILT children is a candidate, so a parent is never dropped out from
    // under a child something is still reading. That makes the erosion one level a frame from the
    // bottom, which is the right shape: the deepest nodes are the ones a moving camera stops
    // needing first, and they are where the bytes are.
    //
    // What is left behind is a node at level nought, which is this pool's spelling of "the world
    // has this and I have not built it" -- the same state an edit leaves (D256) -- so a ray that
    // wants it again reports it and it returns at the level that ray asks for. The slot itself
    // stays in its parent's run of eight, because the run is the allocation unit and the subtree
    // underneath is where the memory actually was.
    // Repeated until nothing more is cold, because one pass only takes one level.
    //
    // A node is a candidate only once its children are gone, so a single sweep erodes exactly one
    // level: an eleven-level subtree that nothing reads would take eleven times `cold_frames` to
    // shed, which is twenty seconds at three hundred frames a second, and R2b asks for a ratio
    // rather than a direction. Repeating collapses a dead subtree in the frame it goes cold.
    //
    // Bounded by the depth of the tree by construction -- each pass removes a level, and there
    // are at most kEntryLevel of them -- and each pass over eighty thousand nodes is a scan of a
    // few hundred kilobytes, so the loop is cheap and its bound is not a guess.
    // A slice a frame, and the cheap test first.
    //
    // Two things made this the most expensive thing in the pool once everything else was fixed.
    // It read the 32-byte node record BEFORE the four-byte timestamp, so a steady frame touched
    // every node in the pool -- 268,761 records is 8.6 MB of memory traffic to discover that
    // nothing had gone cold. And it swept the whole pool every frame, when `cold_frames` is six
    // hundred and there is nothing to gain from asking more than a few times a second.
    //
    // So: the timestamp array is walked instead, which is one megabyte and sequential, and only a
    // slot that is actually cold costs a look at its record. And a slice at a time, so a steady
    // frame reads a hundred and twenty-eight kilobytes rather than nine megabytes. Eviction
    // latency rises by the number of slices, which against six hundred frames is nothing.
    //
    // The repeat stays, but within the slice: a node is only a candidate once its children are
    // gone, so one pass takes one level. Allocation is a bump pointer, so a subtree's nodes are
    // near each other and a slice usually holds the whole of one.
    if (next_free_ > 0) {
        const u32 cold = budget_.cold_frames;
        const u32 now = static_cast<u32>(frame);
        const u32 slice = (next_free_ + kErodeSlices - 1) / kErodeSlices;
        if (erode_cursor_ >= next_free_) erode_cursor_ = 0;
        const u32 first = erode_cursor_;
        const u32 last = (first + slice < next_free_) ? first + slice : next_free_;
        erode_cursor_ = last;

        for (u32 pass = 0; pass <= kEntryLevel; ++pass) {
            const u32 before_pass = batch_.evicted;
            for (u32 slot = first; slot < last; ++slot) {
                // The cheap one first. In a settled frame almost every slot stops here, and this
                // array is a quarter the width of a node record and read in order.
                //
                // R2b's second half is the one thing that cannot be answered from this array, and
                // that costs what it costs: the rule is about where a node IS, so it needs the
                // node's coordinates, which are in the 32-byte record. With the rule off the cheap
                // test still short-circuits, so the control arm reads exactly the memory it always
                // did and a timing difference between the arms is attributable to this line.
                const bool cold_enough = (now - node_last_read_[slot]) > cold;
                if (!cold_enough && !subpixel_rule_) continue;

                const GpuNode& node = nodes_[slot];
                const u32 level = node_level(node);
                if (level == 0 || level >= kEntryLevel) continue;   // unbuilt, or a root live_ owns

                // Age is one reason to give a node up and being unaddressable is another, and they
                // are not the same reason: a cold node MIGHT be wanted again the moment the camera
                // turns round, which is what `cold_frames` is six hundred frames long for, and a
                // sub-pixel node cannot be wanted at all until the camera moves closer -- at which
                // point the request that arrives is served by the ordinary build path.
                const bool subpixel = node_is_subpixel(node);
                if (!cold_enough && !subpixel) continue;

                if ((node_flags(node) & kNodeLeaf) == 0) {
                    if (node.children == kNoNode) continue;   // a shell already; nothing under it
                    bool any_built = false;
                    for (u32 octant = 0; octant < 8 && !any_built; ++octant) {
                        any_built = node_level(nodes_[node.children + octant]) != 0;
                    }
                    // Unchanged, and it is what keeps the sub-pixel arm safe as well: a node is
                    // only ever taken once nothing is left under it, so the pass below collapses a
                    // sub-pixel subtree from the leaves up rather than pulling a parent out from
                    // under a child a ray is still reading. Everything under a sub-pixel node is
                    // sub-pixel too -- it is finer and no further away -- so the repeat reaches all
                    // of it, and allocation being a bump pointer keeps a subtree inside one slice.
                    if (any_built) continue;   // something below is still in use
                }

                evicting_ = true;
                release_contents(slot);
                evicting_ = false;
                nodes_[slot] = GpuNode{};
                dirty_nodes_.mark(slot);
                ++batch_.evicted;
                // Attributed rather than inferred. `evicted` alone cannot say whether a frame gave
                // up four hundred nodes because the camera turned or because the rule reached a
                // subtree it had been holding since the load, and those are opposite readings of
                // the same number -- the first is churn to be explained, the second is the policy
                // working.
                if (subpixel && !cold_enough) {
                    ++batch_.evicted_subpixel;
                    ++subpixel_evicted_;
                }
            }
            if (batch_.evicted == before_pass) break;   // nothing both cold and childless left
        }

        // The churn table, swept when the erosion cursor wraps -- once every kErodeSlices frames.
        // Entries normally leave on the request that matches them; these are the ones nothing
        // asked for again, which is the answer the instrument wanted and is now noise.
        if (erode_cursor_ >= next_free_ && !evicted_at_.empty()) {
            for (auto it = evicted_at_.begin(); it != evicted_at_.end();) {
                it = (now - it->second.frame > kChurnWindow) ? evicted_at_.erase(it)
                                                            : std::next(it);
            }
        }
    }

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
    // Age decides, full stop, and it finally means something: a ray reading a node refreshes it
    // through `touch` (R2a), so a root that has gone cold is one nothing has looked at for
    // `cold_frames` -- which is what this loop always believed and was not true until D247.
    //
    // Not gated on memory pressure any more, and that gate was the bandaid rather than the
    // policy. While it was there nothing was ever evicted, so resident memory was a high-water
    // mark of everything ever asked for rather than a function of what is on screen -- and R2's
    // whole claim is that halving the resolution quarters the memory. Measured with the gate in
    // place: 6.04 MB at 1280x800 and 4.87 MB at 640x400, where a quarter is 1.51 MB.
    //
    // What makes this safe now is that a miss is no longer the only thing that says "wanted". If
    // that ever stops being true the symptom is the churn D247 describes, and the test beside
    // this one -- a root a ray keeps reading survives -- is what catches it.
    //
    // A cold root sheds its subtree and stays: the entry, the `live_` record and the node itself
    // are all kept, and only `children` goes. The memory is in the subtree, so shedding recovers
    // effectively all of it, and what is left says "something is here that I have not built" —
    // which is the truth, and which both a ray and `locate` already know how to answer.
    //
    // Removing the root instead said "nothing is here", and a shadow ray believed it. Occlusion
    // treats an unbuilt cell as opaque (D302) but an empty one as open sky, so a sealed room whose
    // roof had gone cold — the roof is never on screen, so it always goes cold — filled with
    // sunlight as the pool shed. Measured with the enclosed camera, frame 900 against frame 500:
    // four of eight roots gone, 1,163 faces reading fully lit where every correct answer is nought
    // (D324).
    //
    // Keeping the entry also repairs a latent fault of its own. The entry table is open addressing
    // with linear probing, and both `find` and `locate` stop at the first empty bucket; clearing a
    // cell in the middle of a probe run cuts every root behind it out of the table, which then
    // rebuilds as a second copy of a root that is already resident.
    for (auto& [key, resident] : live_) {
        (void)key;
        if (frame - resident.last_wanted <= budget_.cold_frames) continue;
        const u32 slot = resident.slot;
        if (slot == kNoNode || slot >= nodes_.size()) continue;
        if (nodes_[slot].children == kNoNode) continue;   // shed already; nothing under it
        evicting_ = true;
        release_children(slot);
        evicting_ = false;
        ++batch_.evicted;
    }

    // ---- R2b's own audit line ---------------------------------------------------------------
    //
    // The pool says this itself rather than waiting for the frame report to be taught about it, and
    // that is not impatience: the two arms of this change have to be readable from a `--settle` run
    // on a build whose `main.cpp` knows nothing about the rule, or the measurement cannot be taken
    // at all. `edit refresh` above is here for the same reason.
    //
    // Both arms print it. A control arm that says nothing is a control arm whose numbers have to be
    // inferred from a different instrument, and D621's leaf-ceiling table is what that costs.
    //
    // `held` is the walk, and it is the number that is not taken from the policy -- see `stats()`.
    // 600 frames is `cold_frames`, so one line is one full eviction window and the walk costs a
    // read of the node array once every ten seconds.
    if (frame >= subpixel_said_ + 600 && next_free_ > 0) {
        subpixel_said_ = frame;
        u32 held = 0;
        u64 held_bytes = 0;
        u32 held_leaves = 0;
        for (u32 slot = 0; slot < next_free_; ++slot) {
            const GpuNode& node = nodes_[slot];
            const u32 level = node_level(node);
            if (level == 0 || level >= kEntryLevel || level >= kMarcherMaxDetail) continue;
            if (level >= subpixel_finest_for(NodeKey{node.x, node.y, node.z, level})) continue;
            ++held;
            held_bytes += sizeof(GpuNode);
            if ((node_flags(node) & kNodeLeaf) != 0 && node.children != kNoNode &&
                node.children < leaves_.size()) {
                ++held_leaves;
                held_bytes += kBrickWords * sizeof(u64) + leaf_payload_size_[node.children];
            }
        }
        const NodePoolStats live = live_stats();
        // The live pixel angle, not the one `create` printed. `create` can only print the budget's
        // fallback -- there is no camera yet -- and the caller overrides it every frame from the
        // render extent it is actually drawing at. A line that reported the fallback would say
        // 1280x800's number on a 4K run, which is the exact confusion this whole rule turns on.
        WS_LOG_INFO("pool",
                    "R2b {} at frame {}: {} nodes, {} leaves held; sub-pixel STILL HELD {} ({} of "
                    "them leaves, {} bytes); lifetime {} refused, {} given up for being "
                    "unaddressable; pixel angle {:.6f} rad ({:.0f} lines){}",
                    subpixel_rule_ ? "ON" : "OFF", frame, live.nodes, live.leaves, held,
                    held_leaves, held_bytes, subpixel_refused_, subpixel_evicted_, pixel_angle_,
                    (pixel_angle_ > 0.0) ? 2.0 / pixel_angle_ : 0.0,
                    (pixel_angle_ == kSubPixelAngle) ? ", the FALLBACK -- nothing filled the view in"
                                                     : "");
    }

    // ---- R8b's own line, and it is the gate rather than an instrument ---------------------------
    //
    // "Same key, same children, for ever and on every machine" is a claim two runs can settle and
    // nothing else can, so the source prints the number those two runs are compared on. Off by
    // default with the flag, because it reads a quarter of a million voxels out of the world every
    // six hundred frames and a control arm that pays for an instrument it is not using is a control
    // arm with a limp.
    //
    // The box is FIXED and stated in the line. A fingerprint over whatever was resident, or over
    // whatever the camera happened to be near, would differ between two runs for reasons that have
    // nothing to do with the hash -- and a determinism gate that can fail for an unrelated reason
    // is a gate nobody will believe the second time it goes red.
    if (hashed_variation_ && frame >= variation_said_ + 600) {
        variation_said_ = frame;
        const i64 lo[3] = {-128, -128, -128};
        const i64 hi[3] = {127, 127, 127};
        u64 cells = 0;
        const u64 print = variation_fingerprint(world, lo, hi, 4, 3, &cells);
        WS_LOG_INFO("pool",
                    "R8b fingerprint {:016x} at frame {}: {} solid voxels in the 8 m cube at the "
                    "origin, stride 4, three levels below the voxel, seed {:#010x}",
                    print, frame, cells, variation_seed_);
    }

    return batch_;
}

NodePoolStats NodePool::live_stats() const {
    NodePoolStats s;
    s.nodes = next_free_ - static_cast<u32>(free_singles_.size()) -
              static_cast<u32>(free_runs_.size()) * 8;
    s.leaves = next_leaf_ - static_cast<u32>(free_leaves_.size());
    s.payload_in_use = payload_high_;
    s.payload_capacity = budget_.payload_bytes;
    s.node_bytes = static_cast<u64>(s.nodes) * sizeof(GpuNode);
    s.occupancy_bytes = static_cast<u64>(s.leaves) * kBrickWords * sizeof(u64);
    s.total_bytes = s.node_bytes + s.occupancy_bytes + s.payload_in_use +
                    entries_.size() * sizeof(u32);
    s.screen_bytes = s.node_bytes + s.occupancy_bytes + s.payload_in_use;
    s.builds = builds_;
    s.evictions = evictions_;
    s.evictions_on_screen = evictions_on_screen_;
    s.churn = churn_;
    s.evictions_never_read = evicted_never_read_;
    s.churn_never_read = churn_never_read_;
    for (u32 i = 0; i < kRequestSourceCount; ++i) s.churn_by_source[i] = churn_by_source_[i];
    for (u32 level = 0; level < 32; ++level) s.churn_per_level[level] = churn_per_level_[level];
    if (churn_leaves_ > 0) {
        s.churn_fill = static_cast<f64>(churn_fill_sum_) / static_cast<f64>(churn_leaves_);
    }
    if (evicted_leaves_ > 0) {
        s.evicted_fill = static_cast<f64>(evicted_fill_sum_) / static_cast<f64>(evicted_leaves_);
    }
    s.subpixel_refused = subpixel_refused_;
    s.subpixel_evicted = subpixel_evicted_;
    s.subpixel_rule = subpixel_rule_;
    s.requests = requests_;
    s.hits = hits_;
    return s;
}

NodePoolStats NodePool::stats() const {
    NodePoolStats s = live_stats();
    for (u32 slot = 0; slot < next_free_; ++slot) {
        const u32 level = node_level(nodes_[slot]);
        if (level != 0 && level < 32) ++s.per_level[level];

        // R2b's independent witness, and the reason it is HERE rather than beside the counters.
        //
        // `subpixel_refused` and `subpixel_evicted` are incremented by the rule as it works, so
        // they agree with the rule by construction -- a pool whose distance test is wrong reports
        // them just as briskly, and trap 26 is the session this project lost to three checks that
        // all agreed because all three read the same liar. This asks a different question of a
        // different set: of every node the pool is holding right now, how many does the rule say
        // nothing can address? With the rule on it must be nothing and stay nothing; with the rule
        // off it is what the rule would be worth, measured on a build that is not taking it.
        //
        // Deliberately NOT gated on `subpixel_rule_` -- the control arm has to be able to report
        // the size of the prize, or the two arms have nothing to compare.
        if (level == 0 || level >= kEntryLevel || level >= kMarcherMaxDetail) continue;
        const GpuNode& node = nodes_[slot];
        if (level >= subpixel_finest_for(NodeKey{node.x, node.y, node.z, level})) continue;
        ++s.subpixel_resident;
        if (level < 32) ++s.subpixel_per_level[level];
        s.subpixel_bytes += sizeof(GpuNode);
        if ((node_flags(node) & kNodeLeaf) != 0 && node.children != kNoNode &&
            node.children < leaves_.size()) {
            s.subpixel_bytes += kBrickWords * sizeof(u64) + leaf_payload_size_[node.children];
        }
    }
    // The baseline: every leaf the pool is holding right now. Walked rather than tracked, because
    // this is read once at an audit and tracking it would be a running total to keep in step with
    // every build and every free -- which is the shape of half the faults in this file.
    {
        u64 sum = 0;
        u64 count = 0;
        for (u32 slot = 0; slot < next_free_; ++slot) {
            const GpuNode& node = nodes_[slot];
            if ((node_flags(node) & kNodeLeaf) == 0 || node_level(node) == 0) continue;
            if (node.children == kNoNode || node.children >= leaves_.size()) continue;
            const usize base = static_cast<usize>(node.children) * kBrickWords;
            for (u32 word = 0; word < kBrickWords; ++word) {
                sum += static_cast<u64>(std::popcount(occupancy_[base + word]));
            }
            ++count;
        }
        if (count > 0) s.resident_fill = static_cast<f64>(sum) / static_cast<f64>(count);
    }
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
