#include "world/raycast.hpp"

#include <cmath>

#include "world/world.hpp"

namespace ws {
namespace {

constexpr f64 kInfinity = 1e300;

// How far past a box boundary to land when skipping. Doubles hold ~1e-13 relative
// precision, so at the thousands-of-voxels range this tool works at, 1e-6 is far above the
// noise and far below a voxel.
constexpr f64 kSkipEpsilon = 1e-6;

inline i64 floor_i64(f64 v) { return static_cast<i64>(std::floor(v)); }

// A voxel DDA whose parameters are all relative to the *original* ray origin, so skipping
// forward and re-seeding introduces no drift: t is always the true distance along the ray.
struct Walker {
    f64 o[3]{};
    f64 d[3]{};
    f64 inv[3]{};
    f64 delta[3]{};
    i32 step[3]{};
    i64 v[3]{};
    f64 tmax[3]{};

    void refresh() {
        for (int a = 0; a < 3; ++a) {
            if (step[a] > 0) {
                tmax[a] = (static_cast<f64>(v[a] + 1) - o[a]) * inv[a];
            } else if (step[a] < 0) {
                tmax[a] = (static_cast<f64>(v[a]) - o[a]) * inv[a];
            } else {
                tmax[a] = kInfinity;
            }
        }
    }

    void seed(f64 t) {
        for (int a = 0; a < 3; ++a) v[a] = floor_i64(o[a] + d[a] * t);
        refresh();
    }
};

// Distance at which the ray leaves an axis-aligned box it is currently inside, and which
// face it leaves through.
f64 box_exit(const Walker& w, const i64 min_corner[3], i64 size, int& axis) {
    f64 best = kInfinity;
    axis = -1;
    for (int a = 0; a < 3; ++a) {
        if (w.step[a] == 0) continue;
        const f64 plane = static_cast<f64>(w.step[a] > 0 ? min_corner[a] + size : min_corner[a]);
        const f64 t = (plane - w.o[a]) * w.inv[a];
        if (t < best) {
            best = t;
            axis = a;
        }
    }
    return best;
}

}  // namespace

RayHit raycast(const World& world, f64 ox, f64 oy, f64 oz, f64 dx, f64 dy, f64 dz,
               f64 max_distance) {
    RayHit hit;

    const f64 length = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (!(length > 0.0) || !(max_distance > 0.0)) return hit;

    Walker w;
    w.o[0] = ox;
    w.o[1] = oy;
    w.o[2] = oz;
    w.d[0] = dx / length;
    w.d[1] = dy / length;
    w.d[2] = dz / length;
    for (int a = 0; a < 3; ++a) {
        if (w.d[a] > 0.0) {
            w.step[a] = 1;
        } else if (w.d[a] < 0.0) {
            w.step[a] = -1;
        } else {
            w.step[a] = 0;
        }
        w.inv[a] = (w.step[a] != 0) ? 1.0 / w.d[a] : 0.0;
        w.delta[a] = (w.step[a] != 0) ? std::abs(w.inv[a]) : kInfinity;
    }
    w.seed(0.0);

    f64 t = 0.0;
    int axis = -1;   // which face the ray entered the current voxel through

    // Caches. A lookup happens only when the ray crosses out of the cached chunk or brick,
    // which for a long ray through open space is a handful of times, not thousands.
    ChunkCoord chunk_coord{};
    const Chunk* chunk = nullptr;
    bool chunk_valid = false;

    i64 brick_min[3]{};
    const Brick* brick = nullptr;
    u64 occupancy[kBrickWords]{};
    bool brick_valid = false;

    // Each iteration either tests one voxel or skips an empty box, so the bound is loose;
    // it exists only so a degenerate ray cannot hang the frame.
    const i64 max_iterations = static_cast<i64>(max_distance) * 4 + 64;

    auto skip_box = [&](const i64 min_corner[3], i64 size) {
        int exit_axis = -1;
        const f64 exit_t = box_exit(w, min_corner, size, exit_axis);
        if (exit_axis < 0) {
            t = max_distance + 1.0;   // ray is parallel to every axis: impossible, bail out
            return;
        }
        f64 next = exit_t + kSkipEpsilon;
        if (next <= t) next = t + kSkipEpsilon;
        t = next;
        axis = exit_axis;
        w.seed(t);
    };

    for (i64 i = 0; i < max_iterations && t <= max_distance; ++i) {
        const ChunkCoord cc = chunk_coord_of(w.v[0], w.v[1], w.v[2]);
        if (!chunk_valid || !(cc == chunk_coord)) {
            chunk_coord = cc;
            chunk = world.chunk(cc);
            chunk_valid = true;
            brick_valid = false;
        }
        if (chunk == nullptr) {
            const i64 corner[3] = {cc.x << 8, cc.y << 8, cc.z << 8};
            skip_box(corner, 256);
            continue;
        }

        const i64 bmin[3] = {w.v[0] & ~i64{7}, w.v[1] & ~i64{7}, w.v[2] & ~i64{7}};
        if (!brick_valid || bmin[0] != brick_min[0] || bmin[1] != brick_min[1] ||
            bmin[2] != brick_min[2]) {
            brick_min[0] = bmin[0];
            brick_min[1] = bmin[1];
            brick_min[2] = bmin[2];
            brick = chunk->brick(local_of(w.v[0]) >> 3, local_of(w.v[1]) >> 3,
                                 local_of(w.v[2]) >> 3);
            brick_valid = true;
            if (brick != nullptr) brick->occupancy(occupancy);
        }
        if (brick == nullptr) {
            skip_box(bmin, 8);
            continue;
        }

        const u32 index = brick_index(static_cast<u32>(w.v[0] & 7), static_cast<u32>(w.v[1] & 7),
                                      static_cast<u32>(w.v[2] & 7));
        if ((occupancy[index >> 6] >> (index & 63)) & 1ull) {
            hit.hit = true;
            hit.x = w.v[0];
            hit.y = w.v[1];
            hit.z = w.v[2];
            hit.distance = t;
            hit.type = brick->get(index);
            if (axis >= 0) {
                hit.nx = (axis == 0) ? -w.step[0] : 0;
                hit.ny = (axis == 1) ? -w.step[1] : 0;
                hit.nz = (axis == 2) ? -w.step[2] : 0;
            } else {
                // The ray started inside this voxel, so there is no entry face. Face the
                // camera on the dominant axis, which keeps placement sane rather than
                // returning a normal of zero and placing inside solid rock.
                int dominant = 0;
                for (int a = 1; a < 3; ++a) {
                    if (std::abs(w.d[a]) > std::abs(w.d[dominant])) dominant = a;
                }
                hit.nx = (dominant == 0) ? -w.step[0] : 0;
                hit.ny = (dominant == 1) ? -w.step[1] : 0;
                hit.nz = (dominant == 2) ? -w.step[2] : 0;
            }
            return hit;
        }

        int next_axis = 0;
        if (w.tmax[1] < w.tmax[next_axis]) next_axis = 1;
        if (w.tmax[2] < w.tmax[next_axis]) next_axis = 2;
        if (w.tmax[next_axis] >= kInfinity) break;   // ray is axis-locked and left the world

        t = w.tmax[next_axis];
        axis = next_axis;
        w.v[next_axis] += w.step[next_axis];
        w.tmax[next_axis] += w.delta[next_axis];
    }

    return hit;
}

}  // namespace ws
