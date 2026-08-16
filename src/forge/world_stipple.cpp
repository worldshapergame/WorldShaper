#include "forge/world_stipple.hpp"

#include <algorithm>
#include <map>

#include "core/jobs.hpp"
#include "game/clip.hpp"

namespace ws {
namespace forge {
namespace {

// How tall a slab of a chunk is, in voxels. See the header: this is a memory bound and not a
// balance one -- `parallel_for` hands out pieces in proportion to the range whatever the range is,
// so cutting a chunk into four does not change how evenly the work lands. What it changes is how
// much clip a worker is holding while it walks: 258 x 258 x 66 is 22 MB against a whole chunk's 86,
// and this runs on the machine somebody is playing on.
//
// A divisor of 256, so the slabs of a chunk tile it exactly and no interior cell is judged twice.
constexpr i64 kCleanSlab = 64;
static_assert(256 % kCleanSlab == 0, "the slabs of a chunk have to tile it exactly");

// A box of the world to be walked: the INTERIOR, inclusive. The skirt is added when it is captured.
struct CleanBox {
    i64 low[3]{0, 0, 0};
    i64 high[3]{0, 0, 0};
};

std::vector<CleanBox> boxes_of(const World& world) {
    std::vector<CleanBox> out;
    const std::vector<ChunkCoord> coords = world.sorted_chunk_coords();
    out.reserve(coords.size() * static_cast<usize>(256 / kCleanSlab));
    for (const ChunkCoord& coord : coords) {
        const i64 base[3] = {coord.x << 8, coord.y << 8, coord.z << 8};
        for (i64 slab = 0; slab < 256; slab += kCleanSlab) {
            CleanBox box;
            box.low[0] = base[0];
            box.low[1] = base[1];
            box.low[2] = base[2] + slab;
            box.high[0] = base[0] + 255;
            box.high[1] = base[1] + 255;
            box.high[2] = base[2] + slab + kCleanSlab - 1;
            out.push_back(box);
        }
    }
    return out;
}

// The six-neighbour test `paint_specks` and `despeckle` both apply, over the interior of a captured
// box, recording both of the things the two shipped passes each went and found separately.
//
// Written out here rather than shared with `stipple_counts` for the reason `stipple_counts` gives
// for not sharing with `paint_specks`: the only thing they have in common is the neighbour test,
// and every caller wants something different out of it. What this one owes is that its counts are
// the same counts `stipple_counts(box, 1)` would produce, which the suite asserts directly.
void scan_box(const Clip& box, const i64 origin[3], StippleCounts& counts,
              std::vector<WorldSpeck>& specks) {
    const auto at = [&](i32 x, i32 y, i32 z) -> VoxelTypeId {
        if (x < 0 || y < 0 || z < 0 || x >= box.size[0] || y >= box.size[1] ||
            z >= box.size[2]) {
            return kAir;
        }
        return box.at(x, y, z);
    };
    for (i32 z = 1; z < box.size[2] - 1; ++z) {
        for (i32 y = 1; y < box.size[1] - 1; ++y) {
            for (i32 x = 1; x < box.size[0] - 1; ++x) {
                const VoxelTypeId mine = box.at(x, y, z);
                if (mine == kAir) continue;
                const VoxelTypeId around[6]{at(x - 1, y, z), at(x + 1, y, z), at(x, y - 1, z),
                                            at(x, y + 1, z), at(x, y, z - 1), at(x, y, z + 1)};
                bool exposed = false;
                bool kin = false;
                for (const VoxelTypeId neighbour : around) {
                    if (neighbour == kAir) {
                        exposed = true;
                    } else if (neighbour == mine) {
                        kin = true;
                    }
                }
                if (!exposed) continue;   // buried, and nobody's business -- see paint_specks
                StippleCounts::Pair& pair = counts.by_type[mine];
                ++pair.surface;
                if (kin) continue;
                ++pair.specks;

                // The material most of the neighbours wear, with `despeckle`'s tie-break: first in
                // the fixed order above rather than whichever a map happens to iterate first,
                // because a world must clean identically every time on every machine.
                VoxelTypeId best = kAir;
                i32 best_count = 0;
                for (const VoxelTypeId neighbour : around) {
                    if (neighbour == kAir) continue;
                    i32 count = 0;
                    for (const VoxelTypeId other : around) {
                        if (other == neighbour) ++count;
                    }
                    if (count > best_count) {
                        best_count = count;
                        best = neighbour;
                    }
                }
                WorldSpeck one;
                one.at[0] = origin[0] + x;
                one.at[1] = origin[1] + y;
                one.at[2] = origin[2] + z;
                one.was = mine;
                // kAir here is a speck with no solid neighbour at all, which `despeckle` leaves
                // where it is. It is carried rather than dropped so that the two passes see the
                // same population and the counts below can be compared straight across.
                one.becomes = best;
                specks.push_back(one);
            }
        }
    }
}

std::vector<TypeShare> shares_of(const std::map<VoxelTypeId, u64>& tally) {
    std::vector<TypeShare> out;
    for (const auto& entry : tally) {
        TypeShare share;
        share.type = entry.first;
        share.count = entry.second;
        share.fraction = 0.0;
        out.push_back(share);
    }
    std::sort(out.begin(), out.end(),
              [](const TypeShare& a, const TypeShare& b) { return a.count > b.count; });
    return out;
}

}  // namespace

WorldStippleScan scan_world_specks(const World& world, JobSystem* jobs) {
    WorldStippleScan out;
    const std::vector<CleanBox> boxes = boxes_of(world);
    if (boxes.empty()) return out;

    // A slot per box, so no two threads ever touch one and the merge below is the only place
    // anything is combined. Sized before a worker runs, so nothing reallocates under one.
    std::vector<StippleCounts> counts(boxes.size());
    std::vector<std::vector<WorldSpeck>> found(boxes.size());
    std::vector<u64> cells(boxes.size(), 0);
    std::vector<u8> walked(boxes.size(), 0);

    const auto walk = [&](usize begin, usize end) {
        for (usize i = begin; i < end; ++i) {
            const CleanBox& box = boxes[i];
            // An interior with nothing in it has no surface and no specks, whatever its skirt
            // holds, so it can be skipped without capturing it at all. `any_matter_in` is answered
            // by brick pointer and is deliberately conservative -- it may say yes over an empty
            // brick and can never say no over a full one -- so skipping on a no is exact. The
            // world is mostly air and a capture is 22 MB of zeroing, so this is not a micro-
            // optimisation; it is most of the boxes on a building-shaped world.
            if (!any_matter_in(world, box.low, box.high)) continue;
            walked[i] = 1;
            const i64 origin[3] = {box.low[0] - 1, box.low[1] - 1, box.low[2] - 1};
            const Clip captured =
                capture_clip(world, origin[0], origin[1], origin[2], box.high[0] + 1,
                             box.high[1] + 1, box.high[2] + 1);
            cells[i] = captured.cell_count();
            scan_box(captured, origin, counts[i], found[i]);
        }
    };
    if (jobs != nullptr && boxes.size() > 1) {
        jobs->parallel_for(boxes.size(), 1, walk);
    } else {
        walk(0, boxes.size());
    }

    usize total_specks = 0;
    for (const std::vector<WorldSpeck>& one : found) total_specks += one.size();
    out.specks.reserve(total_specks);
    for (usize i = 0; i < boxes.size(); ++i) {
        out.counts.add(counts[i]);
        out.specks.insert(out.specks.end(), found[i].begin(), found[i].end());
        out.cells += cells[i];
        if (walked[i] != 0) {
            ++out.boxes;
        } else {
            ++out.boxes_skipped;
        }
    }
    return out;
}

WorldCleanReport repaint_world_specks(World& world, const WorldStippleScan& scan,
                                      const StippleVerdict& verdict) {
    WorldCleanReport out;
    // An empty verdict means "leave every speck alone, everywhere", which is `despeckle`'s own
    // reading of one and the fault D625 was. Nothing here invents a judgement of its own.
    if (verdict.allowed.empty()) return out;

    std::map<VoxelTypeId, u64> repainted;
    ChunkCoord last{};
    bool have_last = false;
    for (const WorldSpeck& one : scan.specks) {
        const auto may = verdict.allowed.find(one.was);
        if (may == verdict.allowed.end()) continue;   // this material has no specks anywhere
        if (!may->second) {
            ++out.left;   // a stipple, and it stays a stipple
            continue;
        }
        if (one.becomes == kAir) continue;   // no solid neighbour at all: leave it
        world.set(one.at[0], one.at[1], one.at[2], one.becomes);
        ++out.repainted;
        ++repainted[one.was];
        out.repaints.push_back(one);
        // A run-length dedup rather than a set, because the scan emits specks in box order and the
        // slabs of a chunk are consecutive in it -- so every speck of a chunk arrives in one run.
        // If that ordering ever stops holding, this reports a chunk twice, which costs an extra
        // announcement and nothing else.
        const ChunkCoord where = chunk_coord_of(one.at[0], one.at[1], one.at[2]);
        if (!have_last || !(where == last)) {
            out.changed.push_back(where);
            last = where;
            have_last = true;
        }
    }
    out.by_type = shares_of(repainted);
    return out;
}

// ------------------------------------------------------------------------------------------
// The control arm. Below this line is the pass exactly as D630 measured it, moved here from
// `Application` unchanged so that the two arms are two flags of one build (D407) and so that
// "the parallel one agrees with the serial one" is a thing a run can check rather than a claim.
// ------------------------------------------------------------------------------------------

// The stipple counts taken from the WORLD, chunk by chunk, each with a one-voxel skirt.
//
// R11d route 1b, and D628 is why it exists. The verdict is a ratio per material and both its counts
// are additive over disjoint boxes -- but only if the boxes know their neighbours. Summing the
// ladder's own per-node counts does not: `paint_specks` reads outside its clip as air, so every
// voxel on a node face is counted as surface and as a speck, and 296 of a leaf node's 512 cells are
// its own face. Measured, that changed the verdict on 11 of the facility's 35 materials and cleaned
// away two of its six deliberate dithers.
//
// A chunk captured with a one-voxel skirt fixes both halves at once: the skirt supplies REAL
// neighbours across the edge, and the interiors of the chunks tile the world exactly once, so the
// sum over them is the same population a whole-clip capture would have counted, voxel for voxel.
//
// Chunk by chunk because a whole-world capture is not affordable -- the facility's box is 1088 x 669
// x 800, which is 582 million cells and 2.3 GB of clip. One chunk with its skirt is 258 cubed.
StippleCounts stipple_counts_from_world_serial(const World& world, JobSystem* jobs) {
    StippleCounts total;
    const std::vector<ChunkCoord> coords = world.sorted_chunk_coords();
    if (coords.empty()) return total;

    std::vector<StippleCounts> per(coords.size());
    const auto take = [&](usize begin, usize end) {
        for (usize i = begin; i < end; ++i) {
            const i64 base[3] = {coords[i].x << 8, coords[i].y << 8, coords[i].z << 8};
            const Clip box = capture_clip(world, base[0] - 1, base[1] - 1, base[2] - 1,
                                          base[0] + 256, base[1] + 256, base[2] + 256);
            per[i] = stipple_counts(box, 1);
        }
    };
    if (jobs != nullptr && coords.size() > 1) {
        jobs->parallel_for(coords.size(), 1, take);
    } else {
        take(0, coords.size());
    }
    for (const StippleCounts& one : per) total.add(one);
    return total;
}

// Clean the WORLD, chunk by chunk, each with a one-voxel skirt.
//
// R11d option 2, chosen by the user after D629. The verdict is a property of the resolution the
// question is asked at -- metre 8 protects six of the facility's materials and the authored metre 32
// protects one -- and nothing in the world is ever built at metre 8. So the judge moves to where the
// voxels are: the verdict comes from the world and the cleaning happens on the world.
//
// That also removes despeckling's dependency on the up-front whole-clip sample, which is the one
// thing that blocked R11d. Nothing else was ever taken from it.
//
// The skirt is not a detail. Without it every chunk face is judged against air, which is D628 at a
// larger grain: a voxel on a chunk boundary would read as isolated because its own material carries
// on in the chunk next door. The interiors tile the world exactly once, so every voxel is judged
// once, by its real neighbours.
//
// Repainting is colour only -- no voxel is added or removed -- so `set` here cannot create or drop a
// brick, and the volume, the components and the walkability cannot move. That is `despeckle`'s own
// guarantee and it is what makes writing straight back into the world safe.
WorldCleanReport despeckle_world_serial(World& world, const StippleVerdict& verdict) {
    WorldCleanReport out;
    if (!verdict.any()) return out;
    const std::vector<ChunkCoord> coords = world.sorted_chunk_coords();
    if (coords.empty()) return out;

    std::map<VoxelTypeId, u64> repainted;
    for (const ChunkCoord& coord : coords) {
        const i64 base[3] = {coord.x << 8, coord.y << 8, coord.z << 8};
        Clip box = capture_clip(world, base[0] - 1, base[1] - 1, base[2] - 1, base[0] + 256,
                                base[1] + 256, base[2] + 256);
        const std::vector<VoxelTypeId> before = box.voxels;
        const DespeckleReport cleaned = despeckle(box, 0.05, &verdict, 1);
        out.left += cleaned.left;
        if (cleaned.repainted == 0) continue;
        // Only what changed, and only inside the skirt. Walking the whole chunk back into the world
        // would be sixteen million writes to change a few hundred voxels.
        for (i32 z = 1; z < box.size[2] - 1; ++z) {
            for (i32 y = 1; y < box.size[1] - 1; ++y) {
                for (i32 x = 1; x < box.size[0] - 1; ++x) {
                    const usize at = box.index(x, y, z);
                    if (box.voxels[at] == before[at]) continue;
                    WorldSpeck one;
                    one.at[0] = base[0] + x - 1;
                    one.at[1] = base[1] + y - 1;
                    one.at[2] = base[2] + z - 1;
                    one.was = before[at];
                    one.becomes = box.voxels[at];
                    world.set(one.at[0], one.at[1], one.at[2], one.becomes);
                    ++out.repainted;
                    ++repainted[one.was];
                    out.repaints.push_back(one);
                }
            }
        }
        out.changed.push_back(coord);
    }
    out.by_type = shares_of(repainted);
    return out;
}

}  // namespace forge
}  // namespace ws
