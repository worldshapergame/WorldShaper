#include "forge/sample.hpp"

#include <algorithm>
#include <cmath>
#include <map>

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

VariationReport apply_variation(Clip& clip, VoxelTypeTable& types, const Field& field,
                                const Variation& variation, const SampleSettings& settings,
                                const SampleResult& placed) {
    VariationReport report;
    if (clip.empty() || !variation.any()) return report;

    const i32 per_metre = (settings.voxels_per_metre > 0) ? settings.voxels_per_metre
                                                          : kVoxelsPerMetre;
    const f64 voxel = 1.0 / static_cast<f64>(per_metre);
    const f64 centre_shift = settings.sample_at_centre ? 0.5 : 0.0;

    // One cache per base type, so a wall of one material does not re-intern the same variant
    // millions of times. The key is the quantised perturbation, which is what makes the count of
    // distinct records finite and knowable rather than a matter of luck.
    std::map<u64, VoxelTypeId> made;
    std::map<VoxelTypeId, u64> tally;

    for (i32 z = 0; z < clip.size[2]; ++z) {
        for (i32 y = 0; y < clip.size[1]; ++y) {
            for (i32 x = 0; x < clip.size[0]; ++x) {
                const usize index = clip.index(x, y, z);
                const VoxelTypeId base = clip.voxels[index];
                if (base == kAir) continue;
                ++report.voxels;

                const i64 wx = placed.origin_voxel[0] + x;
                const i64 wy = placed.origin_voxel[1] + y;
                const i64 wz = placed.origin_voxel[2] + z;

                f64 scale = 1.0;
                if (variation.has_by) {
                    const Vec3 p{(static_cast<f64>(wx) + centre_shift) * voxel,
                                 (static_cast<f64>(wy) + centre_shift) * voxel,
                                 (static_cast<f64>(wz) + centre_shift) * voxel};
                    scale = std::clamp(field.eval(variation.by, p), 0.0, 1.0);
                }
                if (scale <= 0.0) {
                    ++tally[base];
                    continue;
                }

                const VisualRecord& source = types.visual_of(base);
                VisualRecord record = source;
                const f64 c = variation.colour * scale;
                const f64 r = variation.roughness * scale;
                record.red = nudge(source.red, c, hash_signed(wx, wy, wz, variation.seed));
                record.green = nudge(source.green, c, hash_signed(wx, wy, wz, variation.seed + 71u));
                record.blue = nudge(source.blue, c, hash_signed(wx, wy, wz, variation.seed + 149u));
                record.roughness =
                    nudge(source.roughness, r, hash_signed(wx, wy, wz, variation.seed + 227u));

                const u64 key = (static_cast<u64>(base) << 40) |
                                (static_cast<u64>(record.red) << 32) |
                                (static_cast<u64>(record.green) << 24) |
                                (static_cast<u64>(record.blue) << 16) |
                                (static_cast<u64>(record.roughness) << 8);
                auto it = made.find(key);
                VoxelTypeId variant;
                if (it != made.end()) {
                    variant = it->second;
                } else if (made.size() < variation.budget) {
                    variant = types.intern(record, types.behaviour_of(base));
                    made.emplace(key, variant);
                } else {
                    // The budget is spent. Reuse the nearest record already made for this base
                    // rather than minting another, which keeps the surface varied — just no
                    // longer uniquely so — and keeps the renderer's table within its buffer.
                    ++report.reused;
                    auto near = made.lower_bound(key);
                    if (near == made.end()) --near;
                    variant = near->second;
                }
                clip.voxels[index] = variant;
                ++tally[variant];
            }
        }
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

    std::vector<u64> counted(static_cast<usize>(size[2]), 0);

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
    const f64 slack = field.skip_slack();

    const auto do_slab = [&](usize z_begin, usize z_end) {
        u64 local = 0;
        for (usize zi = z_begin; zi < z_end; ++zi) {
            const i32 z = static_cast<i32>(zi);
            const f64 pz = (static_cast<f64>(lo[2] + z) + centre_shift) * voxel;
            for (i32 y = 0; y < size[1]; ++y) {
                const f64 py = (static_cast<f64>(lo[1] + y) + centre_shift) * voxel;
                for (i32 x = 0; x < size[0]; ++x) {
                    const f64 px = (static_cast<f64>(lo[0] + x) + centre_shift) * voxel;
                    const Vec3 p{px, py, pz};
                    const usize index = clip.index(x, y, z);

                    // Is this cell part of the clip at all?
                    bool covered = true;
                    if (settings.has_bounds) {
                        covered = field.eval(settings.bounds, p) <= 0.0;
                        ++local;
                    }
                    if (!covered) continue;
                    clip.inside[index] = 1;

                    // Is there matter here?
                    const f64 d = field.eval(root, p);
                    ++local;
                    if (d > 0.0) {
                        // Nothing here, and nothing for a while.
                        //
                        // The cells jumped over still have to be marked as part of the clip. They
                        // are empty, but empty *inside the clip* means "this cell is air and
                        // stamping should clear whatever is there", where a cell outside the clip
                        // is none of its business. Skipping the mark as well as the evaluation
                        // left ragged holes in the mask — invisible in the voxels, and visible as
                        // torn edges the moment a slice was printed.
                        const f64 clear = d - slack;
                        if (clear > voxel) {
                            const i32 jump = static_cast<i32>(clear / voxel);
                            if (jump > 1) {
                                const i32 last = std::min(x + jump - 1, size[0] - 1);
                                if (!settings.has_bounds) {
                                    for (i32 fill = x + 1; fill <= last; ++fill) {
                                        clip.inside[clip.index(fill, y, z)] = 1;
                                    }
                                    x = last;
                                }
                            }
                        }
                        continue;
                    }

                    // What is it made of? Later rules paint over earlier ones, so the list reads
                    // as a stack of coats.
                    VoxelTypeId type = kAir;
                    Vec3 normal{0, 0, 0};
                    bool have_normal = false;
                    for (const PaintRule& rule : paint) {
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
                    // A cell with matter in it and no rule that matched is still matter — it
                    // would be worse to silently drop it than to give it the first type asked
                    // for, because a hole in a wall is harder to notice than a wrong colour.
                    if (type == kAir && !paint.empty()) type = paint.front().type;
                    clip.voxels[index] = type;
                }
            }
            counted[zi] = local;
            local = 0;
        }
    };

    (void)wants_normal;
    if (jobs != nullptr && size[2] > 1) {
        jobs->parallel_for(static_cast<usize>(size[2]), 1, do_slab);
    } else {
        do_slab(0, static_cast<usize>(size[2]));
    }

    for (u64 n : counted) result.evaluations += n;
    clip.build_coarse();
    return result;
}

}  // namespace forge
}  // namespace ws
