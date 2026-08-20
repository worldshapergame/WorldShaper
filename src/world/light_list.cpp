#include "world/light_list.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <numeric>
#include <unordered_map>

#include "core/hash.hpp"
#include "core/log.hpp"
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

// Emissive voxels and the box they occupy. The box is carried rather than a centre of mass
// because what the shader needs from this is a sphere that covers the fitting, and a centre of
// mass says nothing about how far the fitting reaches.
//
// One type serves both passes: the fine scan adds voxels to it and the fitting pass adds whole
// cells to it, and both are addition, so a fitting is the sum of its cells with no second
// representation to keep in step.
struct Cluster {
    i64 min_x = 0, min_y = 0, min_z = 0;
    i64 max_x = 0, max_y = 0, max_z = 0;
    f32 red = 0.0f, green = 0.0f, blue = 0.0f;
    u32 voxels = 0;

    void add(i64 x, i64 y, i64 z, f32 r, f32 g, f32 b) {
        if (voxels == 0) {
            min_x = max_x = x;
            min_y = max_y = y;
            min_z = max_z = z;
        } else {
            min_x = std::min(min_x, x);
            max_x = std::max(max_x, x);
            min_y = std::min(min_y, y);
            max_y = std::max(max_y, y);
            min_z = std::min(min_z, z);
            max_z = std::max(max_z, z);
        }
        red += r;
        green += g;
        blue += b;
        ++voxels;
    }

    void absorb(const Cluster& other) {
        if (other.voxels == 0) return;
        if (voxels == 0) {
            *this = other;
            return;
        }
        min_x = std::min(min_x, other.min_x);
        max_x = std::max(max_x, other.max_x);
        min_y = std::min(min_y, other.min_y);
        max_y = std::max(max_y, other.max_y);
        min_z = std::min(min_z, other.min_z);
        max_z = std::max(max_z, other.max_z);
        red += other.red;
        green += other.green;
        blue += other.blue;
        voxels += other.voxels;
    }

    i64 span_x() const { return max_x - min_x + 1; }
    i64 span_y() const { return max_y - min_y + 1; }
    i64 span_z() const { return max_z - min_z + 1; }
};

// Radiance a voxel gives off, from its visual record. Matches material_of() in pt_material.glsl
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

// The figure to put in LightSource::voxels, which is the only way this has of telling the
// shader how big a fitting is: it reads back `radius = 0.87 * cbrt(voxels)`.
//
// For a solid cube the two agree to the digit — 0.87 * cbrt(s^3) is 0.87s, and the half
// diagonal of a cube of side s is 0.866s — so a lamp that really is a cube reports its own
// count and nothing changes. Merging is what makes them differ: join a sconce's dozen cells
// into one entry and the box is longer than it is wide, and the sphere for the raw count would
// leave the ends of the fitting outside the cone the shader draws. Light outside that cone is
// owned by nobody, so it goes out, and a sconce would be lit at its middle and dark at its tips.
u32 covering_voxels(const Cluster& cluster) {
    const f64 nx = static_cast<f64>(cluster.span_x());
    const f64 ny = static_cast<f64>(cluster.span_y());
    const f64 nz = static_cast<f64>(cluster.span_z());
    const f64 radius = 0.5 * std::sqrt(nx * nx + ny * ny + nz * nz);
    const f64 edge = radius / 0.87;
    const f64 needed = std::ceil(edge * edge * edge);
    // kLightFittingVoxels keeps this far under a u32 — a metre cube is about 32000 — but the
    // cast is only safe because of that, so it is stated rather than assumed.
    const f64 capped = std::min(needed, 1.0e9);
    return std::max(cluster.voxels, static_cast<u32>(capped));
}

// What a fitting would deliver at the camera with nothing in the way: its radiance times the
// solid angle its sphere covers. This is the shader's own per-surface estimate with the cosine
// term left out, there being no normal here to take it against — and the cosine is not what
// decides whether a lamp deserves a slot in the list anyway. Distance is floored at one voxel
// so a lamp the camera is standing inside does not divide by nothing.
f64 contribution(const LightSource& light, i64 centre_x, i64 centre_y, i64 centre_z) {
    const f64 dx = static_cast<f64>(static_cast<i64>(light.x) - centre_x);
    const f64 dy = static_cast<f64>(static_cast<i64>(light.y) - centre_y);
    const f64 dz = static_cast<f64>(static_cast<i64>(light.z) - centre_z);
    const f64 distance_sq = std::max(dx * dx + dy * dy + dz * dz, 1.0);
    const f64 radius = 0.87 * std::cbrt(static_cast<f64>(std::max(light.voxels, 1u)));
    // The same weights shaders/shade_faces.comp uses to rank a lamp, so the ranking here agrees with
    // the importance the shader will put on the same lamp.
    const f64 luminance = 0.2126 * static_cast<f64>(light.red) +
                          0.7152 * static_cast<f64>(light.green) +
                          0.0722 * static_cast<f64>(light.blue);
    return luminance * radius * radius / distance_sq;
}

