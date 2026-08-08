#include "forge/sample.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_map>

#include "core/jobs.hpp"
#include "core/time.hpp"

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

// One hash of a voxel's position, wide enough to cut four ways.
//
// The four channels a voxel's material is perturbed on used to be four separate hashes of the
// same three coordinates — the same mixing work done four times over for four slices of the
// answer. Sixty million voxels is sixty million times to do that.
u64 hash_voxel(i64 x, i64 y, i64 z, u32 seed) {
    u64 h = static_cast<u64>(x) * 0x9E3779B97F4A7C15ull;
    h ^= static_cast<u64>(y) * 0xC2B2AE3D27D4EB4Full;
    h ^= static_cast<u64>(z) * 0x165667B19E3779F9ull;
    h ^= static_cast<u64>(seed) * 0x27D4EB2F165667C5ull;
    h ^= h >> 33; h *= 0xFF51AFD7ED558CCDull;
    h ^= h >> 33; h *= 0xC4CEB9FE1A85EC53ull;
    h ^= h >> 33;
    return h;
}

// A signed value in [-1, 1] from sixteen bits of that hash. Deterministic in position, so a
// voxel's perturbation is a property of where it is and not of when it was computed — the same
// clip built twice is the same clip.
f64 signed_slice(u64 h, u32 which) {
    const u32 bits = static_cast<u32>((h >> (which * 16u)) & 0xFFFFu);
    return static_cast<f64>(bits) * (2.0 / 65536.0) - 1.0;
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
    std::vector<u64> slot_keys;   // the key each slot stands for, in slot order
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
        slot_keys.push_back(key);
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
    //
    // How many is a balance and not an obvious choice. More slabs means smaller private tables,
    // which probe faster because they fit in cache — and more copies of the same perturbations
    // discovered independently, which the merge then has to collapse. Three per worker measured
    // best on the facility; eight per worker made the merge cost more than the probing saved.
    const usize slab_count =
        std::min<usize>(static_cast<usize>(clip.size[2]),
                        std::max<usize>(1, (jobs != nullptr) ? jobs->worker_count() * 3 + 3 : 1));
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
            VoxelTypeId cached_base = kAir;
            VisualRecord cached_source{};
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

                        // Looked up through a small cache rather than through the table. A clip
                        // has a dozen materials and sixty million voxels, so the same handful of
                        // records is fetched over and over through two indirections apiece.
                        if (base != cached_base) {
                            cached_base = base;
                            cached_source = types.visual_of(base);
                        }
                        const VisualRecord& source = cached_source;
                        VisualRecord record = source;
                        if (scale > 0.0) {
                            const f64 c = variation.colour * scale;
                            const f64 r = variation.roughness * scale;
                            const u64 h = hash_voxel(wx, wy, wz, variation.seed);
                            record.red = nudge(source.red, c, signed_slice(h, 0));
                            record.green = nudge(source.green, c, signed_slice(h, 1));
                            record.blue = nudge(source.blue, c, signed_slice(h, 2));
                            record.roughness = nudge(source.roughness, r, signed_slice(h, 3));
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

    const u64 phase_start = now_ns();
    if (jobs != nullptr && slab_count > 1) {
        jobs->parallel_for(slab_count, 1, do_slab);
    } else {
        do_slab(0, slab_count);
    }
    const u64 perturbed_at = now_ns();
    report.perturb_ms = ns_to_ms(perturbed_at - phase_start);

    // Phase two, serial: turn the private tables into real type ids.
    //
    // Merged before interning, and that is the whole trick. Every slab discovers the same
    // perturbations independently — seventy slabs across a wall of one material arrive at the
    // same few hundred thousand colours — so handing each slab's records to the type table one
    // at a time asks it to deduplicate fourteen million records down to one million. The table
    // does that correctly and slowly: three node-based hash lookups per call, ten seconds of
    // them, for an answer a flat array gives in one probe.
    //
    // So the duplicates are collapsed here first, against a single open-addressed table keyed on
    // the same quantised key the slabs used, and only the survivors are interned.
    // Collapsed in parallel, by splitting on the key: each worker owns a share of the key space
    // and merges every slab's records that fall in it. The shares never touch, so no locking, and
    // the only cost is that every worker reads every slab's list — a sequential scan, which is
    // the cheap kind of memory traffic, in exchange for the expensive kind (fourteen million
    // random probes) being divided by the number of cores.
    // A power of two, so choosing a key's owner is a mask rather than a division — asked once
    // per record per worker, which on a clip this size is a hundred million divisions.
    usize parts = 1;
    while (jobs != nullptr && parts < jobs->worker_count() + 1u) parts <<= 1;
    std::vector<LocalTable> merged(parts);
    std::vector<std::vector<u32>> to_merged(slab_count);
    for (usize s = 0; s < slab_count; ++s) to_merged[s].assign(tables[s].records.size(), 0);

    const auto do_merge = [&](usize begin, usize end) {
        for (usize p = begin; p < end; ++p) {
            LocalTable& sink = merged[p];
            sink.reserve(4096);
            for (usize s = 0; s < slab_count; ++s) {
                const LocalTable& table = tables[s];
                for (usize i = 0; i < table.records.size(); ++i) {
                    const u64 key = table.slot_keys[i];
                    // A different mixing constant from the one the table probes with, so a key's
                    // owner and its slot are not correlated.
                    if ((((key * 0xD6E8FEB86659FD93ull) >> 56) & (parts - 1)) != p) continue;
                    const u32 slot = sink.slot_for(key, table.records[i], table.bases[i]);
                    // slot_for counts one voxel for the slot it created or found; the slab knows
                    // the real number, so correct it.
                    sink.counts[slot] += table.counts[i] - 1;
                    to_merged[s][i] = (static_cast<u32>(p) << 24) | slot;
                }
            }
        }
    };
    if (jobs != nullptr && parts > 1) {
        jobs->parallel_for(parts, 1, do_merge);
    } else {
        do_merge(0, parts);
    }

    for (usize s = 0; s < slab_count; ++s) {
        // The slab tables are large and nothing needs them past this point.
        LocalTable& table = tables[s];
        table.keys.clear();
        table.keys.shrink_to_fit();
        table.slots.clear();
        table.slots.shrink_to_fit();
        table.records.clear();
        table.records.shrink_to_fit();
        table.slot_keys.clear();
        table.slot_keys.shrink_to_fit();
        report.voxels += touched[s];
    }

    // Interned on one thread, because the type table is one table and the order records enter it
    // is what makes a clip build the same way twice.
    std::vector<std::vector<VoxelTypeId>> part_id(parts);
    std::unordered_map<VoxelTypeId, std::vector<VoxelTypeId>> spent;   // base -> variants made
    u64 minted = 0;
    for (usize p = 0; p < parts; ++p) {
        const LocalTable& sink = merged[p];
        part_id[p].assign(sink.records.size(), kAir);
        for (usize i = 0; i < sink.records.size(); ++i) {
            const VoxelTypeId base = sink.bases[i];
            if (minted < variation.budget) {
                const u32 before = types.type_count();
                part_id[p][i] = types.intern(sink.records[i], types.behaviour_of(base));
                if (types.type_count() != before) {
                    ++minted;
                    spent[base].push_back(part_id[p][i]);
                }
            } else {
                // The budget is spent. Reuse one of the variants already made for this base,
                // picked by position so the choice is stable, which keeps the surface varied —
                // just no longer uniquely so — and keeps the renderer's table within its buffer.
                report.reused += sink.counts[i];
                const std::vector<VoxelTypeId>& pool = spent[base];
                part_id[p][i] = pool.empty() ? base : pool[i % pool.size()];
            }
        }
    }

    std::vector<std::vector<VoxelTypeId>> resolve(slab_count);
    for (usize s = 0; s < slab_count; ++s) {
        resolve[s].assign(to_merged[s].size(), kAir);
        for (usize i = 0; i < to_merged[s].size(); ++i) {
            const u32 code = to_merged[s][i];
            resolve[s][i] = part_id[code >> 24][code & 0xFFFFFFu];
        }
        to_merged[s].clear();
        to_merged[s].shrink_to_fit();
    }

    report.intern_ms = ns_to_ms(now_ns() - perturbed_at);
    const u64 resolve_start = now_ns();

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

    report.resolve_ms = ns_to_ms(now_ns() - resolve_start);

    // How many distinct records there really are, and how large the largest group of identical
    // voxels is. Counted by sorting a million (id, count) pairs rather than by a hash map, which
    // for a million entries is several times faster and does not allocate a million nodes. Two
    // different keys can still land on one id — two materials that share a behaviour and happen
    // on the same colour — so the pairs really do have to be aggregated, not just counted.
    {
        std::vector<std::pair<VoxelTypeId, u64>> groups;
        for (usize p = 0; p < parts; ++p) {
            for (usize i = 0; i < part_id[p].size(); ++i) {
                groups.emplace_back(part_id[p][i], merged[p].counts[i]);
            }
        }
        std::sort(groups.begin(), groups.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });
        for (usize i = 0; i < groups.size();) {
            usize j = i;
            u64 total = 0;
            while (j < groups.size() && groups[j].first == groups[i].first) {
                total += groups[j].second;
                ++j;
            }
            ++report.distinct_types;
            report.largest_group = std::max(report.largest_group, total);
            i = j;
        }
    }

    // The coarse occupancy mask is deliberately not rebuilt. Variation changes what a voxel is
    // made of and never whether there is one — air stays air, matter stays matter — so the mask
    // the sampler built is still exactly right, and rebuilding it means another pass over every
    // cell in the box to arrive at the same bytes.
    return report;
}

