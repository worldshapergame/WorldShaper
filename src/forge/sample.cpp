#include "forge/sample.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <mutex>
#include <unordered_map>

#include "core/jobs.hpp"
#include "core/time.hpp"

namespace ws {
namespace forge {

// The block cut, and its control arm. See the note in sample.hpp.
//
// A plain global read once per `sample` call and carried on the descent, for the reason
// `SamplePlan::rule_bounds` is carried rather than read again where it is used: a setting read
// twice is two settings, and an arm of an A/B that changes under a running descent is not an arm.
namespace {

// The environment is a second door on the same setting, and it is here because the first door
// cannot be opened from a command line: `src/app/main.cpp` has no flag for this yet. Both arms of
// the A/B have to be one binary or they are two builds being compared (trap 29), so
// `WS_SAMPLE_BLOCK_CELLS=0` is the control arm and `WS_SAMPLE_BLOCK_CELLS=64` is a different cut,
// without a rebuild between them. Read once, at start-up, and never again.
u32 block_cells_at_start() {
    // An EMPTY setting is not a setting, and this is not pedantry: a shell that clears a variable
    // by assigning "" to it leaves the variable there, `atoi` reads nought out of it, and nought is
    // the control arm — so the arm under test quietly measures the arm it is being compared with.
    // That happened once here and the run that caught it was a 733-of-733 that proved nothing.
    if (const char* asked = std::getenv("WS_SAMPLE_BLOCK_CELLS")) {
        if (asked[0] != '\0') {
            const int n = std::atoi(asked);
            if (n >= 0) return static_cast<u32>(n);
        }
    }
    return 512;
}

u32 g_block_cells = block_cells_at_start();

}  // namespace
void set_sample_block_cells(u32 cells) { g_block_cells = cells; }
u32 sample_block_cells() { return g_block_cells; }

// --------------------------------------------------------------------------------------
// The box-against-cell agreement audit. See the note in sample.hpp for what it is for.
// --------------------------------------------------------------------------------------
//
// One relaxed flag and one mutex-guarded list. The flag is read once per box the descent settles
// in bulk, which is a predictable branch on a line the box has already touched; the lock is only
// ever taken by a box that has something to say, and a clip with nothing to say never takes it.
namespace {

std::atomic<bool> g_box_cell_audit{false};
std::mutex g_box_cell_lock;
BoxCellAudit g_box_cell;

// A ceiling on the list, so a clip that disagrees everywhere reports the first few thousand rather
// than eating the machine. The counters beside it are not capped and are the numbers to read.
constexpr usize kMaxBoxCellFaults = 8192;

}  // namespace

void set_box_cell_audit(bool on) { g_box_cell_audit.store(on, std::memory_order_relaxed); }
bool box_cell_audit() { return g_box_cell_audit.load(std::memory_order_relaxed); }

BoxCellAudit take_box_cell_audit() {
    std::lock_guard<std::mutex> held(g_box_cell_lock);
    BoxCellAudit out = std::move(g_box_cell);
    g_box_cell = BoxCellAudit{};
    return out;
}

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
    //
    // **The RECORD is compared, not only the key.** The key used to pack the base id and the four
    // channels variation touches into sixty-four bits exactly, so equal keys were equal records by
    // construction — and a driven property (`DrivenChannel`) can move three more channels, which
    // do not fit. So the key is a hash now and the probe carries on past a collision, which keeps
    // the table exact rather than merging two different records that happened to hash alike.
    u32 slot_for(u64 key, const VisualRecord& record, VoxelTypeId base) {
        u64 at = (key * 0x9E3779B97F4A7C15ull) >> 32 & mask;
        while (keys[at] != 0 &&
               (keys[at] != key || bases[slots[at]] != base || !(records[slots[at]] == record))) {
            at = (at + 1) & mask;
        }
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

// Everything a perturbation or a driven property may move, mixed into one word. FNV-1a, because
// the inputs are eight bytes and a byte at a time is as fast as anything cleverer at that size —
// and never zero, because zero is the table's empty slot.
u64 record_key(VoxelTypeId base, const VisualRecord& record) {
    u64 h = 1469598103934665603ull;
    const auto mix = [&h](u64 v) { h = (h ^ v) * 1099511628211ull; };
    mix(static_cast<u64>(base));
    mix(record.red);
    mix(record.green);
    mix(record.blue);
    mix(record.opacity);
    mix(record.roughness);
    mix(record.metallic);
    mix(record.emissive);
    return h | 1ull;
}

// The byte of a record one driven property writes.
u8& channel_of(VisualRecord& record, DrivenChannel::Which which) {
    switch (which) {
        case DrivenChannel::Which::Red: return record.red;
        case DrivenChannel::Which::Green: return record.green;
        case DrivenChannel::Which::Blue: return record.blue;
        case DrivenChannel::Which::Opacity: return record.opacity;
        case DrivenChannel::Which::Roughness: return record.roughness;
        case DrivenChannel::Which::Metallic: return record.metallic;
        case DrivenChannel::Which::Emissive: break;
    }
    return record.emissive;
}

}  // namespace

VariationReport apply_variation(Clip& clip, VoxelTypeTable& types, const Field& field,
                                const Variation& variation, const SampleSettings& settings,
                                const SampleResult& placed, JobSystem* jobs, VariationPool* pool) {
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
            u32 cached_first = 0;   // this base's run in `variation.driven`
            u32 cached_last = 0;
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

                        // Where this voxel is, in the field's own coordinates. Worked out once
                        // and shared, because a driven property and a scaled perturbation both
                        // want it and a clip can have several of each.
                        const bool wants_place = variation.has_by || !variation.driven.empty();
                        const Vec3 p =
                            wants_place ? Vec3{(static_cast<f64>(wx) + centre_shift) * voxel,
                                               (static_cast<f64>(wy) + centre_shift) * voxel,
                                               (static_cast<f64>(wz) + centre_shift) * voxel}
                                        : Vec3{0.0, 0.0, 0.0};

                        f64 scale = 1.0;
                        if (variation.has_by) {
                            scale = std::clamp(field.eval(variation.by, p), 0.0, 1.0);
                        }

                        // Looked up through a small cache rather than through the table. A clip
                        // has a dozen materials and sixty million voxels, so the same handful of
                        // records is fetched over and over through two indirections apiece.
                        if (base != cached_base) {
                            cached_base = base;
                            cached_source = types.visual_of(base);
                            // And which stretch of the driven list belongs to it. They are sorted
                            // by type, so one base's properties are one contiguous run and the
                            // lookup is two walks rather than a search per voxel.
                            cached_first = 0;
                            cached_last = 0;
                            for (u32 d = 0; d < static_cast<u32>(variation.driven.size()); ++d) {
                                if (variation.driven[d].type != base) continue;
                                if (cached_last == cached_first) cached_first = d;
                                cached_last = d + 1;
                            }
                        }
                        const VisualRecord& source = cached_source;
                        VisualRecord record = source;

                        // **The properties this type reads from a field**, before the
                        // perturbation, so a driven colour is varied like any other colour rather
                        // than instead of one.
                        for (u32 d = cached_first; d < cached_last; ++d) {
                            const DrivenChannel& one = variation.driven[d];
                            // The run is contiguous in every document this can read, and the guard
                            // is here so that a list that is not still gives the right answer
                            // rather than another type's colour.
                            if (one.type != base) continue;
                            // **A pattern in this language runs from -1 to 1**, not from 0 to 1:
                            // `value_noise` ends `accum * 2 - 1` and `fbm` is a normalised sum of
                            // it, and `sine`, `waves` and `ridged` are the same shape. Mapping
                            // from 0 to 1 -- which is what `variation by=` does, and what this did
                            // first -- clamps the whole bottom half of a noise field to nothing,
                            // and a red driven by an fbm came out flat BLACK across the block.
                            // Measured, not assumed: the test probes three points and reads -0.5,
                            // -0.2, -0.1.
                            //
                            // Anything outside is held at the ends, and an author who wants a
                            // narrower band aims it with `remap`, `clamp` or `smoothstep`, which
                            // is what the arithmetic words are for.
                            const f64 at =
                                std::clamp((field.eval(one.field, p) + 1.0) * 0.5, 0.0, 1.0);
                            const f64 span = static_cast<f64>(one.high) - static_cast<f64>(one.low);
                            channel_of(record, one.which) = static_cast<u8>(
                                std::lround(static_cast<f64>(one.low) + at * span));
                        }

                        if (scale > 0.0) {
                            const f64 c = variation.colour * scale;
                            const f64 r = variation.roughness * scale;
                            const u64 h = hash_voxel(wx, wy, wz, variation.seed);
                            // **From `record`, not from `source`.** A driven property has already
                            // written into `record` above, and perturbing from the base record
                            // threw that away — a red read from a field came out flat, because
                            // this line put the material's own red back over it every voxel.
                            // Identical to what it was when nothing is driven, because `record` is
                            // then a copy of `source` and this is the same arithmetic on the same
                            // bytes.
                            record.red = nudge(record.red, c, signed_slice(h, 0));
                            record.green = nudge(record.green, c, signed_slice(h, 1));
                            record.blue = nudge(record.blue, c, signed_slice(h, 2));
                            record.roughness = nudge(record.roughness, r, signed_slice(h, 3));
                        }

                        clip.voxels[index] =
                            table.slot_for(record_key(base, record), record, base) + 1;
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
    VariationPool mine;                          // base -> variants made
    VariationPool& spent = (pool != nullptr) ? *pool : mine;
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
                const std::vector<VoxelTypeId>& made = spent[base];
                part_id[p][i] = made.empty() ? base : made[i % made.size()];
            }
        }
    }

    report.minted = minted;

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

