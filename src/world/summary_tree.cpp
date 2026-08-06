#include "world/summary_tree.hpp"

#include "world/chunk.hpp"
#include "world/voxel_type.hpp"

namespace ws {

namespace {

i64 floor_div(i64 value, i64 divisor) {
    return (value >= 0) ? (value / divisor) : -(((-value) + divisor - 1) / divisor);
}

}  // namespace

void SummaryTree::create(const VoxelTypeTable& types) {
    types_ = &types;
    nodes_.clear();
}

void SummaryTree::clear() {
    nodes_.clear();
    occupied_.clear();
    indexed_ = false;
}

void SummaryTree::index_world(const World& world) {
    occupied_.clear();
    world.for_each_chunk([this](const ChunkCoord& coord, const Chunk& chunk) {
        if (chunk.empty()) return;
        for (u32 level = 0; level < kMaxSummaryLevels; ++level) {
            occupied_.insert(SummaryKey{level, block_of(coord, level)});
        }
    });
    indexed_ = true;
}

ChunkCoord SummaryTree::block_of(const ChunkCoord& chunk, u32 level) {
    const i64 span = summary_span(level);
    return ChunkCoord{floor_div(chunk.x, span), floor_div(chunk.y, span),
                      floor_div(chunk.z, span)};
}

void SummaryTree::invalidate(const ChunkCoord& chunk) {
    for (u32 level = 0; level < kMaxSummaryLevels; ++level) {
        const SummaryKey key{level, block_of(chunk, level)};
        nodes_.erase(key);
        // Also mark the path occupied, because this may be a chunk that did not exist when
        // the index was built — and an unindexed block is answered "empty" without looking,
        // so a newly built region would never appear. Marking a block that has since been
        // emptied is harmless: it costs one descent that finds nothing.
        occupied_.insert(key);
    }
}

const Thumbnail* SummaryTree::peek(u32 level, const ChunkCoord& block) const {
    const auto found = nodes_.find(SummaryKey{level, block});
    if (found == nodes_.end() || found->second.empty) return nullptr;
    return &found->second.summary;
}

const Thumbnail* SummaryTree::get(const World& world, u32 level, const ChunkCoord& block) {
    if (!indexed_) index_world(world);
    const Node* node = build(world, level, block);
    return (node != nullptr && !node->empty) ? &node->summary : nullptr;
}

const SummaryTree::Node* SummaryTree::build(const World& world, u32 level,
                                            const ChunkCoord& block) {
    if (level >= kMaxSummaryLevels || types_ == nullptr) return nullptr;

    const SummaryKey key{level, block};
    const auto found = nodes_.find(key);
    if (found != nodes_.end()) return &found->second;

    Node node;

    // Nothing here, and the index says so without looking. This is what bounds the whole
    // structure: a block with no chunks in it is answered in one hash lookup instead of
    // descending into eight children that will each do the same.
    if (occupied_.find(key) == occupied_.end()) {
        return &nodes_.emplace(key, std::move(node)).first->second;
    }

    if (level == 0) {
        const Chunk* chunk = world.chunk(block);
        if (chunk != nullptr && !chunk->empty()) {
            node.summary = build_thumbnail(*chunk, *types_);
            node.empty = node.summary.empty();
        }
        return &nodes_.emplace(key, std::move(node)).first->second;
    }

    // Eight children, each covering an octant of this block. A child's 8x8x8 cells fold into
    // the 4x4x4 cells of the octant it occupies, so every cell of every child contributes to
    // exactly one cell here — nothing is sampled and nothing is skipped.
    //
    // The children are built first and the map is only touched afterwards, because building
    // one can insert nodes and invalidate any reference into it.
    constexpr u32 kHalf = kThumbAxis / 2;
    Thumbnail folded;
    bool any = false;

    for (u32 octant = 0; octant < 8; ++octant) {
        const i64 ox = static_cast<i64>(octant & 1u);
        const i64 oy = static_cast<i64>((octant >> 1) & 1u);
        const i64 oz = static_cast<i64>((octant >> 2) & 1u);
        const ChunkCoord child{block.x * 2 + ox, block.y * 2 + oy, block.z * 2 + oz};

        const Node* built = build(world, level - 1, child);
        if (built == nullptr || built->empty) continue;
        any = true;

        // Copy, because the recursion below could rehash the map out from under a pointer.
        const Thumbnail source = built->summary;

        for (u32 cz = 0; cz < kHalf; ++cz) {
            for (u32 cy = 0; cy < kHalf; ++cy) {
                for (u32 cx = 0; cx < kHalf; ++cx) {
                    // Two cells of the child per cell here, on each axis.
                    u32 red = 0;
                    u32 green = 0;
                    u32 blue = 0;
                    u32 weight = 0;
                    u32 coverage = 0;
                    u32 occupied = 0;

                    for (u32 sz = 0; sz < 2; ++sz) {
                        for (u32 sy = 0; sy < 2; ++sy) {
                            for (u32 sx = 0; sx < 2; ++sx) {
                                const u32 cell =
                                    source.at(cx * 2 + sx, cy * 2 + sy, cz * 2 + sz);
                                if (cell == 0) continue;
                                const u32 alpha = cell >> 24;
                                coverage += alpha;
                                ++occupied;
                                const u32 w = alpha + 1;
                                red += (cell & 0xFFu) * w;
                                green += ((cell >> 8) & 0xFFu) * w;
                                blue += ((cell >> 16) & 0xFFu) * w;
                                weight += w;
                            }
                        }
                    }

                    if (weight == 0) continue;

                    // Coverage is the mean over all eight, empties included: a cell twice the
                    // size holding the same matter is half as full, which is exactly right.
                    u32 alpha = coverage / 8;

                    // And it never reaches zero while anything is there. This is the line
                    // that keeps a railing alive from one metre to a kilometre: halving eight
                    // times takes any real coverage below one, and zero means "empty", so
                    // without the floor a thin thing does not fade — it vanishes, at whatever
                    // distance its level happens to be chosen.
                    if (alpha == 0 && occupied > 0) alpha = 1;

                    const u32 index = thumb_index(cx + static_cast<u32>(ox) * kHalf,
                                                  cy + static_cast<u32>(oy) * kHalf,
                                                  cz + static_cast<u32>(oz) * kHalf);
                    folded.cells[index] = (red / weight) | ((green / weight) << 8) |
                                          ((blue / weight) << 16) | (alpha << 24);
                }
            }
        }
    }

    node.summary = folded;
    node.empty = !any;
    return &nodes_.emplace(key, std::move(node)).first->second;
}

}  // namespace ws