// What a fitting gives off, with the observer taken out of it: `contribution` without the distance
// term. A fact about the lamp, which is the whole point — it is what the cap is spent by when the
// cap is spent on the world rather than on the camera.
f64 emitted_power(const LightSource& light) {
    const f64 radius = 0.87 * std::cbrt(static_cast<f64>(std::max(light.voxels, 1u)));
    const f64 luminance = 0.2126 * static_cast<f64>(light.red) +
                          0.7152 * static_cast<f64>(light.green) +
                          0.0722 * static_cast<f64>(light.blue);
    return luminance * radius * radius;
}

// A total order over two fittings that never consults the camera: brightest first, and the bytes
// when two are equally bright. It has to be total — `std::sort` is not stable, so a comparator that
// leaves two records equal lets the input order decide, and the input order here came from a map.
bool brighter(const LightSource& a, const LightSource& b) {
    const f64 pa = emitted_power(a);
    const f64 pb = emitted_power(b);
    if (pa != pb) return pa > pb;
    return std::memcmp(&a, &b, sizeof(LightSource)) < 0;
}

// The cap, dealt round the world instead of measured from the camera. See kLightCapByWorld.
//
// Buckets of kLightCapCellVoxels, ordered by key; inside a bucket, brightest first; then round
// after round, one from each bucket, until the cap is full. Nothing in it reads `centre`, so the
// answer is a fact about the world and moving does not change it.
//
// `ranked` arrives in the camera's order and the survivors come back in it, so only membership
// changes here. Kept as a filter rather than a rebuild for exactly that reason.
std::vector<LightSource> deal_the_cap(const std::vector<LightSource>& ranked) {
    std::unordered_map<ClusterKey, std::vector<u32>, ClusterKeyHash> in_cell;
    std::vector<ClusterKey> cells;
    for (u32 i = 0; i < static_cast<u32>(ranked.size()); ++i) {
        const LightSource& light = ranked[i];
        const ClusterKey key{floor_div(light.x, kLightCapCellVoxels),
                             floor_div(light.y, kLightCapCellVoxels),
                             floor_div(light.z, kLightCapCellVoxels)};
        const auto [at, fresh] = in_cell.try_emplace(key, std::vector<u32>{});
        if (fresh) cells.push_back(key);
        at->second.push_back(i);
    }

    std::sort(cells.begin(), cells.end(), [](const ClusterKey& a, const ClusterKey& b) {
        if (a.x != b.x) return a.x < b.x;
        if (a.y != b.y) return a.y < b.y;
        return a.z < b.z;
    });
    for (const ClusterKey& key : cells) {
        std::vector<u32>& mine = in_cell.find(key)->second;
        std::sort(mine.begin(), mine.end(),
                  [&](u32 a, u32 b) { return brighter(ranked[a], ranked[b]); });
    }

    std::vector<bool> kept(ranked.size(), false);
    usize admitted = 0;
    for (usize round = 0; admitted < kMaxLights; ++round) {
        bool any = false;
        for (const ClusterKey& key : cells) {
            const std::vector<u32>& mine = in_cell.find(key)->second;
            if (round >= mine.size()) continue;
            any = true;
            kept[mine[round]] = true;
            if (++admitted == kMaxLights) break;
        }
        // Cannot happen while there are more fittings than the cap, which is the only way in here.
        // Stated rather than assumed, because a loop that depends on a caller's condition is a hang
        // waiting for somebody to change the caller.
        if (!any) break;
    }

    std::vector<LightSource> out;
    out.reserve(admitted);
    for (usize i = 0; i < ranked.size(); ++i) {
        if (kept[i]) out.push_back(ranked[i]);
    }
    return out;
}

