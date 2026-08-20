#include "game/clip.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_map>

#include "core/hash.hpp"
#include "core/jobs.hpp"
#include "world/greedy.hpp"
#include "world/world.hpp"

namespace ws {

Clip hollow_clip(const Clip& clip, i64 thickness) {
    if (thickness <= 0) return clip;

    const i64 sx = static_cast<i64>(clip.size[0]);
    const i64 sy = static_cast<i64>(clip.size[1]);
    const i64 sz = static_cast<i64>(clip.size[2]);
    if (sx <= 0 || sy <= 0 || sz <= 0) return clip;

    const usize count = static_cast<usize>(sx * sy * sz);
    auto index = [&](i64 x, i64 y, i64 z) { return static_cast<usize>((z * sy + y) * sx + x); };

    // Erode: a cell survives only if everything within `thickness` of it is also solid. Cells
    // outside the clip count as empty, which is what makes the outer surface part of the shell.
    std::vector<u8> solid(count, 0);
    for (usize i = 0; i < count && i < clip.inside.size(); ++i) {
        solid[i] = (clip.inside[i] != 0 && clip.voxels[i] != kAir) ? 1u : 0u;
    }

    std::vector<u8> a = solid;
    std::vector<u8> b(count, 0);

    for (u32 axis = 0; axis < 3; ++axis) {
        for (i64 z = 0; z < sz; ++z) {
            for (i64 y = 0; y < sy; ++y) {
                for (i64 x = 0; x < sx; ++x) {
                    u8 keep = 1;
                    for (i64 d = -thickness; d <= thickness && keep != 0; ++d) {
                        const i64 nx = x + (axis == 0 ? d : 0);
                        const i64 ny = y + (axis == 1 ? d : 0);
                        const i64 nz = z + (axis == 2 ? d : 0);
                        if (nx < 0 || ny < 0 || nz < 0 || nx >= sx || ny >= sy || nz >= sz) {
                            keep = 0;   // outside is empty, so the rim is always shell
                            break;
                        }
                        if (a[index(nx, ny, nz)] == 0) keep = 0;
                    }
                    b[index(x, y, z)] = keep;
                }
            }
        }
        a.swap(b);
    }

    // The shell is what was solid and did not survive the erosion.
    Clip out = clip;
    for (usize i = 0; i < count && i < out.inside.size(); ++i) {
        if (solid[i] != 0 && a[i] != 0) {
            out.inside[i] = 0;
            out.voxels[i] = kAir;
        }
    }
    out.build_coarse();
    return out;
}
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

namespace {

// What one chunk's worth of the paste turned out to be, so the ledger can be charged after the
// threads have finished rather than from inside them.
//
// Keyed on the pair, because "matter went from air to type 41" and "matter went from type 12 to
// type 41" are different events to an audit that exists to prove nothing appears from nowhere.
//
// Open-addressed, and that is the whole reason this type exists rather than an unordered_map.
// Under the no-two-voxels-alike rule a paste produces a million distinct pairs, and a node-based
// map means a million heap allocations scattered across memory — which measured as most of the
// paste, for bookkeeping nobody looks at until an audit runs.
struct LedgerDelta {
    std::vector<u64> keys;     // 0 is empty; a real pair always has a non-air side
    std::vector<u64> counts;
    u64 mask = 0;
    usize filled = 0;

    LedgerDelta() {
        keys.assign(4096, 0);
        counts.assign(4096, 0);
        mask = keys.size() - 1;
    }

    void grow() {
        std::vector<u64> old_keys;
        std::vector<u64> old_counts;
        old_keys.swap(keys);
        old_counts.swap(counts);
        keys.assign(old_keys.size() * 2, 0);
        counts.assign(old_keys.size() * 2, 0);
        mask = keys.size() - 1;
        for (usize i = 0; i < old_keys.size(); ++i) {
            if (old_keys[i] == 0) continue;
            u64 at = (old_keys[i] * 0x9E3779B97F4A7C15ull) >> 32 & mask;
            while (keys[at] != 0) at = (at + 1) & mask;
            keys[at] = old_keys[i];
            counts[at] = old_counts[i];
        }
    }