// Does a feature THINNER THAN A VOXEL pass close by this point?
//
// The sampler asks one question per voxel — is the field negative at its centre — and that question
// cannot see anything narrower than the spacing between centres. A window mullion half a voxel wide
// is not thinned or aliased by it; it is absent, because no centre ever lands inside it. The glass
// behind it is a broad shape that does cover centres, so it takes the cell and the frame looks
// eaten. Reported exactly that way: vertical bars of window frames disappearing into the glass.
//
// The obvious fix is to sample several points inside each cell and call it solid if any is inside.
// That does rescue thin features and it also FATTENS every surface in the world: a flat wall lying
// on the grid has its boundary cells' centres half a voxel outside, with corners a quarter of a
// voxel in, so every wall grows by a voxel. Not acceptable in a building made of flat walls.
//
// So the test asks about thickness rather than about coverage. Walk from here to just inside the
// surface, then keep going one voxel further along the same line. A wall is still solid down there
// and nothing happens. A mullion has ended, and only then is the cell promoted.
//
// The cost is paid by a shell of cells — those outside the shape but within a cell-diagonal of it —
// and by nothing else. Interior and open air are untouched.
bool thin_feature_here(const Field& field, u32 root, Vec3 p, f64 outside_by, f64 voxel,
                       u64* evaluations) {
    // Which way is out. An SDF's gradient points away from the surface, and central differences over
    // a quarter of a voxel are steady enough for a direction even where the field is not smooth.
    const f64 h = voxel * 0.25;
    Vec3 away{field.eval(root, {p.x + h, p.y, p.z}) - field.eval(root, {p.x - h, p.y, p.z}),
              field.eval(root, {p.x, p.y + h, p.z}) - field.eval(root, {p.x, p.y - h, p.z}),
              field.eval(root, {p.x, p.y, p.z + h}) - field.eval(root, {p.x, p.y, p.z - h})};
    if (evaluations != nullptr) *evaluations += 6;
    const f64 length = std::sqrt(away.x * away.x + away.y * away.y + away.z * away.z);
    if (length < 1.0e-12) return false;
    away.x /= length;
    away.y /= length;
    away.z /= length;

    // Just inside the surface. If there is nothing solid that way, the field was near zero here for
    // some other reason and there is no feature to rescue.
    const f64 reach = outside_by + voxel * 0.05;
    const Vec3 within{p.x - away.x * reach, p.y - away.y * reach, p.z - away.z * reach};
    if (evaluations != nullptr) ++*evaluations;
    if (field.eval(root, within) > 0.0) return false;

    // And a whole voxel further in. Still solid means a wall, and a wall does not need rescuing —
    // this is what keeps the flat faces of the building exactly where they were. Out the far side
    // means the whole feature is thinner than the grid can hold, and it is kept.
    const Vec3 beyond{within.x - away.x * voxel, within.y - away.y * voxel,
                      within.z - away.z * voxel};
    if (evaluations != nullptr) ++*evaluations;
    return field.eval(root, beyond) > 0.0;
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
    // the parts it is actually near. Borrowed from the plan rather than held, because a plan
    // serves many samples and copying its lists per call is the thing D614 exists to stop.
    const std::vector<Field::Aabb>* part_box = nullptr;
    const std::vector<f64>* part_slack = nullptr;
    bool parts_usable = false;
    // True when no bounds shape narrows the clip, so every cell belongs to it and the mask was
    // filled in once at allocation.
    bool inside_by_default = false;
    const std::vector<PaintRule>* paint = nullptr;
    const SampleSettings* settings = nullptr;
    Clip* clip = nullptr;
    const std::vector<f64>* rule_slack = nullptr;
    // Where each rule could possibly apply, when that can be said at all.
    //
    // A rule keyed on a shape and accepted below a threshold applies only inside that shape, so
    // it applies only inside that shape's bounding box — and a box of the descent that lies
    // wholly outside it can settle the rule to "no" for nothing, without asking the field.
    //
    // This is the same trick the union cull plays, and it matters here for the same reason it
    // mattered there, only more so: the facility carries a hundred and thirty-three paint rules,
    // one or two from each of twenty authors, and paint was eighty-six per cent of every field
    // evaluation in the building. Almost none of those rules can apply at any given place, and
    // almost all of them were being asked.
    const std::vector<Field::Aabb>* rule_box = nullptr;

    // The pieces of a placed rule's zone, when its zone is a union of things standing apart.
    //
    // One box round a union of two distant pieces is a box round everything between them, and a
    // weathering zone is exactly that: the facility's sunny zone is the great steps at the ground
    // and the cornice wash at the top, so its bounding box is the building. The box above then
    // rejects nothing and every solid voxel in the facility pays for an occlusion it is thirty
    // metres from.
    //
    // Flat, with a begin/end pair per rule, because this is read at every box of the descent and a
    // vector of vectors is a pointer chase per rule to find out there are no pieces.
    const std::vector<Field::Aabb>* rule_piece = nullptr;
    const std::vector<u32>* rule_piece_at = nullptr;

    // Whether the boxes above are also applied per CELL, in `paint_solid`. Carried from the plan
    // rather than read from the setting, so that a plan says what it is and a sample taken from it
    // cannot be built under one rule and read under another.
    bool rule_bounds = true;

    // Optional: one counter per paint rule, so a build can say which rules it spent itself on.
    //
    // Off unless asked for. Paint has been the majority of every expensive build this project has
    // had, and every time the answer was a handful of rules out of a hundred and thirty-three —
    // but finding out which took a guess, a change and a twenty-minute rebuild each round. A
    // number per rule turns that into one run.
    //
    // Written from every worker, so relaxed atomics: the count is a diagnostic and an occasional
    // lost increment against nine hundred million would not change a single decision made from it.
    std::atomic<u64>* rule_cost = nullptr;
    i64 lo[3]{0, 0, 0};
    i32 size[3]{0, 0, 0};
    f64 voxel = 0.0;
    f64 centre_shift = 0.5;
    f64 slack = 0.0;

    // At or below this many cells, the rest of the descent below the box is run a level at a time
    // rather than a box at a time. Nought is the control arm. See `sample_block_cells`.
    u32 block_cells = 0;
};

// Half a cell's diagonal, in voxels: the furthest a surface can be from a cell's centre and still
// pass through that cell. It is the reach of the thin-feature rescue above, and therefore the
// distance a box has to be clear by before it may be settled as empty in bulk.
//
// One constant in one place, and it was not: the rescue's own test carried the number and the box
// test did not allow for it at all, so a box could be declared empty over cells the per-voxel rule
// would have kept. At the world's thirty-two voxels a metre that is 2.7 cm and it almost never
// bites; at ONE voxel a metre, which is what a level-8 node asks for, it is 87 cm and it bites
// constantly -- and whether it bit depended on how the descent happened to have cut the box up, so
// the same node came out differently sampled alone and sampled inside the building. R11a's
// agreement check found it: 17 of 32 nodes at level 8, 87 cells of matter present in one and
// absent in the other. D613.
constexpr f64 kHalfCellDiagonal = 0.8660254;