// Say it once, not once a frame.
//
// The list is rebuilt on every edit, so a scene that has outgrown the cap would otherwise write
// this line every time anything is placed and bury the rest of the log. Remembering the last
// number reported means a scene that grows further still says so, and one that comes back under
// the cap and overflows again later says so again.
void note_overflow(usize dropped) {
    static std::atomic<usize> reported{0};
    if (reported.exchange(dropped) == dropped || dropped == 0) return;
    WS_LOG_WARN("light",
                "{} fittings past the cap of {} were dropped; the list is truncated, so the "
                "tracer will not aim at any of them and every lamp goes back to the bounce",
                dropped, kMaxLights);
}

}  // namespace

std::vector<EmissiveCell> scan_chunk_emitters(const Chunk& chunk, i64 base_x, i64 base_y,
                                              i64 base_z, const VoxelTypeTable& types) {
    // The fine grid. It is not the answer — a sconce is bigger than one cell and would come out
    // as a dozen lights — but it is what makes the scan cheap, and it reduces a solid emissive
    // wall to something the fitting pass can afford to walk.
    std::unordered_map<ClusterKey, u32, ClusterKeyHash> cell_of;
    std::vector<ClusterKey> cell_keys;
    std::vector<Cluster> cells;

    if (!chunk.empty()) {
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
                                const auto [at, fresh] =
                                    cell_of.try_emplace(key, static_cast<u32>(cells.size()));
                                if (fresh) {
                                    cell_keys.push_back(key);
                                    cells.emplace_back();
                                }
                                f32 r = 0.0f, g = 0.0f, b = 0.0f;
                                emitted_radiance(visual, r, g, b);
                                cells[at->second].add(x, y, z, r, g, b);
                            }
                        }
                    }
                }
            }
        }
    }

    // Out in the form the host can keep: the key beside the box and the sums, so a cached chunk
    // needs nothing else to be merged with any other chunk's.
    std::vector<EmissiveCell> out;
    out.reserve(cells.size());
    for (usize i = 0; i < cells.size(); ++i) {
        const Cluster& c = cells[i];
        out.push_back(EmissiveCell{cell_keys[i].x, cell_keys[i].y, cell_keys[i].z, c.min_x,
                                   c.min_y, c.min_z, c.max_x, c.max_y, c.max_z, c.red, c.green,
                                   c.blue, c.voxels});
    }
    return out;
}

