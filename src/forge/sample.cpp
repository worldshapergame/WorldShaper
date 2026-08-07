#include "forge/sample.hpp"

#include <algorithm>
#include <cmath>

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