namespace {

// Everything the descent needs, gathered once so the recursion carries a pointer instead of a
// dozen captured references.
struct Descent {
    const Field* field = nullptr;
    u32 root = 0;
    // The shape under any displacement, used to settle whole boxes. See Field::undisplaced.
    u32 prune_root = 0;
    f64 prune_slack = 0.0;         // the worst case, when the parts cannot be told apart
    f64 outer_amplitude = 0.0;     // the displacement that applies everywhere, allowed for always
    // Each part of the shape, with its own box and its own slack, so a box can allow only for
    // the parts it is actually near.
    std::vector<Field::Aabb> part_box;
    std::vector<f64> part_slack;
    bool parts_usable = false;
    // True when no bounds shape narrows the clip, so every cell belongs to it and the mask was
    // filled in once at allocation.
    bool inside_by_default = false;
    const std::vector<PaintRule>* paint = nullptr;
    const SampleSettings* settings = nullptr;
    Clip* clip = nullptr;
    const std::vector<f64>* rule_slack = nullptr;
    i64 lo[3]{0, 0, 0};
    i32 size[3]{0, 0, 0};
    f64 voxel = 0.0;
    f64 centre_shift = 0.5;
    f64 slack = 0.0;
};

// How far a point is from a box, zero inside it.
f64 away_from(const Field::Aabb& box, Vec3 p) {
    if (box.infinite()) return 0.0;
    const f64 dx = std::max(std::max(box.low.x - p.x, p.x - box.high.x), 0.0);
    const f64 dy = std::max(std::max(box.low.y - p.y, p.y - box.high.y), 0.0);
    const f64 dz = std::max(std::max(box.low.z - p.z, p.z - box.high.z), 0.0);
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// How much this box has to allow for, given where it is.
//
// A part of the shape whose box the whole sample box lies outside cannot make the union negative
// anywhere inside it, and its own distance is bounded by its box without any allowance — so its
// displacement is not this box's problem. Only the parts within reach are charged for.
//
// The bounding boxes are already grown by the displacement amount (see build_bounds), so "outside
// the box" already means "outside the displaced shape", and this stays conservative.
f64 slack_here(const Descent& d, Vec3 middle, f64 radius) {
    if (!d.parts_usable) return d.prune_slack;
    f64 worst = 0.0;
    for (usize i = 0; i < d.part_box.size(); ++i) {
        if (d.part_slack[i] <= worst) continue;
        if (away_from(d.part_box[i], middle) > radius) continue;
        worst = d.part_slack[i];
        if (worst >= Field::kInfiniteSlack) return Field::kInfiniteSlack;
    }
    // Not capped by the whole-shape figure, deliberately. A part that says nothing about its
    // neighbourhood — a repetition, a twist — makes boxes near *it* undecidable, and taking the
    // smaller of the two numbers would quietly turn that back into a decision.
    return worst + d.outer_amplitude;
}

// Where a box's sample points are centred, and how far the furthest of them is from that centre.
// Measured over the points rather than the cube, because the points are all the field is ever
// asked about and a box one voxel wide has a radius of zero, not half a voxel.
void box_centre(const Descent& d, const i32 low[3], const i32 high[3], Vec3& middle, f64& radius) {
    f64 half[3];
    f64 at[3];
    for (u32 axis = 0; axis < 3; ++axis) {
        const f64 span = static_cast<f64>(high[axis] - 1 - low[axis]) * 0.5;
        half[axis] = span * d.voxel;
        at[axis] = (static_cast<f64>(d.lo[axis] + low[axis]) + span + d.centre_shift) * d.voxel;
    }
    middle = Vec3{at[0], at[1], at[2]};
    radius = std::sqrt(half[0] * half[0] + half[1] * half[1] + half[2] * half[2]);
}

// Marks every cell of a box as part of the clip, and optionally fills it with one material.
//
// When nothing narrows the clip the mask was filled in when it was allocated and there is nothing
// to mark — which makes a settled *empty* box free, and settled empty boxes are most of a clip.
void fill_box(const Descent& d, const i32 low[3], const i32 high[3], VoxelTypeId type,
              bool matter) {
    if (!matter && d.inside_by_default) return;
    Clip& clip = *d.clip;
    for (i32 z = low[2]; z < high[2]; ++z) {
        for (i32 y = low[1]; y < high[1]; ++y) {
            const usize row = clip.index(0, y, z);
            for (i32 x = low[0]; x < high[0]; ++x) {
                if (!d.inside_by_default) clip.inside[row + x] = 1;
                if (matter) clip.voxels[row + x] = type;
            }
        }
    }
}

// Counted apart, because the two kinds of question are reduced by different means and knowing
// which of them dominates is the difference between optimising the sampler and guessing at it.
struct Tally {
    u64 shape = 0;
    u64 paint = 0;
    u64 singles = 0;   // boxes that came all the way down to one voxel
    u64 settled = 0;   // voxels decided in bulk, without being asked about
};

void descend(const Descent& d, const i32 low[3], const i32 high[3], bool whole_covered,
             const u8* parent_state, bool inherited, Tally& local);

// A box that is solid all through, so the shape need not be asked again — but whose paint still
// has rules nobody could settle. Walks the voxels evaluating only those.
void paint_solid(const Descent& d, const i32 low[3], const i32 high[3], const u8* state,
                 Tally& local) {
    const std::vector<PaintRule>& paint = *d.paint;
    for (i32 z = low[2]; z < high[2]; ++z) {
        const f64 pz = (static_cast<f64>(d.lo[2] + z) + d.centre_shift) * d.voxel;
        for (i32 y = low[1]; y < high[1]; ++y) {
            const f64 py = (static_cast<f64>(d.lo[1] + y) + d.centre_shift) * d.voxel;
            const usize row = d.clip->index(0, y, z);
            for (i32 x = low[0]; x < high[0]; ++x) {
                const f64 px = (static_cast<f64>(d.lo[0] + x) + d.centre_shift) * d.voxel;
                const Vec3 p{px, py, pz};
                VoxelTypeId type = kAir;
                Vec3 normal{0, 0, 0};
                bool have_normal = false;
                for (usize i = 0; i < paint.size(); ++i) {
                    if (state[i] == 0) continue;
                    if (state[i] == 1) {
                        type = paint[i].type;
                        continue;
                    }
                    const PaintRule& rule = paint[i];
                    const f64 value = d.field->eval(rule.test, p);
                    ++local.paint;
                    if (value < rule.low || value > rule.high) continue;
                    if (rule.facing_axis < 3) {
                        if (!have_normal) {
                            normal = d.field->normal_at(d.root, p, d.voxel);
                            have_normal = true;
                            local.paint += 6;
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
                // A cell with matter in it and no rule that matched is still matter — it would be
                // worse to silently drop it than to give it the first type asked for, because a
                // hole in a wall is harder to notice than a wrong colour.
                if (type == kAir && !paint.empty()) type = paint.front().type;
                if (!d.inside_by_default) d.clip->inside[row + x] = 1;
                d.clip->voxels[row + x] = type;
            }
        }
    }
}

void descend(const Descent& d, const i32 low[3], const i32 high[3], bool whole_covered,
             const u8* parent_state, bool inherited, Tally& local) {
    if (low[0] >= high[0] || low[1] >= high[1] || low[2] >= high[2]) return;

    const std::vector<PaintRule>& paint = *d.paint;
    const SampleSettings& settings = *d.settings;
    Clip& clip = *d.clip;

    Vec3 middle;
    f64 radius = 0.0;
    box_centre(d, low, high, middle, radius);
    const bool single = (high[0] - low[0] == 1) && (high[1] - low[1] == 1) &&
                        (high[2] - low[2] == 1);
    const f64 reach = radius + (single ? d.slack : slack_here(d, middle, radius));

    // Is any of this box part of the clip at all?
    if (!whole_covered && settings.has_bounds) {
        const f64 db = d.field->eval(settings.bounds, middle);
        ++local.shape;
        if (db > reach) return;              // none of it is; leave the mask clear
        if (db < -reach) whole_covered = true;
        else if (single) whole_covered = db <= 0.0;
        if (single && !whole_covered) return;
    }

    // A box is settled from the undisplaced shape; a voxel is decided from the real one. The
    // difference is the whole of what makes this descend rather than crawl — the undisplaced
    // shape is a true distance, so half as much has to be allowed for, and it does not evaluate
    // the noise the displacement is made of.
    //
    // (Asking the undisplaced shape first at a single voxel, and consulting the noise only when
    // the answer is close enough to zero for displacement to flip it, was tried and measured
    // neutral. A voxel only reaches this level *because* it is near the surface, so the shortcut
    // almost never fires and mostly buys a second evaluation.)
    const f64 dc = d.field->eval(single ? d.root : d.prune_root, middle);
    ++local.shape;
    if (single) ++local.singles;

    if (dc > reach || (single && dc > 0.0)) {
        // Empty. The cells still have to be marked as part of the clip: empty *inside the clip*
        // means "this cell is air and stamping should clear whatever is there", where a cell
        // outside the clip is none of its business. Skipping the mark as well as the evaluation
        // left ragged holes in the mask — invisible in the voxels, and visible the moment a slice
        // was printed.
        if (whole_covered) {
            fill_box(d, low, high, kAir, false);
            local.settled += static_cast<u64>(high[0] - low[0]) * static_cast<u64>(high[1] - low[1]) *
                             static_cast<u64>(high[2] - low[2]);
            return;
        }
        if (single) {
            if (!d.inside_by_default) clip.inside[clip.index(low[0], low[1], low[2])] = 1;
            return;
        }
        // Partly covered, so which cells belong is still a per-cell question — but the shape is
        // settled and is not asked again.
        for (i32 z = low[2]; z < high[2]; ++z) {
            const f64 pz = (static_cast<f64>(d.lo[2] + z) + d.centre_shift) * d.voxel;
            for (i32 y = low[1]; y < high[1]; ++y) {
                const f64 py = (static_cast<f64>(d.lo[1] + y) + d.centre_shift) * d.voxel;
                const usize row = clip.index(0, y, z);
                for (i32 x = low[0]; x < high[0]; ++x) {
                    const f64 px = (static_cast<f64>(d.lo[0] + x) + d.centre_shift) * d.voxel;
                    ++local.shape;
                    if (d.field->eval(settings.bounds, {px, py, pz}) <= 0.0) {
                        clip.inside[row + x] = 1;
                    }
                }
            }
        }
        return;
    }

    const bool all_solid = (dc < -reach) || single;

    // Settle what can be settled for this box. Rules the parent already decided are inherited
    // rather than re-asked; only the ones still open are tested, and they get sharper every time
    // the box halves.
    u8 stack_state[64];
    const usize rules = paint.size();
    u8* state = stack_state;
    std::vector<u8> heap_state;
    if (rules > sizeof(stack_state)) {
        heap_state.assign(rules, 2);
        state = heap_state.data();
    }
    bool every_rule_known = true;
    for (usize i = 0; i < rules; ++i) {
        const u8 was = (inherited && parent_state != nullptr) ? parent_state[i] : u8{2};
        if (was != 2) {
            state[i] = was;
            continue;
        }
        if ((*d.rule_slack)[i] >= Field::kInfiniteSlack) {
            state[i] = 2;
            every_rule_known = false;
            continue;
        }
        const f64 value = d.field->eval(paint[i].test, middle);
        ++local.paint;
        const f64 span = radius + (*d.rule_slack)[i];
        if (value - span > paint[i].high || value + span < paint[i].low) {
            state[i] = 0;
        } else if (value - span >= paint[i].low && value + span <= paint[i].high) {
            state[i] = 1;
        } else if (single) {
            state[i] = (value >= paint[i].low && value <= paint[i].high) ? u8{1} : u8{0};
        } else {
            state[i] = 2;
            every_rule_known = false;
        }
    }

    if (all_solid && whole_covered) {
        if (every_rule_known) {
            // One material all through, and nothing left to ask. This is the inside of a wall.
            VoxelTypeId type = kAir;
            for (usize i = 0; i < rules; ++i) {
                if (state[i] == 1) type = paint[i].type;
            }
            if (type == kAir && !paint.empty()) type = paint.front().type;
            fill_box(d, low, high, type, true);
            local.settled += static_cast<u64>(high[0] - low[0]) * static_cast<u64>(high[1] - low[1]) *
                             static_cast<u64>(high[2] - low[2]);
            return;
        }
        // The shape is settled but a pattern rule is not, so the voxels are walked for the paint
        // alone. Splitting further would only re-ask the shape for an answer already known.
        paint_solid(d, low, high, state, local);
        return;
    }

    if (single) {
        // A single voxel that is neither settled solid nor settled empty cannot happen — the
        // tests above are exhaustive at this size — but the box may still be outside the clip.
        return;
    }

    // Halve on every axis that has more than one voxel left.
    i32 mid[3];
    for (u32 axis = 0; axis < 3; ++axis) {
        mid[axis] = low[axis] + (high[axis] - low[axis]) / 2;
        if (mid[axis] <= low[axis]) mid[axis] = high[axis];
    }
    for (u32 child = 0; child < 8; ++child) {
        i32 clow[3];
        i32 chigh[3];
        bool empty = false;
        for (u32 axis = 0; axis < 3; ++axis) {
            const bool upper = ((child >> axis) & 1u) != 0;
            clow[axis] = upper ? mid[axis] : low[axis];
            chigh[axis] = upper ? high[axis] : mid[axis];
            if (clow[axis] >= chigh[axis]) empty = true;
        }
        if (empty) continue;
        descend(d, clow, chigh, whole_covered, state, true, local);
    }
}

}  // namespace

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
    // Every cell belongs to the clip unless a bounds shape says otherwise, so in the ordinary case
    // the mask is right the moment it exists and the sampler never touches it again.
    clip.inside.assign(cells, settings.has_bounds ? u8{0} : u8{1});

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

    // The shape without its outermost displacements, and how far they can move its surface. Wanted
    // here as well as in the descent, because it is also how far the paint has to reach — see
    // below.
    f64 amplitude = 0.0;
    const u32 prune_root = field.undisplaced(root, amplitude);
    const f64 prune_inner = field.metric_slack(prune_root);
    const f64 prune_slack = (prune_inner >= Field::kInfiniteSlack)
                                ? Field::kInfiniteSlack
                                : std::min(prune_inner + amplitude, slack);

    // Whether each paint rule can be settled for a whole block from one reading at its centre.
    // A rule keyed on a shape can: the distance at the centre bounds the distance anywhere in
    // the block, so the rule either applies to all of it, none of it, or has its surface running
    // through it. A rule keyed on a pattern cannot, and is asked per voxel as before.
    //
    // The same reading says whether the rule may be widened, which is the other half of the job.
    //
    // Displacement moves a surface. It does not move which part a voxel belongs to. A rule says
    // "this material where that shape is inside" — a test against a shape, accepted below zero —
    // and then the shape is displaced, twelve millimetres of grain over the whole facility. A
    // voxel that exists only because the grain pushed the surface *out* sits outside the shape its
    // own rule names, so the test is false, no rule matches, and it falls back to the first coat.
    // The result is a rind of the wrong colour over every outward-facing surface of every painted
    // object, which reads exactly like weathering and was reported as such.
    //
    // So a metric rule is given the displacement's own amplitude at both ends of its band. That is
    // exact rather than generous: the set a one-Lipschitz test accepts, grown by d in space, is the
    // same test accepted over a band d wider — which is precisely what "metric" buys and precisely
    // why a pattern is left alone, a noise's value being no kind of distance for a millimetre to be
    // added to. Both ends, because displacement moves a surface inward as often as outward and a
    // rule reading "within five centimetres of the face" has to follow the face both ways.
    //
    // Only the displacement applied to the whole shape is charged. One applied to a single part of
    // a union is not, because there is no one number for it — and a rule widened by an unbounded
    // amount is a rule that paints the entire clip.
    std::vector<f64> rule_slack(paint.size(), Field::kInfiniteSlack);
    std::vector<PaintRule> widened = paint;
    for (usize i = 0; i < paint.size(); ++i) {
        const f64 metric = field.metric_slack(paint[i].test);
        if (paint[i].facing_axis >= 3) rule_slack[i] = metric;   // a normal is a per-voxel question
        if (metric >= Field::kInfiniteSlack) continue;
        widened[i].low -= amplitude;
        widened[i].high += amplitude;
    }

    // Asked of a box, and then of its halves, and then of theirs.
    //
    // A signed distance says more than "is there matter at this point". It says how far away the
    // nearest matter is, and that bounds the answer for every point within that distance. So the
    // sampler need never ask about a point at all until it has narrowed down to somewhere the
    // answer could still go either way — which is a thin shell around the surface, and nothing
    // else.
    //
    // A box is read once at its centre. If the distance there exceeds the half-diagonal (plus what
    // displacement can hide) then every point in the box is on the same side of the surface, and
    // the box is settled: filled or empty, in one reading, however large it is. Otherwise it is
    // cut in half on each axis and its eight children are asked the same question. Only a box that
    // has come all the way down to a single voxel and *still* straddles the surface costs a
    // per-voxel evaluation.
    //
    // The work therefore scales with the area of the surface rather than the volume of the box.
    // That is the difference between a building costing what its walls cost and costing what the
    // air around them costs — for the facility, sixty million shape evaluations became four.
    //
    // The paint comes down with it. A rule keyed on a shape is settled by the same argument, so
    // "water where the pool is" is answered once for the half of the site that has no pool in it
    // rather than three million times. A rule keyed on a pattern cannot be settled that way — a
    // noise says nothing about the next voxel — so it is inherited as "still to ask" and paid for
    // per voxel, which is the honest price of asking for one.
    Descent descent;
    descent.field = &field;
    descent.root = root;
    descent.prune_root = prune_root;
    descent.prune_slack = prune_slack;
    descent.outer_amplitude = amplitude;
    descent.inside_by_default = !settings.has_bounds;

    // The parts the shape is made of, each with its own box and its own allowance. Only usable
    // when every part can be bounded and measured — one part that cannot is one part that could
    // be anywhere, and then the worst case is the only honest answer.
    {
        std::vector<Field::Part> parts;
        field.union_children(prune_root, parts);
        descent.parts_usable = !parts.empty();
        for (const Field::Part& piece : parts) {
            const u32 part = piece.node;
            // Only the parts that ask for something. A part with no slack contributes nothing to
            // the maximum this list is scanned for, so keeping it means testing its box against
            // every box in the descent to learn that it changes nothing.
            //
            // It is most of them. A building is displaced in one or two places — a lawn given a
            // grain, a wall given a weathering — and is otherwise made of exact shapes. The
            // facility flattens to two hundred and seventeen parts of which three have any slack
            // at all, so this turns a scan of two hundred and seventeen boxes per box into a scan
            // of three.
            //
            // A part nothing can be said about — infinite slack — is kept, and must be: it makes
            // boxes near it undecidable, which is correct, and leaves every box elsewhere free to
            // settle on what its own neighbours need.
            const f64 inner = field.metric_slack(part);
            const f64 part_slack =
                (inner >= Field::kInfiniteSlack) ? inner : inner + piece.extra;
            if (part_slack <= 0.0) continue;
            // Grown by what the peeled displacement can move it, so the box still contains the
            // shape after the displacement it is now being charged for.
            Field::Aabb box = field.bounds_of(part);
            if (!box.infinite() && piece.extra > 0.0) {
                const f64 by = piece.extra * 0.5;
                box.low = Vec3{box.low.x - by, box.low.y - by, box.low.z - by};
                box.high = Vec3{box.high.x + by, box.high.y + by, box.high.z + by};
            }
            descent.part_box.push_back(box);
            descent.part_slack.push_back(part_slack);
        }
    }
    descent.paint = &widened;
    descent.settings = &settings;
    descent.clip = &clip;
    descent.rule_slack = &rule_slack;
    descent.voxel = voxel;
    descent.centre_shift = centre_shift;
    descent.slack = slack;
    for (u32 axis = 0; axis < 3; ++axis) {
        descent.lo[axis] = lo[axis];
        descent.size[axis] = size[axis];
    }

    result.slack = slack;
    result.prune_slack = prune_slack;
    result.parts = descent.parts_usable ? descent.part_slack.size() : 0;

    // The worst part, and how much of the clip it reaches over — which together are the whole
    // story of why a build is slow. A part with a large slack and a small box costs nothing but
    // the boxes beside it; the same slack on a part whose box spans the clip charges every box in
    // the building for it, and the two are indistinguishable from the slack alone.
    result.best_part_slack = 0.0;
    result.worst_part_reach = 0.0;
    const f64 whole = std::max(1e-9, (settings.high.x - settings.low.x) *
                                         (settings.high.y - settings.low.y) *
                                         (settings.high.z - settings.low.z));
    for (usize i = 0; i < descent.part_slack.size(); ++i) {
        if (descent.part_slack[i] <= result.best_part_slack) continue;
        result.best_part_slack = descent.part_slack[i];
        const Field::Aabb& b = descent.part_box[i];
        result.worst_part_reach =
            b.infinite() ? 1.0
                         : std::min(1.0, ((b.high.x - b.low.x) * (b.high.y - b.low.y) *
                                          (b.high.z - b.low.z)) / whole);
    }

    // The top of the tree is cut into pieces first so there is something to spread across the
    // cores. Sixty-four voxels — two metres — is small enough that a clip of any size has more
    // pieces than it has cores, and large enough that the descent does the interesting work
    // rather than the loop that hands it out.
    constexpr i32 kTop = 64;
    const i32 tops[3] = {(size[0] + kTop - 1) / kTop, (size[1] + kTop - 1) / kTop,
                         (size[2] + kTop - 1) / kTop};
    const usize top_count = static_cast<usize>(tops[0]) * static_cast<usize>(tops[1]) *
                            static_cast<usize>(tops[2]);
    std::vector<Tally> counted(top_count);

    const auto do_top = [&](usize begin, usize end) {
        for (usize t = begin; t < end; ++t) {
            const i32 bx = static_cast<i32>(t % static_cast<usize>(tops[0]));
            const i32 by = static_cast<i32>((t / static_cast<usize>(tops[0])) %
                                            static_cast<usize>(tops[1]));
            const i32 bz = static_cast<i32>(t / (static_cast<usize>(tops[0]) *
                                                 static_cast<usize>(tops[1])));
            const i32 low[3] = {bx * kTop, by * kTop, bz * kTop};
            const i32 high[3] = {std::min(low[0] + kTop, size[0]),
                                 std::min(low[1] + kTop, size[1]),
                                 std::min(low[2] + kTop, size[2])};
            Tally local;
            descend(descent, low, high, /*whole_covered=*/!settings.has_bounds,
                    /*parent_state=*/nullptr, /*inherited=*/false, local);
            counted[t] = local;
        }
    };

    (void)wants_normal;
    if (jobs != nullptr && top_count > 1) {
        jobs->parallel_for(top_count, 1, do_top);
    } else {
        do_top(0, top_count);
    }

    for (const Tally& n : counted) {
        result.shape_evaluations += n.shape;
        result.paint_evaluations += n.paint;
        result.voxels_asked += n.singles;
        result.voxels_settled += n.settled;
    }
    result.evaluations = result.shape_evaluations + result.paint_evaluations;
    clip.build_coarse();
    return result;
}

}  // namespace forge
}  // namespace ws