std::vector<LightSource> merge_light_list(const std::vector<EmissiveCell>& source, i64 centre_x,
                                          i64 centre_y, i64 centre_z) {
    // Back into the two parallel arrays the merge below has always walked. A cluster cell is four
    // voxels and a chunk is 256, so cells from two different chunks can never share a key and this
    // is a copy rather than a merge -- the property the caching rests on, stated where it is used.
    std::unordered_map<ClusterKey, u32, ClusterKeyHash> cell_of;
    std::vector<ClusterKey> cell_keys;
    std::vector<Cluster> cells;
    cell_keys.reserve(source.size());
    cells.reserve(source.size());
    for (const EmissiveCell& in : source) {
        const ClusterKey key{in.key_x, in.key_y, in.key_z};
        const auto [at, fresh] = cell_of.try_emplace(key, static_cast<u32>(cells.size()));
        if (fresh) {
            cell_keys.push_back(key);
            cells.push_back(Cluster{in.min_x, in.min_y, in.min_z, in.max_x, in.max_y, in.max_z,
                                    in.red, in.green, in.blue, in.voxels});
            continue;
        }
        // Cannot happen while the divisibility above holds, and it is folded rather than asserted
        // because the arithmetic that makes it impossible is in a different file from this one.
        Cluster& have = cells[at->second];
        have.min_x = std::min(have.min_x, in.min_x);
        have.min_y = std::min(have.min_y, in.min_y);
        have.min_z = std::min(have.min_z, in.min_z);
        have.max_x = std::max(have.max_x, in.max_x);
        have.max_y = std::max(have.max_y, in.max_y);
        have.max_z = std::max(have.max_z, in.max_z);
        have.red += in.red;
        have.green += in.green;
        have.blue += in.blue;
        have.voxels += in.voxels;
    }

    // Cells joined into fittings, and this is the part that decides whether a building fits
    // under the cap at all. A wall sconce is a few hundred voxels across a dozen cells; a hall
    // of forty of them is five hundred entries by cell and forty by fitting. Ranking and
    // dropping can only choose among what is left after this, and there is nothing to choose
    // between if this has already brought the count down to what a room actually holds.
    //
    // Face adjacency, not corner: it means a fitting whose voxels touch face to face comes out
    // as one light, which is a rule that can be stated, and it will not chain two fittings
    // together because they happen to pass near each other on a diagonal.
    std::vector<u32> order(cells.size());
    std::iota(order.begin(), order.end(), 0u);
    // Deterministic, because the map's iteration order is not something to build a render on
    // and because the growth below depends on which cell is reached first.
    std::sort(order.begin(), order.end(), [&](u32 a, u32 b) {
        const ClusterKey& ka = cell_keys[a];
        const ClusterKey& kb = cell_keys[b];
        if (ka.x != kb.x) return ka.x < kb.x;
        if (ka.y != kb.y) return ka.y < kb.y;
        return ka.z < kb.z;
    });

    static constexpr i64 kFaces[6][3] = {{-1, 0, 0}, {1, 0, 0},  {0, -1, 0},
                                         {0, 1, 0},  {0, 0, -1}, {0, 0, 1}};

    std::vector<bool> taken(cells.size(), false);
    std::vector<u32> frontier;
    std::vector<LightSource> lights;

    for (const u32 seed : order) {
        if (taken[seed]) continue;
        taken[seed] = true;
        Cluster fitting = cells[seed];
        frontier.clear();
        frontier.push_back(seed);

        for (usize head = 0; head < frontier.size(); ++head) {
            const ClusterKey at = cell_keys[frontier[head]];
            for (const auto& step : kFaces) {
                const auto found =
                    cell_of.find(ClusterKey{at.x + step[0], at.y + step[1], at.z + step[2]});
                if (found == cell_of.end()) continue;
                const u32 next = found->second;
                if (taken[next]) continue;

                Cluster grown = fitting;
                grown.absorb(cells[next]);
                // Two refusals, and they guard different things. The size limit keeps the
                // sphere small enough that the shader can still own what is inside it (see
                // kLightFittingVoxels); the slack keeps the sphere from being mostly empty (see
                // kLightMergeSlack). A merge that fails either leaves the neighbour to start a
                // fitting of its own — it is not lost, only kept separate.
                if (grown.span_x() > kLightFittingVoxels ||
                    grown.span_y() > kLightFittingVoxels ||
                    grown.span_z() > kLightFittingVoxels)
                    continue;
                if (static_cast<u64>(covering_voxels(grown)) >
                    static_cast<u64>(kLightMergeSlack) * grown.voxels)
                    continue;

                taken[next] = true;
                fitting = grown;
                frontier.push_back(next);
            }
        }

        LightSource light;
        // The middle of the box, not the centre of mass: coverage is measured from here, and a
        // lopsided fitting would pull a mass centre towards its bright end and leave the other
        // end reaching further than the sphere does.
        light.x = static_cast<i32>(floor_div(fitting.min_x + fitting.max_x, 2));
        light.y = static_cast<i32>(floor_div(fitting.min_y + fitting.max_y, 2));
        light.z = static_cast<i32>(floor_div(fitting.min_z + fitting.max_z, 2));
        // Mean radiance over the voxels that are really there, never over the inflated figure
        // below — the sphere may be drawn larger than the fitting, but the fitting is not
        // brighter or dimmer for it.
        const f32 count = static_cast<f32>(fitting.voxels);
        light.red = fitting.red / count;
        light.green = fitting.green / count;
        light.blue = fitting.blue / count;
        light.voxels = covering_voxels(fitting);
        lights.push_back(light);
    }

    // Strongest first, not nearest first. Distance alone spends the cap on whatever happens to
    // be close, and a dim indicator two metres away is worth less than the chandelier across
    // the room; radiance over distance squared is what the shader will decide with when it
    // weighs the same lamp, so it is what this decides with too.
    struct Ranked {
        f64 score;
        LightSource light;
    };
    std::vector<Ranked> ranked;
    ranked.reserve(lights.size());
    for (const LightSource& light : lights) {
        ranked.push_back(Ranked{contribution(light, centre_x, centre_y, centre_z), light});
    }
    std::sort(ranked.begin(), ranked.end(), [](const Ranked& a, const Ranked& b) {
        if (a.score != b.score) return a.score > b.score;
        // A stable tie-break, so the same world always produces the same list and a
        // measurement taken twice is the same measurement.
        if (a.light.x != b.light.x) return a.light.x < b.light.x;
        if (a.light.y != b.light.y) return a.light.y < b.light.y;
        return a.light.z < b.light.z;
    });

    lights.clear();
    for (const Ranked& entry : ranked) lights.push_back(entry.light);

    const usize dropped = (lights.size() > kMaxLights) ? lights.size() - kMaxLights : 0;
    // Down to exactly the cap, which is all the buffer the card has. Whatever is dropped is lit by
    // nobody — direct sampling owns emitters outright, so a fitting past the end goes out until a
    // bounce happens to land on it — and the warning above is how anyone finds out it happened.
    //
    // WHICH ones go is R9g. Cutting the camera's ranking makes the surviving set a fact about where
    // the player is standing, so walking turns lamps off; dealing the cap round the world makes it a
    // fact about the world, so only an edit can. Both arms end with exactly kMaxLights entries and
    // differ in nothing else.
    if (dropped > 0) {
        lights = kLightCapByWorld ? deal_the_cap(lights)
                                  : std::vector<LightSource>(lights.begin(),
                                                             lights.begin() + kMaxLights);
    }
    note_overflow(dropped);
    return lights;
}

