#include "forge/sample.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_map>

#include "core/jobs.hpp"

namespace ws {
namespace forge {

namespace {

// The voxel a metre coordinate falls in, rounded away from zero consistently so that a shape
// spanning the origin is not a voxel wider on one side than the other.
i64 voxel_floor(f64 metres, i32 per_metre) {
    return static_cast<i64>(std::floor(metres * static_cast<f64>(per_metre)));
}

}  // namespace

namespace {

u32 hash3(i64 x, i64 y, i64 z, u32 seed) {
    u32 h = static_cast<u32>(x) * 0x8da6b343u ^ static_cast<u32>(y) * 0xd8163841u ^
            static_cast<u32>(z) * 0xcb1ab31fu ^ seed;
    h ^= h >> 16; h *= 0x7feb352du;
    h ^= h >> 15; h *= 0x846ca68bu;
    h ^= h >> 16;
    return h;
}

// A signed value in [-1, 1] from a hash, so a voxel's perturbation is a property of where it is
// and not of when it was computed. The same clip built twice is the same clip.
f64 hash_signed(i64 x, i64 y, i64 z, u32 seed) {
    return static_cast<f64>(hash3(x, y, z, seed)) * (2.0 / 4294967296.0) - 1.0;
}

u8 nudge(u8 base, f64 amount, f64 signed_unit) {
    const f64 moved = static_cast<f64>(base) + signed_unit * amount * 255.0;
    return static_cast<u8>(std::clamp(moved, 0.0, 255.0));
}

}  // namespace

namespace {

// A slab's private table of perturbations.
//
// Open-addressed on the quantised key, because the map this replaced was doing sixty million
// red-black tree descents into a million-node tree and that alone was seventy seconds. The keys
// are already well mixed — they are a base type and four hashed channel values — so the probe
// sequence is linear and the load factor is kept under a half.
// Aligned to a cache line, and that is worth a sentence. These live in a vector, one per slab,
// and every slab writes to its own — but a std::vector header is about a hundred bytes, so two
// neighbouring tables would share a line and two threads would fight over it on every single
// insert. Sixty million inserts is a lot of fighting for a variable neither thread reads.
struct alignas(64) LocalTable {
    std::vector<u64> keys;        // 0 means empty; a real key always has the base id in it
    std::vector<u32> slots;
    std::vector<VisualRecord> records;
    std::vector<VoxelTypeId> bases;
    std::vector<u64> counts;      // voxels that landed on each slot, for the report
    u64 mask = 0;

    void reserve(usize expected) {
        usize capacity = 1024;
        while (capacity < expected * 2) capacity <<= 1;
        keys.assign(capacity, 0);
        slots.assign(capacity, 0);
        mask = capacity - 1;
    }

    void grow() {
        std::vector<u64> old_keys;
        std::vector<u32> old_slots;
        old_keys.swap(keys);
        old_slots.swap(slots);
        keys.assign(old_keys.size() * 2, 0);
        slots.assign(old_keys.size() * 2, 0);
        mask = keys.size() - 1;
        for (usize i = 0; i < old_keys.size(); ++i) {
            if (old_keys[i] == 0) continue;
            u64 at = (old_keys[i] * 0x9E3779B97F4A7C15ull) >> 32 & mask;
            while (keys[at] != 0) at = (at + 1) & mask;
            keys[at] = old_keys[i];
            slots[at] = old_slots[i];
        }
    }