// How far a point is from a box, zero inside it.
f64 away_from(const Field::Aabb& box, Vec3 p) {
    // No `infinite()` short cut. See `squared_distance_to` in field.cpp for why: a box may be
    // unbounded on one axis and bounded on another, and the arithmetic below already answers
    // nought on the unbounded ones.
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
    if (!d.parts_usable || d.part_box == nullptr) return d.prune_slack;
    const std::vector<Field::Aabb>& boxes = *d.part_box;
    const std::vector<f64>& slacks = *d.part_slack;
    f64 worst = 0.0;
    for (usize i = 0; i < boxes.size(); ++i) {
        if (slacks[i] <= worst) continue;
        if (away_from(boxes[i], middle) > radius) continue;
        worst = slacks[i];
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

// Every cell of a box the descent has just settled in bulk, asked the way a SINGLE VOXEL is asked.
//
// Nothing here shares a line of reasoning with the box test. `Field::eval` on the real root at the
// cell's own centre, plus the thin-feature rescue, is exactly and only what `descend` applies to a
// box of one voxel — no prune root, no slack, no radius, no descent above it. That is the point:
// the reference in `tests/test_sample.cpp` calls `sample` and then compares the clip it got back
// with a per-voxel walk, so the cells a box settled in bulk are read out of the descent's own
// answer and the comparison is downstream of the thing it is checking (trap 26).
//
// Runs only when `set_box_cell_audit(true)` has been called, and its evaluations go into no figure
// the sampler reports, so a gate taken with the audit on is the same gate.
void audit_box(const Descent& d, const i32 low[3], const i32 high[3], f64 centre_value, f64 radius,
               f64 reach, bool claims_solid) {
    std::vector<BoxCellFault> found;
    u64 cells = 0;
    u64 wrong = 0;
    for (i32 z = low[2]; z < high[2]; ++z) {
        const f64 pz = (static_cast<f64>(d.lo[2] + z) + d.centre_shift) * d.voxel;
        for (i32 y = low[1]; y < high[1]; ++y) {
            const f64 py = (static_cast<f64>(d.lo[1] + y) + d.centre_shift) * d.voxel;
            for (i32 x = low[0]; x < high[0]; ++x) {
                const f64 px = (static_cast<f64>(d.lo[0] + x) + d.centre_shift) * d.voxel;
                const Vec3 p{px, py, pz};
                ++cells;
                const f64 value = d.field->eval(d.root, p);
                bool rescued = false;
                if (value > 0.0 && value < d.voxel * kHalfCellDiagonal) {
                    rescued = thin_feature_here(*d.field, d.root, p, value, d.voxel, nullptr);
                }
                const bool cell_solid = (value <= 0.0) || rescued;
                if (cell_solid == claims_solid) continue;
                ++wrong;
                if (found.size() >= kMaxBoxCellFaults) continue;
                BoxCellFault fault;
                fault.voxel[0] = d.lo[0] + x;
                fault.voxel[1] = d.lo[1] + y;
                fault.voxel[2] = d.lo[2] + z;
                for (u32 axis = 0; axis < 3; ++axis) {
                    fault.box_low[axis] = d.lo[axis] + low[axis];
                    fault.side[axis] = high[axis] - low[axis];
                }
                fault.centre_value = centre_value;
                fault.cell_value = value;
                fault.radius = radius;
                fault.reach = reach;
                fault.voxel_metres = d.voxel;
                fault.box_solid = claims_solid;
                fault.cell_rescued = rescued;
                found.push_back(fault);
            }
        }
    }

    std::lock_guard<std::mutex> held(g_box_cell_lock);
    if (claims_solid) {
        ++g_box_cell.boxes_solid;
        g_box_cell.solid_over_claimed += wrong;
    } else {
        ++g_box_cell.boxes_empty;
        g_box_cell.empty_over_claimed += wrong;
    }
    g_box_cell.cells_checked += cells;
    for (const BoxCellFault& f : found) {
        if (g_box_cell.faults.size() >= kMaxBoxCellFaults) break;
        g_box_cell.faults.push_back(f);
    }
}

// Counted apart, because the two kinds of question are reduced by different means and knowing
// which of them dominates is the difference between optimising the sampler and guessing at it.
struct Tally {
    u64 shape = 0;
    u64 paint = 0;
    u64 singles = 0;   // boxes that came all the way down to one voxel
    u64 settled = 0;   // voxels decided in bulk, without being asked about

    // Where the time actually went, as distinct from where the evaluations went.
    //
    // Those two turned out not to be the same thing at all, and assuming they were cost this file
    // a day: halving the paint evaluations moved the build by three per cent, because the ones
    // removed were the cheap ones. An evaluation is a walk of a tree and the trees differ by
    // orders of magnitude, so a count says how often and only a clock says how long.
    u64 paint_ns = 0;
    u64 shape_ns = 0;

    // How many times the field was WALKED, as distinct from how many points were answered.
    //
    // The two used to be the same number and that is the whole of what the sweep changes: 640
    // evaluations a node arriving in six traversals rather than in six hundred and forty. A count
    // of evaluations cannot see that and a clock cannot see it either until the block evaluator
    // underneath `Field::eval_block` is real, so the thing being bought has to be counted directly.
    u64 blocks = 0;    // calls to Field::eval_block
};


void descend(const Descent& d, const i32 low[3], const i32 high[3], bool whole_covered,
             const u8* parent_state, bool inherited, Tally& local);

// Whether a rule could apply AT THIS POINT, from its boxes alone.
//
// The same two tests the descent runs over a box, run over one voxel, and it is not a repetition:
// a box that straddles the edge of a rule's place cannot settle the rule for all of it, so every
// cell of that box — including the ones outside the place entirely — reached the evaluation. That
// made `on=<shape>` mean "inside the shape's box, or in any box that touches it", which is a
// promise nobody can state, and it is the last part of the leak this change is about. Three
// comparisons against six doubles, against a walk of the whole expression.
bool rule_reaches(const Descent& d, usize i, Vec3 p) {
    if (d.rule_box != nullptr && away_from((*d.rule_box)[i], p) > 0.0) return false;
    if (d.rule_piece_at == nullptr) return true;
    const u32 from = (*d.rule_piece_at)[i];
    const u32 to = (*d.rule_piece_at)[i + 1];
    if (from == to) return true;
    for (u32 piece = from; piece < to; ++piece) {
        if (away_from((*d.rule_piece)[piece], p) <= 0.0) return true;
    }
    return false;
}

// A box that is solid all through, so the shape need not be asked again — but whose paint still
// has rules nobody could settle. Walks the voxels evaluating only those.
void paint_solid(const Descent& d, const i32 low[3], const i32 high[3], const u8* state,
                 Tally& local) {
    const u64 entered = now_ns();
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
                    if (d.rule_bounds && !rule_reaches(d, i, p)) continue;
                    const PaintRule& rule = paint[i];
                    const f64 value = d.field->eval(rule.test, p);
                    ++local.paint;
                    if (d.rule_cost != nullptr) {
                        d.rule_cost[i].fetch_add(1, std::memory_order_relaxed);
                    }
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
    local.paint_ns += now_ns() - entered;
}

// --------------------------------------------------------------------------------------
// The block sweep: the same descent, one LEVEL at a time, so the field is walked once a level
// instead of once a box.
// --------------------------------------------------------------------------------------
//
// D722 measured what the descent costs at the size of a render-tree node: about **640 evaluations
// of the shape to fill 512 cells**, and `65,536 voxels asked` of `65,536 cells` over a batch of a
// hundred and twenty-eight nodes — the descent comes all the way down to a single voxel for nearly
// every cell of every ladder node. Every one of those 640 walks visits the same field nodes, tests
// the same boxes and takes the same turns, because every one of them is about a point inside the
// same quarter of a metre. `Field::eval_block` answers many points in one traversal; what follows
// is how the descent is made to ask that way.
//
// # Why the box's cells are not simply all asked at once
//
// That was built first and measured, because it is the obvious reading of the number above: stop at
// the node's own box, evaluate all 512 cells in one call, and do the rescue and the paint from
// those answers. It works, it takes the facility from **638 shape evaluations a node to 581**, and
// **it does not build the same world**. `clips/sampler.clip`, which is gated on a content hash:
//
// | cut | solid voxels | content |
// |---|---|---|
// | the descent, untouched | 1,430,104 | `d0d5f84c685be847` |
// | ask every cell of a 64-cell box | 1,429,884 | `74ccef4afa2d94b6` |
// | ask every cell of a 512-cell box | 1,429,596 | `f165ca95fb5886b7` |
//
// The reason is a fact about the sampler rather than about the change, and it is worth writing
// down. A box that settles SOLID in bulk fills every cell of itself, from the undisplaced shape
// read once at its centre against a radius and a slack. Asking those cells one at a time does not
// always agree: the bulk answer keeps a fringe of matter that the per-cell rule, which is the rule
// `tests/test_sample.cpp`'s brute-force reference implements, does not. Which of the two is right is
// D613's question one size along. What matters here is that they differ, and that the world in
// every cache and every gate in this repository is the descent's.
//
// Note what that also means: cutting at EIGHT cells is bit-identical, because a two-voxel box's
// eight children are single voxels and nothing is settled in bulk below it. It is also worth
// nothing — 639.8 evaluations a node against the control's 638.0 — because it removes one level of
// a four-level tree.
//
// # So the LEVELS are batched, and the descent itself is not touched at all
//
// Every box at one level is evaluated in one call. The boxes at a level are independent — each
// reads its own centre, settles or does not, and hands its own children down — so taking them
// together changes the order the field is asked in and nothing else. Same points, same roots, same
// comparisons, same answers, and therefore the same world: `clips/sampler.clip` builds
// `d0d5f84c685be847` at 1,430,104 solid voxels on both arms.
//
// A node then costs about **six traversals of the field instead of six hundred and forty**, for
// exactly the same six hundred and forty evaluations. The count was never what was overpaid; the
// walking was, and the count is only its shadow.

// A box settled empty whose cells are not all part of the clip. Which of them are is still a
// per-cell question and the shape is not asked again.
//
// Lifted out of `descend` so that the sweep runs this code rather than a copy of it. Two
// implementations of a rule this fiddly — half a cell diagonal, a rescue on the BOUNDS shape rather
// than on the solid — is two rules, and the second one is always the one nobody reads.
void mark_partly_covered(const Descent& d, const i32 low[3], const i32 high[3], Tally& local) {
    const SampleSettings& settings = *d.settings;
    Clip& clip = *d.clip;
    for (i32 z = low[2]; z < high[2]; ++z) {
        const f64 pz = (static_cast<f64>(d.lo[2] + z) + d.centre_shift) * d.voxel;
        for (i32 y = low[1]; y < high[1]; ++y) {
            const f64 py = (static_cast<f64>(d.lo[1] + y) + d.centre_shift) * d.voxel;
            const usize row = clip.index(0, y, z);
            for (i32 x = low[0]; x < high[0]; ++x) {
                const f64 px = (static_cast<f64>(d.lo[0] + x) + d.centre_shift) * d.voxel;
                ++local.shape;
                const f64 here = d.field->eval(settings.bounds, {px, py, pz});
                if (here <= 0.0) {
                    clip.inside[row + x] = 1;
                } else if (here < d.voxel * kHalfCellDiagonal &&
                           thin_feature_here(*d.field, settings.bounds, {px, py, pz}, here,
                                             d.voxel, &local.shape)) {
                    // Half a cell diagonal is the furthest a surface can be from a centre and
                    // still pass through the cell at all, so outside that there is nothing here
                    // to lose and nothing is asked.
                    clip.inside[row + x] = 1;
                }
            }
        }
    }
}

// One box waiting its turn at a level of the sweep. Everything `descend` works out about a box
// before it decides anything, kept beside the box so a level can be worked out for all of them and
// then decided for all of them.
struct SweepBox {
    i32 low[3]{0, 0, 0};
    i32 high[3]{0, 0, 0};
    Vec3 middle{};
    f64 radius = 0.0;
    f64 reach = 0.0;
    f64 dc = 0.0;
    u8 whole_covered = 0;
    u8 single = 0;
    u8 alive = 1;      // a box the bounds shape has already disposed of
    u8 rescued = 0;

    u64 cells() const {
        return static_cast<u64>(high[0] - low[0]) * static_cast<u64>(high[1] - low[1]) *
               static_cast<u64>(high[2] - low[2]);
    }
};

// The lists a sweep runs on, kept per thread so a node's worth of them is allocated once for the
// life of the process rather than once a box.
struct SweepScratch {
    std::vector<SweepBox> cur;
    std::vector<SweepBox> next;
    std::vector<u8> cur_state;    // `rules` bytes a box, in the order of `cur`
    std::vector<u8> next_state;
    std::vector<u8> state;        // one box's settled state, rebuilt per box
    std::vector<Vec3> points;     // whatever the current call is asking about
    std::vector<f64> values;
    std::vector<u32> pick;        // which boxes a gather covers
    std::vector<u32> candidate;   // single voxels near enough the surface to be worth rescuing
    std::vector<u32> live;        // those of them still in play, stage by stage
    std::vector<Vec3> away;       // the outward direction at each
    std::vector<Vec3> within;     // the point just inside the surface at each
};

SweepScratch& sweep_scratch() {
    static thread_local SweepScratch scratch;
    return scratch;
}

// `thin_feature_here` for a whole level's worth of single voxels: three traversals instead of eight
// evaluations apiece.
//
// The same points, in the same order, through the same evaluator — `Field::eval_block` promises the
// same f64 per point as `eval` — so the answer is the one the per-cell rule gives, bit for bit.
//
// Three stages and not one, because that rule has two early exits and they are not decoration: a
// voxel whose gradient is nothing never asks the two questions below it, and one that is still
// solid a hair inside never asks the last. Batching straight through them would ask MORE of the
// field than the rule does, which is the opposite of the point of being here.
void sweep_rescue(const Descent& d, SweepScratch& s, Tally& local) {
    const u64 entered = now_ns();
    const f64 h = d.voxel * 0.25;

    // Which way is out. Central differences over a quarter of a voxel, six points a voxel.
    s.points.clear();
    for (const u32 c : s.candidate) {
        const Vec3 p = s.cur[c].middle;
        s.points.push_back(Vec3{p.x + h, p.y, p.z});
        s.points.push_back(Vec3{p.x - h, p.y, p.z});
        s.points.push_back(Vec3{p.x, p.y + h, p.z});
        s.points.push_back(Vec3{p.x, p.y - h, p.z});
        s.points.push_back(Vec3{p.x, p.y, p.z + h});
        s.points.push_back(Vec3{p.x, p.y, p.z - h});
    }
    s.values.resize(s.points.size());
    d.field->eval_block(d.root, s.points.data(), s.points.size(), s.values.data());
    ++local.blocks;
    local.shape += s.points.size();

    s.away.clear();
    s.live.clear();
    for (usize k = 0; k < s.candidate.size(); ++k) {
        Vec3 away{s.values[k * 6 + 0] - s.values[k * 6 + 1],
                  s.values[k * 6 + 2] - s.values[k * 6 + 3],
                  s.values[k * 6 + 4] - s.values[k * 6 + 5]};
        const f64 length = std::sqrt(away.x * away.x + away.y * away.y + away.z * away.z);
        if (length < 1.0e-12) continue;   // the field is flat here; there is nothing to rescue
        away.x /= length;
        away.y /= length;
        away.z /= length;
        s.away.push_back(away);
        s.live.push_back(s.candidate[k]);
    }

    // Just inside the surface. Nothing solid that way and the field was near zero here for some
    // other reason, so there is no feature to keep.
    if (!s.live.empty()) {
        s.points.clear();
        for (usize k = 0; k < s.live.size(); ++k) {
            const Vec3 p = s.cur[s.live[k]].middle;
            const Vec3 a = s.away[k];
            const f64 reach = s.cur[s.live[k]].dc + d.voxel * 0.05;
            s.points.push_back(Vec3{p.x - a.x * reach, p.y - a.y * reach, p.z - a.z * reach});
        }
        s.values.resize(s.points.size());
        d.field->eval_block(d.root, s.points.data(), s.points.size(), s.values.data());
        ++local.blocks;
        local.shape += s.points.size();

        s.within.clear();
        usize kept = 0;
        for (usize k = 0; k < s.live.size(); ++k) {
            if (s.values[k] > 0.0) continue;
            s.within.push_back(s.points[k]);
            s.away[kept] = s.away[k];
            s.live[kept] = s.live[k];
            ++kept;
        }
        s.away.resize(kept);
        s.live.resize(kept);
    }

    // And a whole voxel further in. Still solid is a wall and a wall needs no rescuing; out the far
    // side means the whole feature is thinner than the grid can hold, and it is kept.
    if (!s.live.empty()) {
        s.points.clear();
        for (usize k = 0; k < s.live.size(); ++k) {
            const Vec3 w = s.within[k];
            const Vec3 a = s.away[k];
            s.points.push_back(
                Vec3{w.x - a.x * d.voxel, w.y - a.y * d.voxel, w.z - a.z * d.voxel});
        }
        s.values.resize(s.points.size());
        d.field->eval_block(d.root, s.points.data(), s.points.size(), s.values.data());
        ++local.blocks;
        local.shape += s.points.size();
        for (usize k = 0; k < s.live.size(); ++k) {
            if (s.values[k] > 0.0) s.cur[s.live[k]].rescued = 1;
        }
    }

    local.shape_ns += now_ns() - entered;
}

// The rest of the descent below one box, level by level.
//
// `descend` has already asked everything about the box itself; this picks up where its recursion
// would have, with the box's eight children, and carries on to single voxels. Every test below is
// the one `descend` runs, in the order `descend` runs it — read the two side by side, because the
// only thing that may ever differ between them is when the field is asked, never what it is asked.
void sweep(const Descent& d, const i32 seed_low[3], const i32 seed_high[3], bool whole_covered,
           const u8* parent_state, Tally& local) {
    const SampleSettings& settings = *d.settings;
    const std::vector<PaintRule>& paint = *d.paint;
    const usize rules = paint.size();
    SweepScratch& s = sweep_scratch();

    // Halve on every axis that has more than one voxel left, exactly as the recursion does.
    s.cur.clear();
    s.cur_state.clear();
    {
        i32 mid[3];
        for (u32 axis = 0; axis < 3; ++axis) {
            mid[axis] = seed_low[axis] + (seed_high[axis] - seed_low[axis]) / 2;
            if (mid[axis] <= seed_low[axis]) mid[axis] = seed_high[axis];
        }
        for (u32 child = 0; child < 8; ++child) {
            SweepBox box;
            box.whole_covered = whole_covered ? u8{1} : u8{0};
            bool empty = false;
            for (u32 axis = 0; axis < 3; ++axis) {
                const bool upper = ((child >> axis) & 1u) != 0;
                box.low[axis] = upper ? mid[axis] : seed_low[axis];
                box.high[axis] = upper ? seed_high[axis] : mid[axis];
                if (box.low[axis] >= box.high[axis]) empty = true;
            }
            if (empty) continue;
            s.cur.push_back(box);
            for (usize r = 0; r < rules; ++r) {
                s.cur_state.push_back((parent_state != nullptr) ? parent_state[r] : u8{2});
            }
        }
    }

    while (!s.cur.empty()) {
        const usize m = s.cur.size();

        // Where each box's sample points are centred, how far the furthest of them is, and what
        // this box has to allow for. All of it is arithmetic and none of it asks the field.
        for (usize i = 0; i < m; ++i) {
            SweepBox& box = s.cur[i];
            box.alive = 1;
            box.rescued = 0;
            box_centre(d, box.low, box.high, box.middle, box.radius);
            box.single = ((box.high[0] - box.low[0] == 1) && (box.high[1] - box.low[1] == 1) &&
                          (box.high[2] - box.low[2] == 1))
                             ? u8{1}
                             : u8{0};
            box.reach = box.radius + (box.single ? d.slack
                                                 : slack_here(d, box.middle, box.radius));
        }

        // Is any of this box part of the clip at all? One call for every box at this level that
        // still has to ask.
        if (settings.has_bounds) {
            s.pick.clear();
            s.points.clear();
            for (usize i = 0; i < m; ++i) {
                if (s.cur[i].whole_covered) continue;
                s.pick.push_back(static_cast<u32>(i));
                s.points.push_back(s.cur[i].middle);
            }
            if (!s.pick.empty()) {
                s.values.resize(s.pick.size());
                d.field->eval_block(settings.bounds, s.points.data(), s.points.size(),
                                    s.values.data());
                ++local.blocks;
                local.shape += s.pick.size();
                for (usize k = 0; k < s.pick.size(); ++k) {
                    SweepBox& box = s.cur[s.pick[k]];
                    const f64 db = s.values[k];
                    const f64 rescue = box.single ? 0.0 : d.voxel * kHalfCellDiagonal;
                    if (db > box.reach + rescue) {
                        box.alive = 0;           // none of it is; leave the mask clear
                        continue;
                    }
                    if (db < -box.reach) {
                        box.whole_covered = 1;
                    } else if (box.single) {
                        box.whole_covered = (db <= 0.0) ? u8{1} : u8{0};
                    }
                    if (box.single && !box.whole_covered) box.alive = 0;
                }
            }
        }

        // A box is settled from the undisplaced shape; a voxel is decided from the real one. Two
        // roots, so two calls — and a level can hold both, because an axis already down to one
        // voxel stops halving while the others carry on.
        for (u32 pass = 0; pass < 2; ++pass) {
            const bool want_single = (pass == 1);
            s.pick.clear();
            s.points.clear();
            for (usize i = 0; i < m; ++i) {
                if (!s.cur[i].alive) continue;
                if ((s.cur[i].single != 0) != want_single) continue;
                s.pick.push_back(static_cast<u32>(i));
                s.points.push_back(s.cur[i].middle);
            }
            if (s.pick.empty()) continue;
            s.values.resize(s.pick.size());
            const u64 entered = now_ns();
            d.field->eval_block(want_single ? d.root : d.prune_root, s.points.data(),
                                s.points.size(), s.values.data());
            ++local.blocks;
            local.shape_ns += now_ns() - entered;
            local.shape += s.pick.size();
            if (want_single) local.singles += s.pick.size();
            for (usize k = 0; k < s.pick.size(); ++k) s.cur[s.pick[k]].dc = s.values[k];
        }

        // The thin-feature rescue, for the shell of single voxels that are outside the shape but
        // within half a cell diagonal of it, and for no others.
        s.candidate.clear();
        {
            const f64 band = d.voxel * kHalfCellDiagonal;
            for (usize i = 0; i < m; ++i) {
                const SweepBox& box = s.cur[i];
                if (!box.alive || !box.single) continue;
                if (box.dc > 0.0 && box.dc < band) s.candidate.push_back(static_cast<u32>(i));
            }
        }
        if (!s.candidate.empty()) sweep_rescue(d, s, local);

        // And now every box decides, with nothing left to ask the field except the paint.
        s.next.clear();
        s.next_state.clear();
        for (usize i = 0; i < m; ++i) {
            SweepBox& box = s.cur[i];
            if (!box.alive) continue;
            const u8* inherited = (rules > 0) ? &s.cur_state[i * rules] : nullptr;
            const f64 rescue = box.single ? 0.0 : d.voxel * kHalfCellDiagonal;

            if (!box.rescued && (box.dc > box.reach + rescue || (box.single && box.dc > 0.0))) {
                // Empty. The cells still have to be marked as part of the clip; see `descend`.
                if (box.whole_covered) {
                    if (!box.single && g_box_cell_audit.load(std::memory_order_relaxed)) {
                        audit_box(d, box.low, box.high, box.dc, box.radius, box.reach,
                                  /*claims_solid=*/false);
                    }
                    fill_box(d, box.low, box.high, kAir, false);
                    local.settled += box.cells();
                    continue;
                }
                if (box.single) {
                    if (!d.inside_by_default) {
                        d.clip->inside[d.clip->index(box.low[0], box.low[1], box.low[2])] = 1;
                    }
                    continue;
                }
                mark_partly_covered(d, box.low, box.high, local);
                continue;
            }

            const bool all_solid = (box.dc < -box.reach) || box.single;

            // Settle what can be settled for this box. Rules the parent already decided are
            // inherited rather than re-asked; only the ones still open are tested, and they get
            // sharper every time the box halves.
            s.state.assign(rules, 2);
            u8* state = s.state.data();
            bool every_rule_known = true;
            for (usize r = 0; r < rules; ++r) {
                const u8 was = (inherited != nullptr) ? inherited[r] : u8{2};
                if (was != 2) {
                    state[r] = was;
                    continue;
                }
                if (away_from((*d.rule_box)[r], box.middle) > box.radius) {
                    state[r] = 0;
                    continue;
                }
                if (d.rule_piece_at != nullptr) {
                    const u32 from = (*d.rule_piece_at)[r];
                    const u32 to = (*d.rule_piece_at)[r + 1];
                    if (from != to) {
                        bool near_a_piece = false;
                        for (u32 q = from; q < to && !near_a_piece; ++q) {
                            near_a_piece = away_from((*d.rule_piece)[q], box.middle) <= box.radius;
                        }
                        if (!near_a_piece) {
                            state[r] = 0;
                            continue;
                        }
                    }
                }
                if ((*d.rule_slack)[r] >= Field::kInfiniteSlack) {
                    state[r] = 2;
                    every_rule_known = false;
                    continue;
                }
                const f64 value = d.field->eval(paint[r].test, box.middle);
                ++local.paint;
                if (d.rule_cost != nullptr) d.rule_cost[r].fetch_add(1, std::memory_order_relaxed);
                const f64 span = box.radius + (*d.rule_slack)[r];
                if (value - span > paint[r].high || value + span < paint[r].low) {
                    state[r] = 0;
                } else if (value - span >= paint[r].low && value + span <= paint[r].high) {
                    state[r] = 1;
                } else if (box.single) {
                    state[r] = (value >= paint[r].low && value <= paint[r].high) ? u8{1} : u8{0};
                } else {
                    state[r] = 2;
                    every_rule_known = false;
                }
            }

            if (all_solid && box.whole_covered) {
                if (!box.single && g_box_cell_audit.load(std::memory_order_relaxed)) {
                    audit_box(d, box.low, box.high, box.dc, box.radius, box.reach,
                              /*claims_solid=*/true);
                }
                if (every_rule_known) {
                    VoxelTypeId type = kAir;
                    for (usize r = 0; r < rules; ++r) {
                        if (state[r] == 1) type = paint[r].type;
                    }
                    if (type == kAir && !paint.empty()) type = paint.front().type;
                    fill_box(d, box.low, box.high, type, true);
                    local.settled += box.cells();
                    continue;
                }
                paint_solid(d, box.low, box.high, state, local);
                continue;
            }

            // A single voxel that is neither settled solid nor settled empty cannot happen — the
            // tests above are exhaustive at this size — but the box may still be outside the clip.
            if (box.single) continue;

            i32 mid[3];
            for (u32 axis = 0; axis < 3; ++axis) {
                mid[axis] = box.low[axis] + (box.high[axis] - box.low[axis]) / 2;
                if (mid[axis] <= box.low[axis]) mid[axis] = box.high[axis];
            }
            for (u32 child = 0; child < 8; ++child) {
                SweepBox kid;
                kid.whole_covered = box.whole_covered;
                bool empty = false;
                for (u32 axis = 0; axis < 3; ++axis) {
                    const bool upper = ((child >> axis) & 1u) != 0;
                    kid.low[axis] = upper ? mid[axis] : box.low[axis];
                    kid.high[axis] = upper ? box.high[axis] : mid[axis];
                    if (kid.low[axis] >= kid.high[axis]) empty = true;
                }
                if (empty) continue;
                s.next.push_back(kid);
                s.next_state.insert(s.next_state.end(), state, state + rules);
            }
        }

        s.cur.swap(s.next);
        s.cur_state.swap(s.next_state);
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

    // How far a BOX has to be clear by before nothing inside it could be rescued as a thin
    // feature. A single voxel asks that question itself, below; a box that settles in bulk never
    // asks it at all, so it may only settle when no cell it holds could have been kept.
    //
    // Nought for a single voxel, so the per-voxel path is exactly what it was.
    const f64 rescue = single ? 0.0 : d.voxel * kHalfCellDiagonal;

    // Is any of this box part of the clip at all?
    if (!whole_covered && settings.has_bounds) {
        const f64 db = d.field->eval(settings.bounds, middle);
        ++local.shape;
        // Plus the rescue, for the same reason as the shape below: the per-cell branch keeps a
        // cell whose bounds centre is outside by less than half a diagonal, and a box settled
        // here never reaches that branch.
        if (db > reach + rescue) return;     // none of it is; leave the mask clear
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
    const u64 shape_at = now_ns();
    const f64 dc = d.field->eval(single ? d.root : d.prune_root, middle);
    local.shape_ns += now_ns() - shape_at;
    ++local.shape;
    if (single) ++local.singles;

    // A single voxel whose centre is outside the shape is normally empty, and THAT is the test that
    // loses anything narrower than a voxel: a mullion half a voxel wide never covers a centre, so no
    // centre ever reports it and it is not thinned but deleted outright.
    //
    // Asked only of leaf voxels that are outside but within half a cell diagonal — the furthest a
    // surface can be from a centre and still cross the cell. Everything else is untouched, so this
    // costs a shell of cells around the surface and nothing anywhere else.
    // Timed and counted into the shape's own figures rather than falling between them. Eight
    // evaluations of the REAL root -- displacement, noise and all -- against one of the undisplaced
    // shape above, so a cell that reaches this is several times the price of one that does not, and
    // until D613 nothing said so: this function's whole cost appeared in no number the sampler
    // reports. Traps 17 and 18, in the oldest code in the file.
    bool rescued = false;
    if (single && dc > 0.0 && dc < d.voxel * kHalfCellDiagonal) {
        const u64 rescue_at = now_ns();
        rescued = thin_feature_here(*d.field, d.root, middle, dc, d.voxel, &local.shape);
        local.shape_ns += now_ns() - rescue_at;
    }

    // `reach + rescue` rather than `reach`, and that is the whole of D613. A box may be settled
    // empty when the field at its centre is further out than anything inside it could be near --
    // but "near" for a leaf voxel is not nought, it is half a cell diagonal, because that is how
    // far a surface can be from a centre and still cross the cell the rescue keeps. Without the
    // term a box hands back air over cells the per-voxel rule would have kept, and WHICH cells
    // depends on how the box happened to be cut up.
    if (!rescued && (dc > reach + rescue || (single && dc > 0.0))) {
        // Empty. The cells still have to be marked as part of the clip: empty *inside the clip*
        // means "this cell is air and stamping should clear whatever is there", where a cell
        // outside the clip is none of its business. Skipping the mark as well as the evaluation
        // left ragged holes in the mask — invisible in the voxels, and visible the moment a slice
        // was printed.
        if (whole_covered) {
            if (!single && g_box_cell_audit.load(std::memory_order_relaxed)) {
                audit_box(d, low, high, dc, radius, reach, /*claims_solid=*/false);
            }
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
        mark_partly_covered(d, low, high, local);
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
        // Nowhere near the only place this rule could apply, so it does not, and no evaluation is
        // needed to know it. Free, and the difference between asking a hundred and thirty-three
        // rules at every box and asking the two or three that could possibly be true here.
        if (away_from((*d.rule_box)[i], middle) > radius) {
            state[i] = 0;
            continue;
        }
        // Inside the rule's overall box, but a zone made of separate pieces is mostly the space
        // between them. Asked second and only for the few boxes the test above let through, so a
        // rule with no pieces pays two loads and a comparison.
        if (d.rule_piece_at != nullptr) {
            const u32 from = (*d.rule_piece_at)[i];
            const u32 to = (*d.rule_piece_at)[i + 1];
            if (from != to) {
                bool near_a_piece = false;
                for (u32 p = from; p < to && !near_a_piece; ++p) {
                    near_a_piece = away_from((*d.rule_piece)[p], middle) <= radius;
                }
                if (!near_a_piece) {
                    state[i] = 0;
                    continue;
                }
            }
        }
        if ((*d.rule_slack)[i] >= Field::kInfiniteSlack) {
            state[i] = 2;
            every_rule_known = false;
            continue;
        }
        const f64 value = d.field->eval(paint[i].test, middle);
        ++local.paint;
        if (d.rule_cost != nullptr) d.rule_cost[i].fetch_add(1, std::memory_order_relaxed);
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
        if (!single && g_box_cell_audit.load(std::memory_order_relaxed)) {
            audit_box(d, low, high, dc, radius, reach, /*claims_solid=*/true);
        }
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

    // Small enough that the rest of the tree is worth doing a level at a time.
    //
    // Every settling test above has been tried and none of them took: this box straddles the
    // surface, or the clip's edge runs through it. Below a certain size that means it is going to
    // be cut into eight boxes that each straddle it too, and so on down to single voxels — which is
    // exactly what D722 measured happening inside every node of the render tree, 640 walks of the
    // expression over one quarter of a metre. `sweep` does the same descent breadth-first, so those
    // walks become one a level. See the long note above it.
    //
    // The cut is here rather than at the top for one reason: memory. A level of the sweep holds
    // every box abreast, with its inherited paint state, and the facility carries 628 rules — so
    // the bottom level of a 512-cell box is 512 boxes at 628 bytes, and the bottom level of the
    // whole-clip sampler's own 64³ top box would be a quarter of a million.
    const u64 cells = static_cast<u64>(high[0] - low[0]) * static_cast<u64>(high[1] - low[1]) *
                      static_cast<u64>(high[2] - low[2]);
    if (d.block_cells > 0 && cells <= static_cast<u64>(d.block_cells)) {
        sweep(d, low, high, whole_covered, state, local);
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

SamplePlan plan_sample(const Field& field, u32 root, const std::vector<PaintRule>& paint) {
    SamplePlan plan;
    plan.field = &field;
    plan.root = root;
    plan.rule_bounds = rule_bounds_used();

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
    std::vector<Field::Aabb> rule_box(paint.size());

    // Enough for a zone written as a handful of named places, and a ceiling so that a rule placed
    // on a hedge of four hundred bushes falls back to its one box rather than making every voxel
    // walk four hundred boxes to find out it is in none of them.
    constexpr usize kMaxRulePieces = 16;
    std::vector<Field::Aabb> rule_piece;
    std::vector<u32> rule_piece_at(paint.size() + 1, 0);

    std::vector<PaintRule> widened = paint;
    for (usize i = 0; i < paint.size(); ++i) {
        // A rule that names its place is placed, WHATEVER its test is, and this has to come before
        // anything else gives up on the rule.
        //
        // It is the whole point of the field existing. A weathering coat keyed on a curvature or
        // an occlusion cannot be settled for a region by any cleverness about its test — and does
        // not need to be, because it was confined to a named shape when it was written. Placing it
        // below the "this test says nothing, give up" branch put it after a `continue` taken by
        // exactly the rules it was written for, and changed nothing at all.
        rule_piece_at[i] = static_cast<u32>(rule_piece.size());
        if (paint[i].has_place) {
            // Asked of `containing_bounds_of` and not of `bounds_of`, and the difference is the
            // whole of this rule's bound.
            //
            // A place is a CONTAINMENT question. The descent never evaluates it — it compares the
            // sample box against this box and settles the rule to "no" when the two do not meet —
            // so what it needs is "the shape is inside here" and not "a point outside here is told
            // an honest distance". `bounds_of` promises both, which is why it refuses a box to a
            // non-uniformly scaled shape however well it knows where that shape is, and refusing
            // one node takes the box off everything standing over it. The pavilion scales its
            // coping and `weather overgrown ... on=pav_coping` stands on the result: two coats, of
            // 628, asked at every solid voxel of the estate for a box that was known and withheld.
            //
            // The test below is deliberately NOT asked this way. A test's box is grown by the
            // band the rule accepts, which is an argument about distance, and an uneven scale
            // under-reports distance — so the grown box could be smaller than the set the rule
            // accepts, and a rule settled to "no" over cells it would have painted is paint that
            // silently disappears.
            const Field::Aabb where = rule_bounds_used()
                                          ? field.containing_bounds_of(paint[i].place)
                                          : field.bounds_of(paint[i].place);
            if (!where.infinite()) {
                // Grown by the whole-clip displacement, because that is what can carry a voxel out
                // of the shape it belongs to.
                const f64 reach = amplitude;
                rule_box[i].low = Vec3{where.low.x - reach, where.low.y - reach,
                                       where.low.z - reach};
                rule_box[i].high = Vec3{where.high.x + reach, where.high.y + reach,
                                        where.high.z + reach};

                // And the pieces the zone is actually made of.
                //
                // A weathering zone is written as the places that weather, which on a building are
                // not one place: the facility's sunny zone is the great steps at the ground and the
                // cornice wash twelve metres above them. One box round both is a box round the
                // whole building, so the test above rejects nothing and every solid voxel pays for
                // the occlusion of a cornice it is nowhere near — which is most of what a cold
                // build of the facility was spending its time on.
                //
                // Only taken when the pieces are a real improvement on the box. Two overlapping
                // pieces of one wall are the wall, and carrying them separately would cost a
                // second test at every voxel to reach the same answer.
                std::vector<Field::Part> parts;
                field.union_children(paint[i].place, parts);
                if (parts.size() > 1 && parts.size() <= kMaxRulePieces) {
                    f64 piece_volume = 0.0;
                    const usize first = rule_piece.size();
                    for (const Field::Part& part : parts) {
                        // The same containment question as the zone's own box above, one level
                        // down, so a zone whose pieces the plain bounds refuse does not lose them
                        // all to the one piece that was scaled.
                        // Moved out of the part's own space, because `union_children` now takes
                        // a shape apart through its translates and rotates as well as its unions
                        // (D722) -- so a piece's box is where it was AUTHORED and every question
                        // asked of it is where the sample is.
                        Field::Aabb box = field.moved_box(
                            part, rule_bounds_used() ? field.containing_bounds_of(part.node)
                                                     : field.bounds_of(part.node));
                        if (box.infinite()) {   // one unbounded piece and the zone is the box again
                            rule_piece.resize(first);
                            piece_volume = -1.0;
                            break;
                        }
                        const f64 grow = reach + part.extra;
                        box.low = Vec3{box.low.x - grow, box.low.y - grow, box.low.z - grow};
                        box.high = Vec3{box.high.x + grow, box.high.y + grow, box.high.z + grow};
                        piece_volume += (box.high.x - box.low.x) * (box.high.y - box.low.y) *
                                        (box.high.z - box.low.z);
                        rule_piece.push_back(box);
                    }
                    const f64 whole = (rule_box[i].high.x - rule_box[i].low.x) *
                                      (rule_box[i].high.y - rule_box[i].low.y) *
                                      (rule_box[i].high.z - rule_box[i].low.z);
                    // Overlap makes the sum an overestimate, which only ever makes this cautious.
                    if (piece_volume < 0.0 || piece_volume > whole * 0.75) {
                        rule_piece.resize(first);
                    }
                }
            }
        }

        const f64 metric = field.metric_slack(paint[i].test);
        if (paint[i].facing_axis >= 3) rule_slack[i] = metric;   // a normal is a per-voxel question

        // Whether the rule can be decided for a whole block from one reading at its centre.
        const bool settleable = metric < Field::kInfiniteSlack;
        if (settleable) {
            widened[i].low -= amplitude;
            widened[i].high += amplitude;
        }

        // Where this rule could possibly say yes.
        //
        // Asked of EVERY rule, including the ones that cannot be settled for a block, because
        // those are two different questions and this used to answer only the first. A rule keyed
        // on a shape the sampler cannot settle — a colonnade behind a `repeat`, a face carrying a
        // pattern — got no box at all and was therefore asked at every solid voxel in the clip,
        // even though the room it names is one per cent of the building. Measured on the facility
        // at eight voxels to the metre: five such rules, one point nine million evaluations each,
        // three quarters of all the paint work in the build.
        //
        // Only a rule bounded ABOVE can be placed. `where=<shape> below=0.02` accepts a small
        // distance, so it accepts only inside that shape or just outside it, and the shape's box
        // grown by the threshold contains every point it could ever paint.
        //
        // A rule bounded below instead — `above=0.5` on a pattern, "where the grain is high" — is
        // the complement of a shape and has no box at all: it can be true anywhere the shape is
        // not, which is most of the world. Those keep the infinite box they start with.
        if (!paint[i].has_place && widened[i].high < 1e29 && widened[i].low <= -1e29) {
            const Field::Aabb inner = field.bounds_of(paint[i].test);
            if (!inner.infinite()) {
                // A settleable rule reaches its accepted band plus its own slack, exactly as
                // before. One that is not settleable has no slack to reach by, so it is charged
                // the clip's displacement instead — the same allowance `bounds_of` has already
                // put round the shape itself, and the only thing that can carry a voxel of that
                // shape outside the box the shape came in.
                const f64 reach = std::max(0.0, widened[i].high) + (settleable ? metric : amplitude);
                rule_box[i].low = Vec3{inner.low.x - reach, inner.low.y - reach,
                                       inner.low.z - reach};
                rule_box[i].high = Vec3{inner.high.x + reach, inner.high.y + reach,
                                        inner.high.z + reach};
            }
        }
    }
    rule_piece_at[paint.size()] = static_cast<u32>(rule_piece.size());

    // The parts the shape is made of, each with its own box and its own allowance. Only usable
    // when every part can be bounded and measured — one part that cannot is one part that could
    // be anywhere, and then the worst case is the only honest answer.
    {
        std::vector<Field::Part> parts;
        field.union_children(prune_root, parts);
        plan.parts_usable = !parts.empty();
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
            // `containing_bounds_of` and not `bounds_of`, and the difference is a real one here.
            //
            // This box answers ONE question: is the sample box near enough to this part to have to
            // allow for what the part can move? That is containment. `bounds_of` refuses to bound
            // anything whose reported distance would then mislead a cull -- a non-uniform scale
            // above all -- and refusing means `everywhere()`, which makes the part near every box
            // in the clip and charges the whole building its slack. Nothing here reads the part's
            // distance, so nothing here needs that promise.
            Field::Aabb box = field.moved_box(piece, field.containing_bounds_of(part));
            if (!box.infinite() && piece.extra > 0.0) {
                const f64 by = piece.extra * 0.5;
                box.low = Vec3{box.low.x - by, box.low.y - by, box.low.z - by};
                box.high = Vec3{box.high.x + by, box.high.y + by, box.high.z + by};
            }
            plan.part_box.push_back(box);
            plan.part_slack.push_back(part_slack);
        }
    }

    // What every sample taken from this plan reports about the clip rather than about its box.
    plan.rules_total = paint.size();
    for (const PaintRule& r : paint) { if (r.has_place) ++plan.rules_placed; }
    for (usize i = 0; i < paint.size(); ++i) {
        if (rule_slack[i] >= Field::kInfiniteSlack && rule_box[i].infinite()) ++plan.rules_per_voxel;
    }

    plan.prune_root = prune_root;
    plan.slack = slack;
    plan.prune_slack = prune_slack;
    plan.amplitude = amplitude;
    plan.widened = std::move(widened);
    plan.rule_slack = std::move(rule_slack);
    plan.rule_box = std::move(rule_box);
    plan.rule_piece = std::move(rule_piece);
    plan.rule_piece_at = std::move(rule_piece_at);
    return plan;
}

SampleResult sample(const SamplePlan& plan, const SampleSettings& settings, JobSystem* jobs,
                    const SampleWatcher& watch) {
    SampleResult result;
    if (!plan.ok()) return result;
    const Field& field = *plan.field;

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
    descent.root = plan.root;
    descent.prune_root = plan.prune_root;
    descent.prune_slack = plan.prune_slack;
    descent.outer_amplitude = plan.amplitude;
    descent.inside_by_default = !settings.has_bounds;
    descent.part_box = &plan.part_box;
    descent.part_slack = &plan.part_slack;
    descent.parts_usable = plan.parts_usable;
    descent.paint = &plan.widened;
    descent.settings = &settings;
    descent.clip = &clip;
    std::vector<std::atomic<u64>> rule_cost(
        settings.count_rule_cost ? plan.widened.size() : 0);
    for (std::atomic<u64>& c : rule_cost) c.store(0, std::memory_order_relaxed);
    if (!rule_cost.empty()) descent.rule_cost = rule_cost.data();

    descent.rule_slack = &plan.rule_slack;
    descent.rule_box = &plan.rule_box;
    if (!plan.rule_piece.empty()) {
        descent.rule_piece = &plan.rule_piece;
        descent.rule_piece_at = &plan.rule_piece_at;
    }
    descent.rule_bounds = plan.rule_bounds;
    descent.voxel = voxel;
    descent.centre_shift = centre_shift;
    descent.slack = plan.slack;
    // Read once, here, so that every box of this sample is cut the same way however the setting
    // moves while it runs.
    descent.block_cells = g_block_cells;
    for (u32 axis = 0; axis < 3; ++axis) {
        descent.lo[axis] = lo[axis];
        descent.size[axis] = size[axis];
    }

    result.rules_total = plan.rules_total;
    result.rules_placed = plan.rules_placed;
    result.rules_per_voxel = plan.rules_per_voxel;
    result.slack = plan.slack;
    result.prune_slack = plan.prune_slack;
    result.parts = plan.parts_usable ? plan.part_slack.size() : 0;

    // The worst part, and how much of the clip it reaches over — which together are the whole
    // story of why a build is slow. A part with a large slack and a small box costs nothing but
    // the boxes beside it; the same slack on a part whose box spans the clip charges every box in
    // the building for it, and the two are indistinguishable from the slack alone.
    result.best_part_slack = 0.0;
    result.worst_part_reach = 0.0;
    const f64 whole = std::max(1e-9, (settings.high.x - settings.low.x) *
                                         (settings.high.y - settings.low.y) *
                                         (settings.high.z - settings.low.z));
    for (usize i = 0; i < plan.part_slack.size(); ++i) {
        if (plan.part_slack[i] <= result.best_part_slack) continue;
        result.best_part_slack = plan.part_slack[i];
        const Field::Aabb& b = plan.part_box[i];
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

    // What the loading bar watches. Counted in blocks rather than in voxels because a block is the
    // unit of work that actually completes: the bar is then monotonic and never has to be revised
    // downward, which is the property that makes people believe it.
    //
    // Blocks are not all equally expensive — one full of matter costs far more than one full of
    // sky — so the fraction is not linear in TIME. That is fine and is why the estimate is
    // computed from elapsed seconds against fraction rather than from the fraction alone: an
    // uneven rate corrects itself within a second or two.
    //
    // Called from every worker at once. `within` and `count` are relaxed atomic stores, so
    // concurrent callers race to write and the loser's value is one block stale, which is a bar
    // one block stale, which nobody can see.
    std::atomic<usize> tops_done{0};
    std::atomic<u64> voxels_done{0};
    const u64 voxels_total = static_cast<u64>(size[0]) * static_cast<u64>(size[1]) *
                             static_cast<u64>(size[2]);

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

            if (watch) {
                const usize done = tops_done.fetch_add(1, std::memory_order_relaxed) + 1;
                const u64 voxels =
                    voxels_done.fetch_add(static_cast<u64>(high[0] - low[0]) *
                                              static_cast<u64>(high[1] - low[1]) *
                                              static_cast<u64>(high[2] - low[2]),
                                          std::memory_order_relaxed);
                watch(static_cast<f64>(done) / static_cast<f64>(top_count), voxels, voxels_total);
            }
        }
    };

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
        result.paint_ns += n.paint_ns;
        result.shape_ns += n.shape_ns;
        result.block_calls += n.blocks;
    }
    result.evaluations = result.shape_evaluations + result.paint_evaluations;
    if (!rule_cost.empty()) {
        result.rule_evaluations.resize(rule_cost.size());
        for (usize i = 0; i < rule_cost.size(); ++i) {
            result.rule_evaluations[i] = rule_cost[i].load(std::memory_order_relaxed);
        }
    }
    clip.build_coarse();
    return result;
}

bool box_may_hold_matter(const SamplePlan& plan, Vec3 low, Vec3 high, i32 voxels_per_metre) {
    if (!plan.ok()) return true;
    const i32 per_metre = (voxels_per_metre > 0) ? voxels_per_metre : kVoxelsPerMetre;
    const f64 voxel = 1.0 / static_cast<f64>(per_metre);

    const Vec3 middle{(low.x + high.x) * 0.5, (low.y + high.y) * 0.5, (low.z + high.z) * 0.5};
    const f64 half[3] = {(high.x - low.x) * 0.5, (high.y - low.y) * 0.5, (high.z - low.z) * 0.5};
    const f64 radius = std::sqrt(half[0] * half[0] + half[1] * half[1] + half[2] * half[2]);

    // What this box has to allow for, by the same reasoning as `slack_here`: only the parts of the
    // shape whose own boxes it is near, and the whole-shape displacement always.
    f64 slack = plan.prune_slack;
    if (plan.parts_usable && !plan.part_box.empty()) {
        f64 worst = 0.0;
        for (usize i = 0; i < plan.part_box.size(); ++i) {
            if (plan.part_slack[i] <= worst) continue;
            if (away_from(plan.part_box[i], middle) > radius) continue;
            worst = plan.part_slack[i];
            if (worst >= Field::kInfiniteSlack) return true;
        }
        slack = worst + plan.amplitude;
    }
    if (slack >= Field::kInfiniteSlack) return true;

    const f64 dc = plan.field->eval(plan.prune_root, middle);
    return !(dc > radius + slack + voxel * kHalfCellDiagonal);
}

// One sample from a clip that is only sampled once. The ladder, the instrument and anything else
// taking more than one from the same field wants the two halves apart.
SampleResult sample(const Field& field, u32 root, const std::vector<PaintRule>& paint,
                    const SampleSettings& settings, JobSystem* jobs, const SampleWatcher& watch) {
    return sample(plan_sample(field, root, paint), settings, jobs, watch);
}

// --------------------------------------------------------------------------------------
// One node, as a box and a resolution. The block above these in sample.hpp says why the three of
// them are one place rather than three callers.
// --------------------------------------------------------------------------------------

NodeBox node_box(const NodeKey& key) {
    // Exact in binary at every level this can be asked about: the span is a power of two over
    // another power of two, so a node's corner is never a rounding away from the voxel grid the
    // sampler floors against. That matters more than it looks -- `sample` takes the box's corners
    // to voxels with a floor, and a corner half an epsilon low is a clip one voxel wide of where
    // its node is, for ever, in a file.
    const f64 span = static_cast<f64>(i64{1} << key.level) / static_cast<f64>(kVoxelsPerMetre);
    NodeBox box;
    box.low = {static_cast<f64>(key.x) * span, static_cast<f64>(key.y) * span,
               static_cast<f64>(key.z) * span};
    box.high = {box.low.x + span, box.low.y + span, box.low.z + span};
    return box;
}

SampleSettings node_sample_settings(const SampleSettings& base, const NodeKey& key) {
    const NodeBox box = node_box(key);
    SampleSettings settings = base;
    settings.low = box.low;
    settings.high = box.high;
    settings.voxels_per_metre = node_voxels_per_metre(key.level);
    return settings;
}

bool node_block(const SampleResult& sampled, const NodeKey& key, VoxelTypeId* types, u8* inside) {
    const i32 per_metre = node_voxels_per_metre(key.level);
    if (per_metre <= 0 || sampled.clip.empty()) return false;

    // Where this node starts in the sample's own voxel grid. The node spans metres
    // [c << level, (c+1) << level) / 32 and the sample is at 256 / 2^level voxels a metre, so the
    // product is c * 8 exactly -- a node lands on an eight-voxel boundary of any sample taken at
    // its own level, whatever that sample's own origin is, and nothing here has to round.
    const i64 first[3] = {key.x * kNodeVoxels, key.y * kNodeVoxels, key.z * kNodeVoxels};
    i64 at[3];
    for (u32 axis = 0; axis < 3; ++axis) {
        at[axis] = first[axis] - sampled.origin_voxel[axis];
        if (at[axis] < 0 || at[axis] + kNodeVoxels > sampled.clip.size[axis]) return false;
    }

    for (i32 z = 0; z < kNodeVoxels; ++z) {
        for (i32 y = 0; y < kNodeVoxels; ++y) {
            for (i32 x = 0; x < kNodeVoxels; ++x) {
                const usize from = sampled.clip.index(static_cast<i32>(at[0]) + x,
                                                      static_cast<i32>(at[1]) + y,
                                                      static_cast<i32>(at[2]) + z);
                const usize to = static_cast<usize>(x) +
                                 static_cast<usize>(y) * kNodeVoxels +
                                 static_cast<usize>(z) * kNodeVoxels * kNodeVoxels;
                if (types != nullptr) types[to] = sampled.clip.voxels[from];
                if (inside != nullptr) inside[to] = sampled.clip.inside[from];
            }
        }
    }
    return true;
}

}  // namespace forge
}  // namespace ws