    void add(VoxelTypeId from, VoxelTypeId to, u64 count) {
        const u64 key = (static_cast<u64>(from) << 32) | static_cast<u64>(to);
        u64 at = (key * 0x9E3779B97F4A7C15ull) >> 32 & mask;
        while (keys[at] != 0 && keys[at] != key) at = (at + 1) & mask;
        if (keys[at] == 0) {
            keys[at] = key;
            ++filled;
        }
        counts[at] += count;
        if (filled * 2 > keys.size()) grow();
    }
};

}  // namespace

PasteStats paste_clip(World& world, MatterLedger& ledger, const Clip& clip, i64 ox, i64 oy,
                      i64 oz, PasteMode mode, MatterReason reason, u32 player, JobSystem* jobs,
                      usize type_count, u32 scale, bool drop_empty) {
    PasteStats stats;
    if (clip.empty()) return stats;

    // How many world voxels one clip cell covers, as a shift. Anything that is not a power of two
    // is rounded down to one, because a non-shift would put a division in the innermost loop of the
    // one function in the engine that writes voxels.
    u32 shift = 0;
    while ((1u << (shift + 1)) <= scale) ++shift;
    if ((1u << shift) != scale) shift = 0;

    const i64 lo[3] = {ox, oy, oz};
    const i64 hi[3] = {ox + (static_cast<i64>(clip.size[0]) << shift) - 1,
                       oy + (static_cast<i64>(clip.size[1]) << shift) - 1,
                       oz + (static_cast<i64>(clip.size[2]) << shift) - 1};

    const WriteMask mask = (mode == PasteMode::IntoAir) ? WriteMask::IntoAir : WriteMask::All;
    const bool clears = (mode == PasteMode::Replace);   // the clip's empty parts erase

    // Which chunks the clip reaches into, and a pointer to each.
    //
    // Creating them all here, on one thread, is what makes the fill below safe to run in
    // parallel: the map is never touched again, and World holds its chunks in a node-based
    // map, so a Chunk* stays valid however many more are inserted.
    struct Target {
        ChunkCoord coord;
        Chunk* chunk;
    };
    std::vector<Target> targets;
    for (i64 cz = chunk_of(lo[2]); cz <= chunk_of(hi[2]); ++cz) {
        for (i64 cy = chunk_of(lo[1]); cy <= chunk_of(hi[1]); ++cy) {
            for (i64 cx = chunk_of(lo[0]); cx <= chunk_of(hi[0]); ++cx) {
                const ChunkCoord coord{cx, cy, cz};
                // A chunk the clip only covers with air, in a mode where air writes nothing,
                // must not be brought into existence. An empty chunk that exists is not the
                // same as one that does not: it costs memory, it streams, and it makes the
                // chunk count a lie.
                if (!clears) {
                    const i64 base[3] = {cx << 8, cy << 8, cz << 8};
                    bool any = false;
                    // Asked of the coarse mask, one byte per 8³ block, so this is a few
                    // thousand reads for a chunk rather than sixteen million.
                    const i64 c0[3] = {std::max(lo[0], base[0]) - ox,
                                       std::max(lo[1], base[1]) - oy,
                                       std::max(lo[2], base[2]) - oz};
                    const i64 c1[3] = {std::min(hi[0], base[0] + 255) - ox,
                                       std::min(hi[1], base[1] + 255) - oy,
                                       std::min(hi[2], base[2] + 255) - oz};
                    // The mask is one byte per eight-cubed block OF THE CLIP, so a world offset
                    // becomes a clip offset (>> shift) before it becomes a block (>> 3).
                    const u32 block = shift + 3;
                    for (i64 z = c0[2] >> block; z <= (c1[2] >> block) && !any; ++z) {
                        for (i64 y = c0[1] >> block; y <= (c1[1] >> block) && !any; ++y) {
                            for (i64 x = c0[0] >> block; x <= (c1[0] >> block) && !any; ++x) {
                                any = clip.coarse[clip.coarse_index(
                                          static_cast<i32>(x), static_cast<i32>(y),
                                          static_cast<i32>(z))] != 0;
                            }
                        }
                    }
                    if (!any) continue;
                }
                targets.push_back({coord, &world.chunk_for_write(coord)});
            }
        }
    }
    if (targets.empty()) return stats;
    stats.chunks_touched = targets.size();

    std::vector<LedgerDelta> deltas(targets.size());
    std::vector<u64> changed(targets.size(), 0);
    std::vector<u64> written(targets.size(), 0);
    std::vector<u64> emptied(targets.size(), 0);
    std::vector<u8> empty(targets.size(), 0);

    // How far the type ids in this clip reach, so the per-worker tally below can be a plain array
    // indexed by type. The caller usually knows already; when it does not, one pass over the clip
    // finds out, spread across the cores because at this size even reading an array is work.
    if (type_count == 0) {
        type_count = 1;
        const usize stripes = std::max<usize>(1, (jobs != nullptr) ? jobs->worker_count() + 1 : 1);
        std::vector<u32> highest(stripes, 0);
        const auto scan = [&](usize begin, usize end) {
            for (usize s = begin; s < end; ++s) {
                const usize from = clip.voxels.size() * s / stripes;
                const usize to = clip.voxels.size() * (s + 1) / stripes;
                u32 top = 0;
                for (usize i = from; i < to; ++i) top = std::max(top, clip.voxels[i]);
                highest[s] = top;
            }
        };
        if (jobs != nullptr && stripes > 1) {
            jobs->parallel_for(stripes, 1, scan);
        } else {
            scan(0, stripes);
        }
        for (u32 top : highest) type_count = std::max(type_count, static_cast<usize>(top) + 1);
    }

    const auto do_chunk = [&](usize begin, usize end) {
        VoxelTypeId want[kBrickVoxels];
        VoxelTypeId have[kBrickVoxels];
        // One counter per type, for the common case of matter arriving where there was none.
        // Allocated per batch of chunks rather than per chunk, because it is the size of the type
        // table and clearing it once per chunk would cost more than it saves.
        std::vector<u32> from_air(type_count, 0);
        for (usize t = begin; t < end; ++t) {
            Chunk& chunk = *targets[t].chunk;
            LedgerDelta& delta = deltas[t];
            // Counted locally and stored once at the end. Written straight into the shared vector
            // they would be two neighbouring words in one cache line, and two threads bouncing a
            // line between them on every brick costs more than the counting does.
            u64 local_changed = 0;
            u64 local_written = 0;
            u64 local_emptied = 0;
            const i64 base[3] = {targets[t].coord.x << 8, targets[t].coord.y << 8,
                                 targets[t].coord.z << 8};
            const i64 clo[3] = {std::max(lo[0], base[0]), std::max(lo[1], base[1]),
                                std::max(lo[2], base[2])};
            const i64 chi[3] = {std::min(hi[0], base[0] + 255), std::min(hi[1], base[1] + 255),
                                std::min(hi[2], base[2] + 255)};
            bool touched = false;

            for (i64 bz = (clo[2] - base[2]) >> 3; bz <= (chi[2] - base[2]) >> 3; ++bz) {
                for (i64 by = (clo[1] - base[1]) >> 3; by <= (chi[1] - base[1]) >> 3; ++by) {
                    for (i64 bx = (clo[0] - base[0]) >> 3; bx <= (chi[0] - base[0]) >> 3; ++bx) {
                        const i64 origin[3] = {base[0] + (bx << 3), base[1] + (by << 3),
                                               base[2] + (bz << 3)};

                        // R12d — A BRICK SOMEBODY EDITED IS NOT THE FIELD'S TO REWRITE.
                        //
                        // This paste is a REPLACE over its whole box, so where it lands on a
                        // doorway a player cut, the doorway fills back in. Today that survives
                        // only because `pump_refinement` replays the ENTIRE op log immediately
                        // afterwards and re-cuts every edit — which works while the log is in
                        // memory and stops working the moment a world is reloaded, because a
                        // resumed run has the carvings and not the ops that made them.
                        //
                        // `any_edits()` is checked first and is nearly always false: it is one
                        // comparison for a chunk of 32,768 brick slots, so an unedited world pays
                        // nothing for this at all.
                        if (kEditTracking && chunk.any_edits() &&
                            chunk.brick_edited(static_cast<u32>(bx), static_cast<u32>(by),
                                               static_cast<u32>(bz))) {
                            continue;
                        }

                        // Gather what the clip wants in these 512 cells. A cell outside the
                        // clip, or one the mode says nothing about, keeps whatever is already
                        // there — recorded as "no opinion" by leaving the bit clear.
                        //
                        // Walked as eight-voxel rows, because a brick row and a clip row run
                        // along the same axis: the eight cells are contiguous in both, so the
                        // addressing is worked out once per row rather than once per voxel.
                        // At sixty million voxels the difference is not a micro-optimisation.
                        bool wants_any = false;
                        bool wants_all = true;
                        u64 opinion[8]{};
                        for (u32 lz = 0; lz < 8u; ++lz) {
                            const i64 wz = origin[2] + static_cast<i64>(lz);
                            const bool z_in = wz >= lo[2] && wz <= hi[2];
                            for (u32 ly = 0; ly < 8u; ++ly) {
                                const u32 v0 = brick_index(0, ly, lz);
                                const i64 wy = origin[1] + static_cast<i64>(ly);
                                if (!z_in || wy < lo[1] || wy > hi[1]) {
                                    for (u32 lx = 0; lx < 8u; ++lx) want[v0 + lx] = kAir;
                                    wants_all = false;
                                    continue;
                                }
                                // The row start for this clip line. With a scale the eight world
                                // voxels of a brick row no longer map one-to-one onto eight clip
                                // cells — at a scale of eight they are all the same cell — so the
                                // column is worked out per voxel below rather than added on.
                                const usize row = clip.index(0, static_cast<i32>((wy - oy) >> shift),
                                                             static_cast<i32>((wz - oz) >> shift));
                                for (u32 lx = 0; lx < 8u; ++lx) {
                                    const i64 wx = origin[0] + static_cast<i64>(lx);
                                    VoxelTypeId value = kAir;
                                    bool has_opinion = false;
                                    if (wx >= lo[0] && wx <= hi[0]) {
                                        const usize cell =
                                            row + static_cast<usize>((wx - ox) >> shift);
                                        if (clip.inside[cell] != 0) {
                                            value = clip.voxels[cell];
                                            has_opinion = clears || value != kAir;
                                        }
                                    }
                                    want[v0 + lx] = value;
                                    if (has_opinion) {
                                        opinion[(v0 + lx) >> 6] |= (u64{1} << ((v0 + lx) & 63u));
                                        wants_any = true;
                                    } else {
                                        wants_all = false;
                                    }
                                }
                            }
                        }
                        if (!wants_any) continue;

                        const Brick* existing = chunk.brick(static_cast<u32>(bx),
                                                            static_cast<u32>(by),
                                                            static_cast<u32>(bz));

                        // The common case, and worth its own path: the clip covers this brick
                        // completely, nothing is there yet, and no mask is filtering. Then the
                        // brick simply *is* what the clip says, with nothing to read back, merge
                        // or compare — and building an entire brick's worth of the world costs
                        // one pass over 512 values.
                        u64 hits = 0;
                        if (existing == nullptr && wants_all && mask == WriteMask::All) {
                            // Everything here comes from air, so the ledger only needs a count
                            // per type — a straight increment into a dense array, no key, no
                            // probe. Under the no-two-voxels-alike rule that array is the whole
                            // type table and the increments are scattered across it, so making
                            // each one a single indexed write rather than a hash lookup is worth
                            // it sixty million times over.
                            for (u32 v = 0; v < static_cast<u32>(kBrickVoxels); ++v) {
                                if (want[v] == kAir) continue;
                                ++from_air[want[v]];
                                ++hits;
                            }
                        } else {
                            if (existing == nullptr) {
                                if (mask == WriteMask::OntoSolid) continue;
                                for (u32 v = 0; v < static_cast<u32>(kBrickVoxels); ++v) {
                                    have[v] = kAir;
                                }
                            } else {
                                existing->decode(have);
                            }

                            // Merge. Where the clip has no opinion, or the mask refuses, what
                            // was there survives.
                            for (u32 v = 0; v < static_cast<u32>(kBrickVoxels); ++v) {
                                const bool mine = (opinion[v >> 6] & (u64{1} << (v & 63u))) != 0;
                                if (!mine || !mask_allows(mask, have[v])) {
                                    want[v] = have[v];
                                    continue;
                                }
                                if (want[v] == have[v]) continue;
                                delta.add(have[v], want[v], 1);
                                ++hits;
                            }
                        }
                        if (hits == 0) continue;

                        Brick& into = chunk.brick_for_write(static_cast<u32>(bx),
                                                            static_cast<u32>(by),
                                                            static_cast<u32>(bz));
                        into.assign(want);
                        local_changed += hits;
                        ++local_written;
                        touched = true;

                        // A Replace paste erases where the clip is empty, so this write can take
                        // the last voxel out of a brick — and `assign` is a bulk write the chunk
                        // never sees voxel by voxel, so nothing above unlinks it. Left allocated,
                        // it is `world_has` claiming matter that is not there, which is a cube the
                        // marcher draws and can never build (D620). Counted either way, because
                        // the count is what makes the control arm readable.
                        if (into.empty()) {
                            ++local_emptied;
                            if (drop_empty) {
                                chunk.drop_brick_if_empty(static_cast<u32>(bx),
                                                          static_cast<u32>(by),
                                                          static_cast<u32>(bz));
                            }
                        }
                    }
                }
            }
            changed[t] = local_changed;
            written[t] = local_written;
            emptied[t] = local_emptied;
            if (touched) chunk.mark_modified();
            // Whether the CHUNK is empty now, rather than whether this paste happened to write
            // into it. A paste that clears the last brick out of a chunk it did touch leaves one
            // exactly as surely as one that creates a chunk and writes nothing, and only the
            // second was ever noticed.
            empty[t] = chunk.empty() ? u8{1} : u8{0};
        }

        // Fold the batch's air-to-matter counts into the first chunk's delta. Safe without a
        // lock: only this thread ever touched these chunks, so only this thread touches that
        // delta.
        if (begin < end) {
            LedgerDelta& sink = deltas[begin];
            for (usize type = 1; type < from_air.size(); ++type) {
                if (from_air[type] != 0) {
                    sink.add(kAir, static_cast<VoxelTypeId>(type), from_air[type]);
                }
            }
        }
    };

    if (jobs != nullptr && targets.size() > 1) {
        // Batched so that each worker is called about once, because the per-batch scratch is the
        // size of the type table — a million counters — and allocating and clearing that for
        // every handful of chunks costs more than the load balancing it buys back.
        const usize workers = std::max<usize>(1, jobs->worker_count() + 1);
        jobs->parallel_for(targets.size(), (targets.size() + workers - 1) / workers, do_chunk);
    } else {
        do_chunk(0, targets.size());
    }

    for (usize t = 0; t < targets.size(); ++t) {
        stats.voxels_changed += changed[t];
        stats.bricks_written += written[t];
        stats.bricks_emptied += emptied[t];
        if (empty[t] != 0) {
            stats.chunks_left_empty = true;
            ++stats.chunks_emptied;
            // On this thread, after the workers are done: erasing from the world's chunk map is
            // not something a per-chunk worker may do. It has to happen HERE and not at the
            // ladder's fixed point, which is where `world_.compact()` was called from -- so for
            // the whole of a load, which is the entire window the lumps were reported in, the
            // world held chunks that claim eight metres of occupancy and hold nothing.
            if (drop_empty) world.drop_chunk_if_empty(targets[t].coord);
        }
        const LedgerDelta& delta = deltas[t];
        for (usize i = 0; i < delta.keys.size(); ++i) {
            if (delta.keys[i] == 0) continue;
            const VoxelTypeId from = static_cast<VoxelTypeId>(delta.keys[i] >> 32);
            const VoxelTypeId to = static_cast<VoxelTypeId>(delta.keys[i] & 0xFFFFFFFFull);
            ledger.record_bulk(from, to, delta.counts[i], reason, player);
        }
    }
    return stats;
}

}  // namespace ws