    // The slot for this key, adding it when new.
    u32 slot_for(u64 key, const VisualRecord& record, VoxelTypeId base) {
        u64 at = (key * 0x9E3779B97F4A7C15ull) >> 32 & mask;
        while (keys[at] != 0 && keys[at] != key) at = (at + 1) & mask;
        if (keys[at] == key) {
            ++counts[slots[at]];
            return slots[at];
        }
        const u32 slot = static_cast<u32>(records.size());
        keys[at] = key;
        slots[at] = slot;
        records.push_back(record);
        bases.push_back(base);
        counts.push_back(1);
        if (records.size() * 2 > keys.size()) grow();
        return slot;
    }
};

}  // namespace

VariationReport apply_variation(Clip& clip, VoxelTypeTable& types, const Field& field,
                                const Variation& variation, const SampleSettings& settings,
                                const SampleResult& placed, JobSystem* jobs) {
    VariationReport report;
    if (clip.empty() || !variation.any()) return report;

    const i32 per_metre = (settings.voxels_per_metre > 0) ? settings.voxels_per_metre
                                                          : kVoxelsPerMetre;
    const f64 voxel = 1.0 / static_cast<f64>(per_metre);
    const f64 centre_shift = settings.sample_at_centre ? 0.5 : 0.0;

    // Split by z, the axis the clip is laid out along, so no two slabs share a cache line.
    // More slabs than workers, because a clip is not evenly full and a slab through the roof
    // finishes long before one through the floor.
    const usize slab_count =
        std::min<usize>(static_cast<usize>(clip.size[2]),
                        std::max<usize>(1, (jobs != nullptr) ? jobs->worker_count() * 8 + 8 : 1));
    std::vector<usize> edge(slab_count + 1);
    for (usize s = 0; s <= slab_count; ++s) {
        edge[s] = static_cast<usize>(clip.size[2]) * s / slab_count;
    }

    std::vector<LocalTable> tables(slab_count);
    std::vector<u64> touched(slab_count, 0);

    // Phase one, parallel: perturb, quantise, and record which private slot each voxel wants.
    // The clip's own array is the scratch space — a voxel holds `slot + 1` until phase three
    // turns it into a type id, and zero still means air, so nothing else has to be allocated.
    const auto do_slab = [&](usize begin, usize end) {
        for (usize s = begin; s < end; ++s) {
            LocalTable& table = tables[s];
            table.reserve(4096);
            u64 count = 0;
            for (usize zi = edge[s]; zi < edge[s + 1]; ++zi) {
                const i32 z = static_cast<i32>(zi);
                const i64 wz = placed.origin_voxel[2] + z;
                for (i32 y = 0; y < clip.size[1]; ++y) {
                    const i64 wy = placed.origin_voxel[1] + y;
                    const usize row = clip.index(0, y, z);
                    for (i32 x = 0; x < clip.size[0]; ++x) {
                        const usize index = row + static_cast<usize>(x);
                        const VoxelTypeId base = clip.voxels[index];
                        if (base == kAir) continue;
                        ++count;
                        const i64 wx = placed.origin_voxel[0] + x;

                        f64 scale = 1.0;
                        if (variation.has_by) {
                            const Vec3 p{(static_cast<f64>(wx) + centre_shift) * voxel,
                                         (static_cast<f64>(wy) + centre_shift) * voxel,
                                         (static_cast<f64>(wz) + centre_shift) * voxel};
                            scale = std::clamp(field.eval(variation.by, p), 0.0, 1.0);
                        }

                        const VisualRecord& source = types.visual_of(base);
                        VisualRecord record = source;
                        if (scale > 0.0) {
                            const f64 c = variation.colour * scale;
                            const f64 r = variation.roughness * scale;
                            record.red =
                                nudge(source.red, c, hash_signed(wx, wy, wz, variation.seed));
                            record.green = nudge(source.green, c,
                                                 hash_signed(wx, wy, wz, variation.seed + 71u));
                            record.blue = nudge(source.blue, c,
                                                hash_signed(wx, wy, wz, variation.seed + 149u));
                            record.roughness = nudge(source.roughness, r,
                                                     hash_signed(wx, wy, wz, variation.seed + 227u));
                        }

                        // A key that is never zero, because zero is the empty slot: the base id
                        // is at least one for any voxel that is not air.
                        const u64 key = (static_cast<u64>(base) << 40) |
                                        (static_cast<u64>(record.red) << 32) |
                                        (static_cast<u64>(record.green) << 24) |
                                        (static_cast<u64>(record.blue) << 16) |
                                        (static_cast<u64>(record.roughness) << 8);
                        clip.voxels[index] = table.slot_for(key, record, base) + 1;
                    }
                }
            }
            touched[s] = count;
        }
    };

    if (jobs != nullptr && slab_count > 1) {
        jobs->parallel_for(slab_count, 1, do_slab);
    } else {
        do_slab(0, slab_count);
    }

    // Phase two, serial: intern every private record into the real table. This is where slabs
    // that independently arrived at the same colour collapse onto one id, and where the budget
    // is enforced — it is the only point that can see the global count.
    std::vector<std::vector<VoxelTypeId>> resolve(slab_count);
    std::unordered_map<VoxelTypeId, u64> tally;
    std::unordered_map<VoxelTypeId, std::vector<VoxelTypeId>> spent;   // base -> variants made
    u64 minted = 0;
    for (usize s = 0; s < slab_count; ++s) {
        LocalTable& table = tables[s];
        resolve[s].resize(table.records.size());
        for (usize i = 0; i < table.records.size(); ++i) {
            const VoxelTypeId base = table.bases[i];
            VoxelTypeId variant;
            if (minted < variation.budget) {
                const u32 before = types.type_count();
                variant = types.intern(table.records[i], types.behaviour_of(base));
                if (types.type_count() != before) {
                    ++minted;
                    spent[base].push_back(variant);
                }
            } else {
                // The budget is spent. Reuse one of the variants already made for this base,
                // picked by the key so the choice is stable, which keeps the surface varied —
                // just no longer uniquely so — and keeps the renderer's table within its buffer.
                report.reused += table.counts[i];
                const std::vector<VoxelTypeId>& pool = spent[base];
                variant = pool.empty() ? base : pool[i % pool.size()];
            }
            resolve[s][i] = variant;
            tally[variant] += table.counts[i];
        }
        // The tables are large and nothing needs them past this point.
        table.keys.clear();
        table.keys.shrink_to_fit();
        table.slots.clear();
        table.slots.shrink_to_fit();
        table.records.clear();
        table.records.shrink_to_fit();
        report.voxels += touched[s];
    }

    // Phase three, parallel: swap each voxel's private slot for the type id it became.
    const auto resolve_slab = [&](usize begin, usize end) {
        for (usize s = begin; s < end; ++s) {
            const std::vector<VoxelTypeId>& map = resolve[s];
            const usize from = clip.index(0, 0, static_cast<i32>(edge[s]));
            const usize to = (edge[s + 1] < static_cast<usize>(clip.size[2]))
                                 ? clip.index(0, 0, static_cast<i32>(edge[s + 1]))
                                 : clip.voxels.size();
            for (usize i = from; i < to; ++i) {
                const VoxelTypeId slot = clip.voxels[i];
                if (slot == kAir) continue;
                clip.voxels[i] = map[slot - 1];
            }
        }
    };
    if (jobs != nullptr && slab_count > 1) {
        jobs->parallel_for(slab_count, 1, resolve_slab);
    } else {
        resolve_slab(0, slab_count);
    }

    report.distinct_types = tally.size();
    for (const auto& entry : tally) {
        report.largest_group = std::max(report.largest_group, entry.second);
    }
    clip.build_coarse();
    return report;
}

SampleResult sample(const Field& field, u32 root, const std::vector<PaintRule>& paint,
                    const SampleSettings& settings, JobSystem* jobs) {
    SampleResult result;

    const i32 per_metre = (settings.voxels_per_metre > 0) ? settings.voxels_per_metre
                                                          : kVoxelsPerMetre;
    const f64 voxel = 1.0 / static_cast<f64>(per_metre);

    // The voxel range the requested box covers. Half-open at the top, so a box exactly one metre
    // across at thirty-two voxels per metre is thirty-two voxels and not thirty-three — an
    // off-by-one here is a clip that grows a voxel every time it is re-sampled.
    const i64 lo[3] = {voxel_floor(settings.low.x, per_metre),
                       voxel_floor(settings.low.y, per_metre),
                       voxel_floor(settings.low.z, per_metre)};
    const i64 hi[3] = {voxel_floor(settings.high.x, per_metre),
                       voxel_floor(settings.high.y, per_metre),
                       voxel_floor(settings.high.z, per_metre)};

    i32 size[3];
    for (u32 axis = 0; axis < 3; ++axis) {
        const i64 span = hi[axis] - lo[axis];
        size[axis] = static_cast<i32>(std::max<i64>(span, 0));
        result.origin_voxel[axis] = lo[axis];
    }
    if (size[0] <= 0 || size[1] <= 0 || size[2] <= 0) return result;

    Clip& clip = result.clip;
    clip.size[0] = size[0];
    clip.size[1] = size[1];
    clip.size[2] = size[2];
    const usize cells = static_cast<usize>(size[0]) * static_cast<usize>(size[1]) *
                        static_cast<usize>(size[2]);
    clip.voxels.assign(cells, kAir);
    clip.inside.assign(cells, 0);

    const f64 centre_shift = settings.sample_at_centre ? 0.5 : 0.0;

    // Whether any rule wants a surface normal. Six extra evaluations a voxel is not free, and
    // most clips never ask for it.
    bool wants_normal = false;
    for (const PaintRule& rule : paint) {
        if (rule.facing_axis < 3) wants_normal = true;
    }

    // How far it is safe to jump ahead through empty space.
    //
    // The field answers with a distance, so a point that is half a metre from anything means the
    // next sixteen voxels along the row are empty too and need not be asked about. On a clip that
    // is mostly air — which is every room, every arch, every railing — that is most of the work.
    //
    // The slack is what displacement can hide: a displaced surface may be nearer than the field
    // admits by up to the amount displaced, so that much comes off every jump. When the field
    // cannot bound it, the slack is enormous, no jump is ever taken, and the sampler walks every
    // voxel exactly as it did before.
    // Asked of the shape itself rather than of the whole field, so a displacement in some other
    // expression does not make this one skip less. When the shape is not a distance at all — it
    // has been twisted, or scaled, or displaced by something unbounded — the answer is infinite,
    // every test below fails, and the sampler walks every voxel exactly as it would have.
    const f64 slack = std::min(field.metric_slack(root), field.skip_slack());

    // Whether each paint rule can be settled for a whole block from one reading at its centre.
    // A rule keyed on a shape can: the distance at the centre bounds the distance anywhere in
    // the block, so the rule either applies to all of it, none of it, or has its surface running
    // through it. A rule keyed on a pattern cannot, and is asked per voxel as before.
    std::vector<f64> rule_slack(paint.size(), Field::kInfiniteSlack);
    for (usize i = 0; i < paint.size(); ++i) {
        if (paint[i].facing_axis < 3) continue;   // a normal is a per-voxel question
        rule_slack[i] = field.metric_slack(paint[i].test);
    }

    // The same argument, applied to a block instead of a row.
    //
    // Walking a row and jumping along it only skips in one direction. A clip is empty in all
    // three, and a room is a hundred voxels of nothing in every direction from the middle — asked
    // row by row that is a thousand questions, each answering the same thing.
    //
    // So the sampler asks about an 8×8×8 block first, at its centre. The distance there bounds
    // the distance everywhere in the block: no point in it is further from the centre than the
    // half-diagonal, so a centre distance greater than that (plus the displacement slack) proves
    // every voxel in the block is empty, and one evaluation stands in for five hundred and twelve.
    // The same test the other way round proves a block is entirely solid, which saves the shape
    // evaluation on every voxel inside a wall and leaves only the paint.
    //
    // Eight is the brick edge deliberately. It is the granularity the world stores at, so the
    // blocks the sampler skips are the blocks the world will not allocate.
    constexpr i32 kBlock = 8;
    const i32 blocks[3] = {(size[0] + kBlock - 1) / kBlock, (size[1] + kBlock - 1) / kBlock,
                           (size[2] + kBlock - 1) / kBlock};
    std::vector<u64> counted(static_cast<usize>(blocks[2]), 0);

    const auto do_slab = [&](usize z_begin, usize z_end) {
        // 0 = cannot apply anywhere in this block, 1 = applies to all of it, 2 = ask per voxel.
        std::vector<u8> rule_state(paint.size(), 2);
        for (usize bzi = z_begin; bzi < z_end; ++bzi) {
            u64 local = 0;
            const i32 z0 = static_cast<i32>(bzi) * kBlock;
            const i32 z1 = std::min(z0 + kBlock, size[2]);
            for (i32 by = 0; by < blocks[1]; ++by) {
                const i32 y0 = by * kBlock;
                const i32 y1 = std::min(y0 + kBlock, size[1]);
                for (i32 bx = 0; bx < blocks[0]; ++bx) {
                    const i32 x0 = bx * kBlock;
                    const i32 x1 = std::min(x0 + kBlock, size[0]);

                    // The centre of the block's sample points, and how far the furthest of them
                    // is from it. Measured over the points, not the cube, because the points are
                    // all the field is ever asked about.
                    const f64 half[3] = {static_cast<f64>(x1 - 1 - x0) * 0.5 * voxel,
                                         static_cast<f64>(y1 - 1 - y0) * 0.5 * voxel,
                                         static_cast<f64>(z1 - 1 - z0) * 0.5 * voxel};
                    const Vec3 middle{
                        (static_cast<f64>(lo[0] + x0) + static_cast<f64>(x1 - 1 - x0) * 0.5 +
                         centre_shift) * voxel,
                        (static_cast<f64>(lo[1] + y0) + static_cast<f64>(y1 - 1 - y0) * 0.5 +
                         centre_shift) * voxel,
                        (static_cast<f64>(lo[2] + z0) + static_cast<f64>(z1 - 1 - z0) * 0.5 +
                         centre_shift) * voxel};
                    const f64 radius = std::sqrt(half[0] * half[0] + half[1] * half[1] +
                                                 half[2] * half[2]);
                    const f64 reach = radius + slack;

                    // Is any of this block part of the clip?
                    bool whole_covered = !settings.has_bounds;
                    if (settings.has_bounds) {
                        const f64 db = field.eval(settings.bounds, middle);
                        ++local;
                        if (db > reach) continue;               // none of it is
                        whole_covered = db < -reach;            // all of it is
                    }

                    const f64 dc = field.eval(root, middle);
                    ++local;
                    const bool all_air = dc > reach;
                    const bool all_solid = dc < -reach;

                    if (all_air) {
                        // Nothing in here. The cells still have to be marked as part of the clip:
                        // empty *inside the clip* means "this cell is air and stamping should
                        // clear whatever is there", where a cell outside the clip is none of its
                        // business. Skipping the mark as well as the evaluation left ragged holes
                        // in the mask — invisible in the voxels, and visible the moment a slice
                        // was printed.
                        if (whole_covered) {
                            for (i32 z = z0; z < z1; ++z) {
                                for (i32 y = y0; y < y1; ++y) {
                                    const usize row = clip.index(0, y, z);
                                    for (i32 x = x0; x < x1; ++x) clip.inside[row + x] = 1;
                                }
                            }
                            continue;
                        }
                        // Partly covered: the bounds still have to be asked per voxel, but the
                        // shape does not.
                        for (i32 z = z0; z < z1; ++z) {
                            const f64 pz = (static_cast<f64>(lo[2] + z) + centre_shift) * voxel;
                            for (i32 y = y0; y < y1; ++y) {
                                const f64 py = (static_cast<f64>(lo[1] + y) + centre_shift) * voxel;
                                const usize row = clip.index(0, y, z);
                                for (i32 x = x0; x < x1; ++x) {
                                    const f64 px =
                                        (static_cast<f64>(lo[0] + x) + centre_shift) * voxel;
                                    ++local;
                                    if (field.eval(settings.bounds, {px, py, pz}) <= 0.0) {
                                        clip.inside[row + x] = 1;
                                    }
                                }
                            }
                        }
                        continue;
                    }

                    // Settle what can be settled for the block as a whole.
                    bool every_rule_known = true;
                    for (usize i = 0; i < paint.size(); ++i) {
                        if (rule_slack[i] >= Field::kInfiniteSlack) {
                            rule_state[i] = 2;
                            every_rule_known = false;
                            continue;
                        }
                        const f64 value = field.eval(paint[i].test, middle);
                        ++local;
                        const f64 span = radius + rule_slack[i];
                        if (value - span > paint[i].high || value + span < paint[i].low) {
                            rule_state[i] = 0;
                        } else if (value - span >= paint[i].low && value + span <= paint[i].high) {
                            rule_state[i] = 1;
                        } else {
                            rule_state[i] = 2;
                            every_rule_known = false;
                        }
                    }

                    // Solid all through, inside the clip all through, and every coat of paint
                    // decided. Then the block is one material and there is nothing left to ask.
                    // This is the inside of a wall, which is most of a building.
                    if (all_solid && whole_covered && every_rule_known) {
                        VoxelTypeId type = kAir;
                        for (usize i = 0; i < paint.size(); ++i) {
                            if (rule_state[i] == 1) type = paint[i].type;
                        }
                        if (type == kAir && !paint.empty()) type = paint.front().type;
                        for (i32 z = z0; z < z1; ++z) {
                            for (i32 y = y0; y < y1; ++y) {
                                const usize row = clip.index(0, y, z);
                                for (i32 x = x0; x < x1; ++x) {
                                    clip.inside[row + x] = 1;
                                    clip.voxels[row + x] = type;
                                }
                            }
                        }
                        continue;
                    }

                    for (i32 z = z0; z < z1; ++z) {
                        const f64 pz = (static_cast<f64>(lo[2] + z) + centre_shift) * voxel;
                        for (i32 y = y0; y < y1; ++y) {
                            const f64 py = (static_cast<f64>(lo[1] + y) + centre_shift) * voxel;
                            const usize row = clip.index(0, y, z);
                            for (i32 x = x0; x < x1; ++x) {
                                const f64 px = (static_cast<f64>(lo[0] + x) + centre_shift) * voxel;
                                const Vec3 p{px, py, pz};
                                const usize index = row + static_cast<usize>(x);

                                if (!whole_covered) {
                                    ++local;
                                    if (field.eval(settings.bounds, p) > 0.0) continue;
                                }
                                clip.inside[index] = 1;

                                // Is there matter here? Not asked when the block proved solid.
                                if (!all_solid) {
                                    const f64 d = field.eval(root, p);
                                    ++local;
                                    if (d > 0.0) {
                                        // Empty here, and empty for a while along the row.
                                        const f64 clear = d - slack;
                                        if (clear > voxel && whole_covered) {
                                            const i32 jump = static_cast<i32>(clear / voxel);
                                            if (jump > 1) {
                                                const i32 last = std::min(x + jump - 1, x1 - 1);
                                                for (i32 fill = x + 1; fill <= last; ++fill) {
                                                    clip.inside[row + fill] = 1;
                                                }
                                                x = last;
                                            }
                                        }
                                        continue;
                                    }
                                }

                                // What is it made of? Later rules paint over earlier ones, so the
                                // list reads as a stack of coats.
                                VoxelTypeId type = kAir;
                                Vec3 normal{0, 0, 0};
                                bool have_normal = false;
                                for (usize i = 0; i < paint.size(); ++i) {
                                    const PaintRule& rule = paint[i];
                                    if (rule_state[i] == 0) continue;   // decided for the block
                                    if (rule_state[i] == 1) {
                                        type = rule.type;
                                        continue;
                                    }
                                    const f64 value = field.eval(rule.test, p);
                                    ++local;
                                    if (value < rule.low || value > rule.high) continue;
                                    if (rule.facing_axis < 3) {
                                        if (!have_normal) {
                                            normal = field.normal_at(root, p, voxel);
                                            have_normal = true;
                                            local += 6;
                                        }
                                        const f64 component = (rule.facing_axis == 0)   ? normal.x
                                                              : (rule.facing_axis == 1) ? normal.y
                                                                                        : normal.z;
                                        if (rule.facing_min >= 0.0) {
                                            if (component < rule.facing_min) continue;
                                        } else {
                                            if (component > rule.facing_min) continue;
                                        }
                                    }
                                    type = rule.type;
                                }
                                // A cell with matter in it and no rule that matched is still
                                // matter — it would be worse to silently drop it than to give it
                                // the first type asked for, because a hole in a wall is harder to
                                // notice than a wrong colour.
                                if (type == kAir && !paint.empty()) type = paint.front().type;
                                clip.voxels[index] = type;
                            }
                        }
                    }
                }
            }
            counted[bzi] = local;
        }
    };

    (void)wants_normal;
    if (jobs != nullptr && blocks[2] > 1) {
        jobs->parallel_for(static_cast<usize>(blocks[2]), 1, do_slab);
    } else {
        do_slab(0, static_cast<usize>(blocks[2]));
    }

    for (u64 n : counted) result.evaluations += n;
    clip.build_coarse();
    return result;
}

}  // namespace forge
}  // namespace ws
