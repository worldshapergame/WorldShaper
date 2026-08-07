#include "world/light_list.hpp"

#include <algorithm>
#include <unordered_map>

#include "core/hash.hpp"
#include "world/chunk.hpp"
#include "world/world.hpp"

namespace ws {

namespace {

i64 floor_div(i64 value, i64 divisor) {
    return (value >= 0) ? (value / divisor) : -(((-value) + divisor - 1) / divisor);
}

struct ClusterKey {
    i64 x, y, z;
    bool operator==(const ClusterKey& other) const = default;
};

struct ClusterKeyHash {
    usize operator()(const ClusterKey& key) const {
        return static_cast<usize>(hash_cell(key.x, key.y, key.z, 0, 7));
    }
};

// Radiance a voxel gives off, from its visual record. Matches material_of() in pathtrace.comp
// exactly: the same squared scale and the same RGB565 tint. Two places computing this
// differently would light the scene one way and aim the rays another.
void emitted_radiance(const VisualRecord& visual, f32& red, f32& green, f32& blue) {
    const f32 scale = static_cast<f32>(visual.emissive) / 255.0f;
    const f32 strength = scale * scale * 64.0f;
    const u16 tint = visual.emissive_tint;
    red = static_cast<f32>((tint >> 11) & 0x1F) / 31.0f * strength;
    green = static_cast<f32>((tint >> 5) & 0x3F) / 63.0f * strength;
    blue = static_cast<f32>(tint & 0x1F) / 31.0f * strength;
}

}  // namespace

std::vector<LightSource> build_light_list(const World& world, const VoxelTypeTable& types,
                                          i64 centre_x, i64 centre_y, i64 centre_z) {
    // Accumulated per cluster, then averaged. A fitting made of eight voxels is one light with
    // the colour they share, not eight lights fighting for the same shadow ray.
    struct Accumulated {
        i64 sum_x = 0, sum_y = 0, sum_z = 0;
        f32 red = 0.0f, green = 0.0f, blue = 0.0f;
        u32 voxels = 0;
    };
    std::unordered_map<ClusterKey, Accumulated, ClusterKeyHash> clusters;

    world.for_each_chunk([&](const ChunkCoord& coord, const Chunk& chunk) {
        if (chunk.empty()) return;
        const i64 base_x = coord.x * static_cast<i64>(kChunkEdge);
        const i64 base_y = coord.y * static_cast<i64>(kChunkEdge);
        const i64 base_z = coord.z * static_cast<i64>(kChunkEdge);

        for (u32 bz = 0; bz < kChunkBricks; ++bz) {
            for (u32 by = 0; by < kChunkBricks; ++by) {
                for (u32 bx = 0; bx < kChunkBricks; ++bx) {
                    const Brick* brick = chunk.brick(bx, by, bz);
                    if (brick == nullptr) continue;

                    // The palette is what makes this affordable. A brick lists the types it
                    // contains, so a brick with no emitter in it is rejected in a handful of
                    // comparisons rather than by reading five hundred voxels. Nearly every
                    // brick in a world is rejected here.
                    bool any_emissive = false;
                    for (const VoxelTypeId type : brick->palette_data()) {
                        if (type != kAir && types.visual_of(type).emissive != 0) {
                            any_emissive = true;
                            break;
                        }
                    }
                    if (!any_emissive) continue;

                    for (u32 vz = 0; vz < kBrickEdge; ++vz) {
                        for (u32 vy = 0; vy < kBrickEdge; ++vy) {
                            for (u32 vx = 0; vx < kBrickEdge; ++vx) {
                                const u32 lx = bx * kBrickEdge + vx;
                                const u32 ly = by * kBrickEdge + vy;
                                const u32 lz = bz * kBrickEdge + vz;
                                const VoxelTypeId type = chunk.get(lx, ly, lz);
                                if (type == kAir) continue;
                                const VisualRecord& visual = types.visual_of(type);
                                if (visual.emissive == 0) continue;

                                const i64 x = base_x + lx;
                                const i64 y = base_y + ly;
                                const i64 z = base_z + lz;
                                const ClusterKey key{floor_div(x, kLightClusterVoxels),
                                                     floor_div(y, kLightClusterVoxels),
                                                     floor_div(z, kLightClusterVoxels)};
                                Accumulated& into = clusters[key];
                                f32 r = 0.0f, g = 0.0f, b = 0.0f;
                                emitted_radiance(visual, r, g, b);
                                into.sum_x += x;
                                into.sum_y += y;
                                into.sum_z += z;
                                into.red += r;
                                into.green += g;
                                into.blue += b;
                                ++into.voxels;
                            }
                        }
                    }
                }
            }
        }
    });

    std::vector<LightSource> lights;
    lights.reserve(clusters.size());
    for (const auto& [key, acc] : clusters) {
        if (acc.voxels == 0) continue;
        LightSource light;
        light.x = static_cast<i32>(acc.sum_x / static_cast<i64>(acc.voxels));
        light.y = static_cast<i32>(acc.sum_y / static_cast<i64>(acc.voxels));
        light.z = static_cast<i32>(acc.sum_z / static_cast<i64>(acc.voxels));
        light.red = acc.red / static_cast<f32>(acc.voxels);
        light.green = acc.green / static_cast<f32>(acc.voxels);
        light.blue = acc.blue / static_cast<f32>(acc.voxels);
        light.voxels = acc.voxels;
        lights.push_back(light);
    }

    // Nearest first, because the list is capped and a lamp on the far side of the world lights
    // nothing that is on screen.
    std::sort(lights.begin(), lights.end(), [&](const LightSource& a, const LightSource& b) {
        const i64 ax = a.x - centre_x, ay = a.y - centre_y, az = a.z - centre_z;
        const i64 bx = b.x - centre_x, by = b.y - centre_y, bz = b.z - centre_z;
        const i64 da = ax * ax + ay * ay + az * az;
        const i64 db = bx * bx + by * by + bz * bz;
        if (da != db) return da < db;
        // A stable tie-break, so the same world always produces the same list and a
        // measurement taken twice is the same measurement.
        if (a.x != b.x) return a.x < b.x;
        if (a.y != b.y) return a.y < b.y;
        return a.z < b.z;
    });
    if (lights.size() > kMaxLights) lights.resize(kMaxLights);
    return lights;
}

}  // namespace ws