std::vector<LightSource> build_light_list(const World& world, const VoxelTypeTable& types,
                                          i64 centre_x, i64 centre_y, i64 centre_z) {
    // The whole world, scanned from scratch. Kept as what it always was, because it is the
    // reference the incremental path in the application is checked against -- the two must produce
    // an identical list, and `light_list_hash` is what says so.
    std::vector<EmissiveCell> cells;
    world.for_each_chunk([&](const ChunkCoord& coord, const Chunk& chunk) {
        std::vector<EmissiveCell> mine =
            scan_chunk_emitters(chunk, coord.x * static_cast<i64>(kChunkEdge),
                                coord.y * static_cast<i64>(kChunkEdge),
                                coord.z * static_cast<i64>(kChunkEdge), types);
        cells.insert(cells.end(), mine.begin(), mine.end());
    });
    return merge_light_list(cells, centre_x, centre_y, centre_z);
}

u64 light_list_hash(const std::vector<LightSource>& lights) {
    // Seeded with the length so that an empty list and a list of one lamp at the origin with no
    // radiance cannot collide, and so that a truncated list is distinct from the same lamps
    // untruncated.
    u64 hash = hash_mix(static_cast<u64>(lights.size()) + 0x9E3779B97F4A7C15ull);
    if (lights.empty()) return hash;

    // Over a CANONICAL order, because the list's own order is a fact about where the camera is
    // standing and not about the lamps.
    //
    // `build_light_list` ranks by what each fitting would deliver at the camera, so walking two
    // paces re-orders it with nothing having changed. This hash is the gate on `light_reset`, and
    // `light_reset` reopens the lamp term of EVERY face in the store -- so hashing the rank order
    // meant that any world change made while the player had moved since the last one relit the
    // whole room. Measured on the facility, warm cache, one edit every sixty frames: nine strokes
    // from a static camera bumped the version once and left `lamps on the card: 469,861 of 507,251
    // live faces cast no more rays at all`; the same nine while flying bumped it NINE times and
    // left `0 of 997,296`. Nothing converges, so every face is re-measuring every frame, which is
    // what a player sees as the light turning into per-face squares that flicker. It is D433's
    // symptom exactly, arriving through the one door D434 did not close. D500.
    //
    // What ordering was said to buy -- catching a change that happens to be a permutation -- is not
    // lost, because a permutation of the same records IS the same set of lamps. Rank decides only
    // which of them survive the `kMaxLights` cap, and a cap that drops a different fitting changes
    // the set, so it still changes this hash.
    //
    // Ordered by the bytes rather than field by field, which is a total order over a 28-byte POD
    // with no padding (the static_assert beside the record is what makes that safe) and is the same
    // bytes the hash then runs over. A comparison on a subset of the fields would leave two records
    // equal-comparing, and `std::sort` is not stable, so the hash would depend on the input order
    // again -- through a narrower door.
    std::vector<LightSource> canonical = lights;
    std::sort(canonical.begin(), canonical.end(), [](const LightSource& a, const LightSource& b) {
        return std::memcmp(&a, &b, sizeof(LightSource)) < 0;
    });
    return hash_bytes(reinterpret_cast<const u8*>(canonical.data()),
                      canonical.size() * sizeof(LightSource), hash);
}

}  // namespace ws
