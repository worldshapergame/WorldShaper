#include "game/clip.hpp"

#include <algorithm>
#include <cmath>

#include "core/hash.hpp"
#include "world/greedy.hpp"
#include "world/world.hpp"

namespace ws {
namespace {

constexpr f64 kPi = 3.14159265358979323846;

// A clip plus the coordinate its (0,0,0) cell sits at, so the shears can move the array
// around without losing track of where the rotation centre is.
struct Frame {
    Clip clip;
    f64 origin[3]{0.0, 0.0, 0.0};
};

// Moves whole slices along `moving`, by an amount that depends only on `driving`. Whole
// slices, so this is a bijection: nothing merges, nothing is left behind.
void shear(Frame& frame, u32 moving, u32 driving, const std::vector<i32>& delta) {
    if (delta.empty()) return;
    const i32 lowest = *std::min_element(delta.begin(), delta.end());
    const i32 highest = *std::max_element(delta.begin(), delta.end());
    if (lowest == 0 && highest == 0) return;

    Clip& source = frame.clip;
    Clip result;
    for (u32 axis = 0; axis < 3; ++axis) result.size[axis] = source.size[axis];
    result.size[moving] += highest - lowest;
    result.voxels.assign(static_cast<usize>(result.cell_count()), kAir);
    result.inside.assign(static_cast<usize>(result.cell_count()), 0);

    i32 at[3];
    for (at[2] = 0; at[2] < source.size[2]; ++at[2]) {
        for (at[1] = 0; at[1] < source.size[1]; ++at[1]) {
            for (at[0] = 0; at[0] < source.size[0]; ++at[0]) {
                if (!source.covered(at[0], at[1], at[2])) continue;
                i32 to[3] = {at[0], at[1], at[2]};
                to[moving] += delta[static_cast<usize>(at[driving])] - lowest;
                const usize target = result.index(to[0], to[1], to[2]);
                result.voxels[target] = source.at(at[0], at[1], at[2]);
                result.inside[target] = 1;
            }
        }
    }

    frame.origin[moving] += static_cast<f64>(lowest);
    frame.clip = std::move(result);
}

// One shear stage of the Paeth decomposition. `factor` is the shear coefficient and the
// offset is measured from the rotation centre, so the clip turns about its middle.
void shear_stage(Frame& frame, u32 moving, u32 driving, f64 factor, f64 centre) {
    std::vector<i32> delta(static_cast<usize>(frame.clip.size[driving]));
    for (usize i = 0; i < delta.size(); ++i) {
        const f64 coordinate = frame.origin[driving] + static_cast<f64>(i) + 0.5 - centre;
        delta[i] = static_cast<i32>(std::lround(factor * coordinate));
    }
    shear(frame, moving, driving, delta);
}

// A quarter turn, done by permuting and flipping axes. Exact, and the only way to get past
// the ±45° window the shears are good for.
Clip quarter_turn(const Clip& clip, u32 axis) {
    const u32 u = (axis + 1) % 3;
    const u32 v = (axis + 2) % 3;

    Clip result;
    for (u32 a = 0; a < 3; ++a) result.size[a] = clip.size[a];
    result.size[u] = clip.size[v];
    result.size[v] = clip.size[u];
    result.voxels.assign(static_cast<usize>(result.cell_count()), kAir);
    result.inside.assign(static_cast<usize>(result.cell_count()), 0);

    i32 at[3];
    for (at[2] = 0; at[2] < clip.size[2]; ++at[2]) {
        for (at[1] = 0; at[1] < clip.size[1]; ++at[1]) {
            for (at[0] = 0; at[0] < clip.size[0]; ++at[0]) {
                i32 to[3] = {at[0], at[1], at[2]};
                // (u, v) -> (-v, u) in the plane perpendicular to the axis.
                to[u] = clip.size[v] - 1 - at[v];
                to[v] = at[u];
                const usize target = result.index(to[0], to[1], to[2]);
                result.voxels[target] = clip.voxels[clip.index(at[0], at[1], at[2])];
                result.inside[target] = clip.inside[clip.index(at[0], at[1], at[2])];
            }
        }
    }
    result.build_coarse();
    return result;
}

}  // namespace

const char* paste_mode_name(PasteMode mode) {
    switch (mode) {
        case PasteMode::Replace:   return "replace everything";
        case PasteMode::SolidOnly: return "matter only";
        case PasteMode::IntoAir:   return "into empty space only";
        default:                   return "?";
    }
}

void Clip::build_coarse() {
    for (u32 a = 0; a < 3; ++a) coarse_size[a] = (size[a] + 7) / 8;
    coarse.assign(static_cast<usize>(coarse_count()), 0);
    if (empty()) return;
    for (i32 z = 0; z < size[2]; ++z) {
        for (i32 y = 0; y < size[1]; ++y) {
            for (i32 x = 0; x < size[0]; ++x) {
                const usize cell = index(x, y, z);
                if (inside[cell] == 0 || voxels[cell] == kAir) continue;
                coarse[coarse_index(x >> 3, y >> 3, z >> 3)] = 1;
            }
        }
    }
}

u64 Clip::solid_count() const {
    u64 total = 0;
    for (usize i = 0; i < voxels.size(); ++i) {
        if (inside[i] != 0 && voxels[i] != kAir) ++total;
    }
    return total;
}

u64 Clip::covered_count() const {
    u64 total = 0;
    for (u8 flag : inside) total += (flag != 0) ? 1 : 0;
    return total;
}

u64 Clip::content_hash() const {
    u64 h = hash_mix(static_cast<u64>(size[0]));
    h = hash_combine(h, static_cast<u64>(size[1]));
    h = hash_combine(h, static_cast<u64>(size[2]));
    for (usize i = 0; i < voxels.size(); ++i) {
        h = hash_combine(h, (inside[i] != 0) ? (static_cast<u64>(voxels[i]) + 1) : 0);
    }
    return h;
}

Clip capture_clip(const World& world, i64 x0, i64 y0, i64 z0, i64 x1, i64 y1, i64 z1) {
    if (x0 > x1) std::swap(x0, x1);
    if (y0 > y1) std::swap(y0, y1);
    if (z0 > z1) std::swap(z0, z1);

    Clip clip;
    clip.size[0] = static_cast<i32>(x1 - x0 + 1);
    clip.size[1] = static_cast<i32>(y1 - y0 + 1);
    clip.size[2] = static_cast<i32>(z1 - z0 + 1);
    clip.voxels.assign(static_cast<usize>(clip.cell_count()), kAir);
    clip.inside.assign(static_cast<usize>(clip.cell_count()), 1);

    // Walked brick by brick with the chunk pointer cached, for the same reason apply_op is:
    // a per-voxel World::get over a five-metre selection is two orders of magnitude short.
    const Chunk* chunk = nullptr;
    ChunkCoord chunk_coord{};
    bool chunk_valid = false;
    VoxelTypeId decoded[kBrickVoxels];
    const Brick* decoded_from = nullptr;

    for (i32 z = 0; z < clip.size[2]; ++z) {
        for (i32 y = 0; y < clip.size[1]; ++y) {
            for (i32 x = 0; x < clip.size[0]; ++x) {
                const i64 wx = x0 + x, wy = y0 + y, wz = z0 + z;
                const ChunkCoord cc = chunk_coord_of(wx, wy, wz);
                if (!chunk_valid || !(cc == chunk_coord)) {
                    chunk_coord = cc;
                    chunk = world.chunk(cc);
                    chunk_valid = true;
                    decoded_from = nullptr;
                }
                if (chunk == nullptr) continue;   // stays air
                const Brick* brick =
                    chunk->brick(local_of(wx) >> 3, local_of(wy) >> 3, local_of(wz) >> 3);
                if (brick == nullptr) continue;
                if (brick != decoded_from) {
                    brick->decode(decoded);
                    decoded_from = brick;
                }
                clip.voxels[clip.index(x, y, z)] =
                    decoded[brick_index(static_cast<u32>(wx & 7), static_cast<u32>(wy & 7),
                                        static_cast<u32>(wz & 7))];
            }
        }
    }
    clip.build_coarse();
    return clip;
}

Clip rotate_clip(const Clip& clip, u32 axis, f64 radians) {
    if (clip.empty()) return clip;

    // Fold into (-45°, 45°] plus a whole number of quarter turns. tan(θ/2) runs away as θ
    // approaches half a turn, and a shear by a huge coefficient is a smear, not a rotation.
    //
    // The subtraction uses the *unmasked* count and the mask comes after. Doing it the other
    // way round is a bug that only shows above 315°: four quarters masks to zero, so nothing
    // is subtracted, and the shears are handed an angle near a full turn. On screen that is
    // a row of copies rotating smoothly and then all collapsing at once.
    const f64 turn = 2.0 * kPi;
    f64 angle = std::fmod(radians, turn);
    if (angle < 0.0) angle += turn;
    const i32 nearest_quarter = static_cast<i32>(std::lround(angle / (kPi * 0.5)));
    angle -= static_cast<f64>(nearest_quarter) * kPi * 0.5;
    const i32 quarters = nearest_quarter & 3;

    Clip turned = clip;
    for (i32 i = 0; i < quarters; ++i) turned = quarter_turn(turned, axis);
    if (std::abs(angle) < 1e-9) return turned;

    const u32 u = (axis + 1) % 3;
    const u32 v = (axis + 2) % 3;

    Frame frame;
    frame.clip = std::move(turned);
    const f64 centre_u = static_cast<f64>(frame.clip.size[u]) * 0.5;
    const f64 centre_v = static_cast<f64>(frame.clip.size[v]) * 0.5;

    const f64 alpha = -std::tan(angle * 0.5);
    const f64 beta = std::sin(angle);

    shear_stage(frame, u, v, alpha, centre_v);
    shear_stage(frame, v, u, beta, centre_u);
    shear_stage(frame, u, v, alpha, centre_v);

    // Trim the empty margin the shears left around the edges. Cells outside the clip are
    // not air, they are nothing, so dropping them changes no content.
    const Clip& grown = frame.clip;
    i32 lo[3] = {grown.size[0], grown.size[1], grown.size[2]};
    i32 hi[3] = {-1, -1, -1};
    for (i32 z = 0; z < grown.size[2]; ++z) {
        for (i32 y = 0; y < grown.size[1]; ++y) {
            for (i32 x = 0; x < grown.size[0]; ++x) {
                if (!grown.covered(x, y, z)) continue;
                const i32 at[3] = {x, y, z};
                for (u32 a = 0; a < 3; ++a) {
                    lo[a] = std::min(lo[a], at[a]);
                    hi[a] = std::max(hi[a], at[a]);
                }
            }
        }
    }
    if (hi[0] < lo[0]) return Clip{};

    Clip result;
    for (u32 a = 0; a < 3; ++a) result.size[a] = hi[a] - lo[a] + 1;
    result.voxels.assign(static_cast<usize>(result.cell_count()), kAir);
    result.inside.assign(static_cast<usize>(result.cell_count()), 0);
    for (i32 z = lo[2]; z <= hi[2]; ++z) {
        for (i32 y = lo[1]; y <= hi[1]; ++y) {
            for (i32 x = lo[0]; x <= hi[0]; ++x) {
                const usize from = grown.index(x, y, z);
                const usize to = result.index(x - lo[0], y - lo[1], z - lo[2]);
                result.voxels[to] = grown.voxels[from];
                result.inside[to] = grown.inside[from];
            }
        }
    }
    result.build_coarse();
    return result;
}

Clip rotate_clip(const Clip& clip, const f64 radians[3]) {
    Clip result = clip;
    for (u32 axis = 0; axis < 3; ++axis) {
        if (std::abs(radians[axis]) > 1e-9) result = rotate_clip(result, axis, radians[axis]);
    }
    return result;
}

Clip mirror_clip(const Clip& clip, u32 axis) {
    Clip result = clip;
    i32 at[3];
    for (at[2] = 0; at[2] < clip.size[2]; ++at[2]) {
        for (at[1] = 0; at[1] < clip.size[1]; ++at[1]) {
            for (at[0] = 0; at[0] < clip.size[0]; ++at[0]) {
                i32 to[3] = {at[0], at[1], at[2]};
                to[axis] = clip.size[axis] - 1 - at[axis];
                const usize target = result.index(to[0], to[1], to[2]);
                const usize source = clip.index(at[0], at[1], at[2]);
                result.voxels[target] = clip.voxels[source];
                result.inside[target] = clip.inside[source];
            }
        }
    }
    result.build_coarse();
    return result;
}

Clip scale_clip(const Clip& clip, const f64 factor[3]) {
    if (clip.empty()) return clip;
    f64 wanted[3];
    bool identity = true;
    for (u32 a = 0; a < 3; ++a) {
        // No ceiling; the floor is one voxel on the axis.
        wanted[a] = std::max(factor[a], 1.0 / std::max(1.0, static_cast<f64>(clip.size[a])));
        if (std::abs(wanted[a] - 1.0) > 1e-9) identity = false;
    }
    if (identity) return clip;

    Clip out;
    for (u32 a = 0; a < 3; ++a) {
        out.size[a] = std::max(
            1, static_cast<i32>(std::llround(static_cast<f64>(clip.size[a]) * wanted[a])));
    }
    out.voxels.assign(static_cast<usize>(out.cell_count()), kAir);
    out.inside.assign(static_cast<usize>(out.cell_count()), 0);
    // The occupancy mask is filled as we go rather than by a second walk over every cell.
    // At the sizes a resize reaches, one avoided pass over several million cells is worth
    // more than the tidiness of building it separately.
    for (u32 a = 0; a < 3; ++a) out.coarse_size[a] = (out.size[a] + 7) / 8;
    out.coarse.assign(static_cast<usize>(out.coarse_count()), 0);

    // Destination-driven: every output voxel asks which part of the input maps onto it.
    // The other way round — pushing input voxels forward and rounding — is what leaves
    // stripes of holes when the ratio is not a whole number.
    f64 span[3];
    for (u32 a = 0; a < 3; ++a) {
        span[a] = static_cast<f64>(clip.size[a]) / static_cast<f64>(out.size[a]);
    }

    std::vector<Clip::TypeTally> tally;
    for (i32 z = 0; z < out.size[2]; ++z) {
        for (i32 y = 0; y < out.size[1]; ++y) {
            for (i32 x = 0; x < out.size[0]; ++x) {
                const i32 at[3] = {x, y, z};
                i32 lo[3];
                i32 hi[3];
                for (u32 a = 0; a < 3; ++a) {
                    lo[a] = std::clamp(static_cast<i32>(std::floor(at[a] * span[a])), 0,
                                       clip.size[a] - 1);
                    // Inclusive, and never empty: when the region is under a voxel wide the
                    // answer is the single voxel it lands in, which is what makes growing by
                    // a whole number reproduce each voxel exactly.
                    hi[a] = std::clamp(
                        static_cast<i32>(std::ceil((at[a] + 1) * span[a])) - 1, lo[a],
                        clip.size[a] - 1);
                }

                // Growing: the region is under a voxel wide, so there is exactly one source
                // voxel and nothing to count. This is the common case — it is every output
                // cell of every enlargement — and taking the counting path for it was most
                // of what made holding the resize key stutter.
                if (lo[0] == hi[0] && lo[1] == hi[1] && lo[2] == hi[2]) {
                    const usize from = clip.index(lo[0], lo[1], lo[2]);
                    if (clip.inside[from] == 0) continue;
                    const usize to = out.index(x, y, z);
                    out.voxels[to] = clip.voxels[from];
                    out.inside[to] = 1;
                    if (clip.voxels[from] != kAir) {
                        out.coarse[out.coarse_index(x >> 3, y >> 3, z >> 3)] = 1;
                    }
                    continue;
                }

                tally.clear();
                bool covered = false;
                bool any_matter = false;
                for (i32 sz = lo[2]; sz <= hi[2]; ++sz) {
                    for (i32 sy = lo[1]; sy <= hi[1]; ++sy) {
                        for (i32 sx = lo[0]; sx <= hi[0]; ++sx) {
                            if (!clip.covered(sx, sy, sz)) continue;
                            covered = true;
                            const VoxelTypeId type = clip.at(sx, sy, sz);
                            if (type == kAir) continue;   // air never votes; see below
                            any_matter = true;
                            bool found = false;
                            for (Clip::TypeTally& entry : tally) {
                                if (entry.type == type) {
                                    ++entry.count;
                                    found = true;
                                    break;
                                }
                            }
                            if (!found) tally.push_back({type, 1});
                        }
                    }
                }
                if (!covered) continue;

                // Air does not get a vote, and that is the whole difference between a resize
                // that keeps a shape and one that eats it.
                //
                // A plain majority deletes exactly the things worth keeping. Shrink a
                // one-voxel-thick diagonal by half and every block it passes through is
                // seven-eighths air, so the majority is air and the slope disappears. The
                // same arithmetic shaves the corner off a right angle — a corner voxel is one
                // of eight — and thins a one-voxel wall to nothing.
                //
                // So: if any matter falls in the region, the answer is matter, and which kind
                // is decided among the materials present. The cost is that a shape can gain a
                // little bulk rather than lose pieces, and small gaps can close. That is the
                // right way round to be wrong: a slope that survives slightly chunky is still
                // the slope you drew; a slope that vanished is not.
                //
                // Ties go to the lowest type id, so two machines resizing the same clip
                // agree — deterministic for the same reason every other edit is.
                VoxelTypeId best = kAir;
                u32 best_count = 0;
                if (any_matter) {
                    for (const Clip::TypeTally& entry : tally) {
                        if (entry.count > best_count ||
                            (entry.count == best_count && entry.type < best)) {
                            best = entry.type;
                            best_count = entry.count;
                        }
                    }
                }
                const usize to = out.index(x, y, z);
                out.voxels[to] = best;
                out.inside[to] = 1;
                if (best != kAir) out.coarse[out.coarse_index(x >> 3, y >> 3, z >> 3)] = 1;
            }
        }
    }
    return out;
}

u64 clip_to_ops(const Clip& clip, i64 ox, i64 oy, i64 oz, PasteMode mode, u64 tick, u32 player,
                std::vector<Op>& out) {
    if (clip.empty()) return 0;
    const u64 before = out.size();

    // Cells the mode says nothing about start claimed, so the box grower steps over them.
    std::vector<u8> claimed(clip.voxels.size(), 0);
    for (usize i = 0; i < claimed.size(); ++i) {
        const bool covered = clip.inside[i] != 0;
        const bool matter = clip.voxels[i] != kAir;
        const bool write = covered && (mode == PasteMode::Replace || matter);
        claimed[i] = write ? u8{0} : u8{1};
    }

    const WriteMask mask =
        (mode == PasteMode::IntoAir) ? WriteMask::IntoAir : WriteMask::All;

    greedy_boxes(clip.voxels.data(), clip.size[0], clip.size[1], clip.size[2], claimed,
                 [&](VoxelTypeId value, i32 x0, i32 y0, i32 z0, i32 x1, i32 y1, i32 z1) {
                     out.push_back(Op::fill_box(
                         tick, player, ox + x0, oy + y0, oz + z0, ox + x1, oy + y1, oz + z1,
                         value,
                         (value == kAir) ? MatterReason::PlayerBreak : MatterReason::PlayerPlace,
                         mask));
                 });
    return out.size() - before;
}

}  // namespace ws
