#include "forge/measure.hpp"

#include <algorithm>
#include <cstdio>
#include <map>

namespace ws {
namespace forge {

namespace {

bool solid_at(const Clip& clip, i32 x, i32 y, i32 z) {
    if (x < 0 || y < 0 || z < 0 || x >= clip.size[0] || y >= clip.size[1] || z >= clip.size[2]) {
        return false;
    }
    return clip.at(x, y, z) != kAir;
}

// The cell index along the two axes that are not `axis`, in ascending order — the same
// convention the field uses, so a slice and a prism agree about which way is across.
void other_axes(u32 axis, u32& a, u32& b) {
    if (axis == 0) { a = 1; b = 2; }
    else if (axis == 1) { a = 0; b = 2; }
    else { a = 0; b = 1; }
}

}  // namespace

Measurement measure(const Clip& clip, i32 voxels_per_metre) {
    Measurement m;
    m.voxels_per_metre = (voxels_per_metre > 0) ? voxels_per_metre : kVoxelsPerMetre;
    m.size[0] = clip.size[0];
    m.size[1] = clip.size[1];
    m.size[2] = clip.size[2];
    if (clip.empty()) return m;

    std::map<VoxelTypeId, u64> tally;
    f64 sum[3] = {0, 0, 0};

    for (i32 z = 0; z < clip.size[2]; ++z) {
        for (i32 y = 0; y < clip.size[1]; ++y) {
            for (i32 x = 0; x < clip.size[0]; ++x) {
                if (clip.covered(x, y, z)) ++m.covered;
                const VoxelTypeId type = clip.at(x, y, z);
                if (type == kAir) continue;

                ++m.solid;
                ++tally[type];
                sum[0] += x;
                sum[1] += y;
                sum[2] += z;

                if (!m.extent.any) {
                    m.extent.any = true;
                    m.extent.low[0] = m.extent.high[0] = x;
                    m.extent.low[1] = m.extent.high[1] = y;
                    m.extent.low[2] = m.extent.high[2] = z;
                } else {
                    m.extent.low[0] = std::min(m.extent.low[0], x);
                    m.extent.low[1] = std::min(m.extent.low[1], y);
                    m.extent.low[2] = std::min(m.extent.low[2], z);
                    m.extent.high[0] = std::max(m.extent.high[0], x);
                    m.extent.high[1] = std::max(m.extent.high[1], y);
                    m.extent.high[2] = std::max(m.extent.high[2], z);
                }

                // Six neighbours; a face is exposed when the neighbour is not solid. Cells at
                // the edge of the clip count their outward faces as exposed, because that is
                // what they are once the clip is stamped into open air.
                if (!solid_at(clip, x - 1, y, z)) ++m.exposed_faces;
                if (!solid_at(clip, x + 1, y, z)) ++m.exposed_faces;
                if (!solid_at(clip, x, y - 1, z)) ++m.exposed_faces;
                if (!solid_at(clip, x, y + 1, z)) ++m.exposed_faces;
                if (!solid_at(clip, x, y, z - 1)) ++m.exposed_faces;
                if (!solid_at(clip, x, y, z + 1)) ++m.exposed_faces;
            }
        }
    }

    if (m.solid > 0) {
        const f64 n = static_cast<f64>(m.solid);
        m.centroid[0] = sum[0] / n;
        m.centroid[1] = sum[1] / n;
        m.centroid[2] = sum[2] / n;
    }

    m.types.reserve(tally.size());
    for (const auto& entry : tally) {
        TypeShare share;
        share.type = entry.first;
        share.count = entry.second;
        share.fraction = (m.solid > 0) ? static_cast<f64>(entry.second) / static_cast<f64>(m.solid)
                                       : 0.0;
        m.types.push_back(share);
    }
    std::sort(m.types.begin(), m.types.end(),
              [](const TypeShare& a, const TypeShare& b) { return a.count > b.count; });
    return m;
}

namespace {

// Walks a line and reports the run of cells matching `want_solid`.
Span run_along(const Clip& clip, u32 axis, i32 a, i32 b, bool want_solid) {
    Span span;
    if (clip.empty()) return span;
    u32 axis_a = 0, axis_b = 0;
    other_axes(axis, axis_a, axis_b);
    if (a < 0 || a >= clip.size[axis_a] || b < 0 || b >= clip.size[axis_b]) return span;

    i32 coord[3];
    coord[axis_a] = a;
    coord[axis_b] = b;

    i32 gaps = 0;
    bool inside_run = false;
    for (i32 i = 0; i < clip.size[axis]; ++i) {
        coord[axis] = i;
        const bool is_solid = clip.at(coord[0], coord[1], coord[2]) != kAir;
        const bool matches = (is_solid == want_solid);
        if (matches) {
            if (!span.any) {
                span.any = true;
                span.first = i;
            } else if (!inside_run) {
                ++gaps;
            }
            span.last = i;
            ++span.solid;
            inside_run = true;
        } else {
            inside_run = false;
        }
    }
    span.contiguous = span.any && (gaps == 0);
    return span;
}

}  // namespace

Span span_along(const Clip& clip, u32 axis, i32 a, i32 b) {
    return run_along(clip, axis, a, b, true);
}

Span gap_along(const Clip& clip, u32 axis, i32 a, i32 b) {
    return run_along(clip, axis, a, b, false);
}

u64 mirror_mismatch(const Clip& clip, u32 axis) {
    if (clip.empty()) return 0;
    u64 differ = 0;
    const i32 extent = clip.size[axis];
    for (i32 z = 0; z < clip.size[2]; ++z) {
        for (i32 y = 0; y < clip.size[1]; ++y) {
            for (i32 x = 0; x < clip.size[0]; ++x) {
                i32 mirrored[3] = {x, y, z};
                mirrored[axis] = extent - 1 - mirrored[axis];
                if (clip.at(x, y, z) != clip.at(mirrored[0], mirrored[1], mirrored[2])) ++differ;
            }
        }
    }
    return differ;
}

std::string slice_text(const Clip& clip, u32 axis, i32 at, i32 step) {
    std::string out;
    if (clip.empty()) return out;
    if (step < 1) step = 1;
    u32 axis_a = 0, axis_b = 0;
    other_axes(axis, axis_a, axis_b);
    if (at < 0 || at >= clip.size[axis]) return out;

    const i32 width = clip.size[axis_a];
    const i32 height = clip.size[axis_b];

    // Drawn with the second axis increasing upwards, because that is how a person reads a wall.
    for (i32 b = height - 1; b >= 0; b -= step) {
        for (i32 a = 0; a < width; a += step) {
            // The most common state in this block, so a wall one voxel thick still shows when
            // the slice is scaled down.
            i32 solid = 0;
            i32 covered = 0;
            i32 total = 0;
            for (i32 db = 0; db < step && b - db >= 0; ++db) {
                for (i32 da = 0; da < step && a + da < width; ++da) {
                    i32 coord[3];
                    coord[axis] = at;
                    coord[axis_a] = a + da;
                    coord[axis_b] = b - db;
                    ++total;
                    if (clip.at(coord[0], coord[1], coord[2]) != kAir) ++solid;
                    if (clip.covered(coord[0], coord[1], coord[2])) ++covered;
                }
            }
            if (total == 0) continue;
            // Solid wins ties: losing a thin wall to a scaled-down slice defeats the point.
            if (solid > 0) out += (solid * 2 >= total) ? '#' : '+';
            else if (covered > 0) out += '.';
            else out += ' ';
        }
        out += '\n';
    }
    return out;
}

Connectivity connectivity(const Clip& clip, usize report_at_most) {
    Connectivity out;
    if (clip.empty()) return out;

    const usize cells = static_cast<usize>(clip.size[0]) * static_cast<usize>(clip.size[1]) *
                        static_cast<usize>(clip.size[2]);
    std::vector<u8> seen(cells, 0);
    std::vector<i32> stack;
    std::vector<Island> islands;

    const auto solid = [&](i32 x, i32 y, i32 z) {
        return x >= 0 && y >= 0 && z >= 0 && x < clip.size[0] && y < clip.size[1] &&
               z < clip.size[2] && clip.at(x, y, z) != kAir;
    };

    for (i32 z = 0; z < clip.size[2]; ++z) {
        for (i32 y = 0; y < clip.size[1]; ++y) {
            for (i32 x = 0; x < clip.size[0]; ++x) {
                if (!solid(x, y, z)) continue;
                const usize start = clip.index(x, y, z);
                if (seen[start]) continue;

                Island island;
                island.low[0] = island.high[0] = x;
                island.low[1] = island.high[1] = y;
                island.low[2] = island.high[2] = z;

                seen[start] = 1;
                stack.clear();
                stack.push_back(x);
                stack.push_back(y);
                stack.push_back(z);
                while (!stack.empty()) {
                    const i32 cz = stack.back(); stack.pop_back();
                    const i32 cy = stack.back(); stack.pop_back();
                    const i32 cx = stack.back(); stack.pop_back();
                    ++island.voxels;
                    island.low[0] = std::min(island.low[0], cx);
                    island.low[1] = std::min(island.low[1], cy);
                    island.low[2] = std::min(island.low[2], cz);
                    island.high[0] = std::max(island.high[0], cx);
                    island.high[1] = std::max(island.high[1], cy);
                    island.high[2] = std::max(island.high[2], cz);

                    const i32 steps[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0},
                                             {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
                    for (const auto& step : steps) {
                        const i32 nx = cx + step[0], ny = cy + step[1], nz = cz + step[2];
                        if (!solid(nx, ny, nz)) continue;
                        const usize at = clip.index(nx, ny, nz);
                        if (seen[at]) continue;
                        seen[at] = 1;
                        stack.push_back(nx);
                        stack.push_back(ny);
                        stack.push_back(nz);
                    }
                }
                islands.push_back(island);
            }
        }
    }

    out.components = islands.size();
    std::sort(islands.begin(), islands.end(),
              [](const Island& a, const Island& b) { return a.voxels > b.voxels; });
    if (!islands.empty()) {
        out.largest = islands.front().voxels;
        for (usize i = 1; i < islands.size(); ++i) out.floating_voxels += islands[i].voxels;
        for (usize i = 1; i < islands.size() && out.islands.size() < report_at_most; ++i) {
            out.islands.push_back(islands[i]);
        }
    }
    return out;
}

Walkability walkability(const Clip& clip, i32 max_step, i32 head_room) {
    Walkability out;
    if (clip.empty()) return out;
    const i32 width = clip.size[0];
    const i32 depth = clip.size[2];

    // The top of the matter in each column, and whether there is room to stand on it.
    std::vector<i32> top(static_cast<usize>(width) * static_cast<usize>(depth), -1);
    for (i32 z = 0; z < depth; ++z) {
        for (i32 x = 0; x < width; ++x) {
            i32 found = -1;
            for (i32 y = clip.size[1] - 1; y >= 0; --y) {
                if (clip.at(x, y, z) == kAir) continue;
                // Standing needs clear air above.
                i32 clear = 0;
                for (i32 h = y + 1; h < clip.size[1] && clear < head_room; ++h) {
                    if (clip.at(x, h, z) != kAir) break;
                    ++clear;
                }
                if (clear >= head_room) found = y;
                break;
            }
            top[static_cast<usize>(z) * width + x] = found;
            if (found >= 0) ++out.surfaces;
        }
    }

    // The worst rise between neighbouring columns that both have a surface.
    for (i32 z = 0; z < depth; ++z) {
        for (i32 x = 0; x < width; ++x) {
            const i32 here = top[static_cast<usize>(z) * width + x];
            if (here < 0) continue;
            const i32 neighbours[2][2] = {{1, 0}, {0, 1}};
            for (const auto& step : neighbours) {
                const i32 nx = x + step[0], nz = z + step[1];
                if (nx >= width || nz >= depth) continue;
                const i32 there = top[static_cast<usize>(nz) * width + nx];
                if (there < 0) continue;
                const i32 rise = std::abs(there - here);
                // A genuine wall is not a bad step, so only rises a person might have been
                // meant to take are counted.
                if (rise > out.max_rise && rise <= max_step * 4) {
                    out.max_rise = rise;
                    out.max_rise_at[0] = x;
                    out.max_rise_at[1] = here;
                    out.max_rise_at[2] = z;
                }
            }
        }
    }

    // Flood from the lowest surface, stepping only where the rise is climbable.
    i32 start = -1;
    i32 lowest = clip.size[1];
    for (i32 z = 0; z < depth; ++z) {
        for (i32 x = 0; x < width; ++x) {
            const i32 here = top[static_cast<usize>(z) * width + x];
            if (here >= 0 && here < lowest) {
                lowest = here;
                start = static_cast<i32>(z) * width + x;
            }
        }
    }
    if (start < 0) return out;

    std::vector<u8> reached(top.size(), 0);
    std::vector<i32> queue{start};
    reached[static_cast<usize>(start)] = 1;
    while (!queue.empty()) {
        const i32 at = queue.back();
        queue.pop_back();
        ++out.reachable;
        const i32 x = at % width;
        const i32 z = at / width;
        const i32 here = top[static_cast<usize>(at)];
        const i32 steps[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
        for (const auto& step : steps) {
            const i32 nx = x + step[0], nz = z + step[1];
            if (nx < 0 || nz < 0 || nx >= width || nz >= depth) continue;
            const usize next = static_cast<usize>(nz) * width + nx;
            if (reached[next]) continue;
            const i32 there = top[next];
            if (there < 0) continue;
            if (std::abs(there - here) > max_step) continue;
            reached[next] = 1;
            queue.push_back(static_cast<i32>(next));
        }
    }
    return out;
}

std::string report(const Measurement& m, const std::vector<std::string>* names) {
    char line[256];
    std::string out;

    std::snprintf(line, sizeof(line), "sampled box   %d x %d x %d voxels   %.3f x %.3f x %.3f m\n",
                  m.size[0], m.size[1], m.size[2], m.metres(m.size[0]), m.metres(m.size[1]),
                  m.metres(m.size[2]));
    out += line;

    if (!m.extent.any) {
        out += "matter        none\n";
        return out;
    }

    std::snprintf(line, sizeof(line),
                  "matter extent %d x %d x %d voxels   %.3f x %.3f x %.3f m\n",
                  m.extent.span(0), m.extent.span(1), m.extent.span(2),
                  m.metres(m.extent.span(0)), m.metres(m.extent.span(1)),
                  m.metres(m.extent.span(2)));
    out += line;
    std::snprintf(line, sizeof(line), "              from (%d,%d,%d) to (%d,%d,%d)\n",
                  m.extent.low[0], m.extent.low[1], m.extent.low[2], m.extent.high[0],
                  m.extent.high[1], m.extent.high[2]);
    out += line;

    std::snprintf(line, sizeof(line),
                  "volume        %llu voxels   %.4f m3   %.1f litres   %.2f%% of the box\n",
                  static_cast<unsigned long long>(m.solid), m.cubic_metres(),
                  m.cubic_metres() * 1000.0,
                  (m.size[0] > 0)
                      ? 100.0 * static_cast<f64>(m.solid) /
                            (static_cast<f64>(m.size[0]) * m.size[1] * m.size[2])
                      : 0.0);
    out += line;

    std::snprintf(line, sizeof(line), "surface       %llu faces   %.3f m2\n",
                  static_cast<unsigned long long>(m.exposed_faces), m.square_metres());
    out += line;

    // How much surface there is for the volume enclosed. A smooth box sits near the bottom of
    // the range; a rasped or greebled one climbs, and a number climbing far past what the shape
    // should need is the sign of a displacement that has broken the surface into gravel.
    if (m.solid > 0) {
        std::snprintf(line, sizeof(line), "roughness     %.2f faces per voxel of volume\n",
                      static_cast<f64>(m.exposed_faces) / static_cast<f64>(m.solid));
        out += line;
    }

    std::snprintf(line, sizeof(line), "centroid      (%.1f, %.1f, %.1f) voxels   ",
                  m.centroid[0], m.centroid[1], m.centroid[2]);
    out += line;
    // Where the centroid sits as a fraction of the box, which is the form that shows a lean:
    // a symmetric shape reads 0.50 on the axes it is symmetric about.
    std::snprintf(line, sizeof(line), "(%.3f, %.3f, %.3f) of the box\n",
                  (m.size[0] > 0) ? m.centroid[0] / static_cast<f64>(m.size[0] - 1) : 0.0,
                  (m.size[1] > 0) ? m.centroid[1] / static_cast<f64>(m.size[1] - 1) : 0.0,
                  (m.size[2] > 0) ? m.centroid[2] / static_cast<f64>(m.size[2] - 1) : 0.0);
    out += line;

    // Capped, because per-voxel variation turns a dozen materials into tens of thousands of
    // records and a report nobody can read is a report nobody reads. The tail is summarised
    // rather than dropped, so the count is still honest.
    constexpr usize kShown = 12;
    std::snprintf(line, sizeof(line), "materials     %zu distinct records\n", m.types.size());
    out += line;
    usize shown = 0;
    u64 remainder = 0;
    for (const TypeShare& share : m.types) {
        if (shown >= kShown) {
            remainder += share.count;
            continue;
        }
        ++shown;
        const char* name = "";
        if (names != nullptr && static_cast<usize>(share.type) < names->size()) {
            name = (*names)[share.type].c_str();
        }
        std::snprintf(line, sizeof(line), "  %-5u %-16s %10llu   %6.2f%%\n",
                      static_cast<unsigned>(share.type), name,
                      static_cast<unsigned long long>(share.count), share.fraction * 100.0);
        out += line;
    }
    if (remainder > 0) {
        std::snprintf(line, sizeof(line), "  %-22s %10llu   %6.2f%%\n", "... and the rest",
                      static_cast<unsigned long long>(remainder),
                      (m.solid > 0) ? 100.0 * static_cast<f64>(remainder) /
                                          static_cast<f64>(m.solid)
                                    : 0.0);
        out += line;
    }
    return out;
}

SpeckReport paint_specks(const Clip& clip, usize examples_per_type) {
    SpeckReport out;
    if (clip.empty()) return out;

    // Counts by material. A map and not a vector indexed by type id: type ids are minted by the
    // world's table and are not dense, and a clip that uses six of them should not carry an array
    // as long as the largest one.
    std::map<VoxelTypeId, u64> surface;
    std::map<VoxelTypeId, u64> specks;
    std::map<VoxelTypeId, usize> shown;

    const auto type_at = [&](i32 x, i32 y, i32 z) -> VoxelTypeId {
        if (x < 0 || y < 0 || z < 0 || x >= clip.size[0] || y >= clip.size[1] ||
            z >= clip.size[2]) {
            return kAir;
        }
        return clip.at(x, y, z);
    };

    for (i32 z = 0; z < clip.size[2]; ++z) {
        for (i32 y = 0; y < clip.size[1]; ++y) {
            for (i32 x = 0; x < clip.size[0]; ++x) {
                const VoxelTypeId mine = clip.at(x, y, z);
                if (mine == kAir) continue;

                const VoxelTypeId around[6]{
                    type_at(x - 1, y, z), type_at(x + 1, y, z), type_at(x, y - 1, z),
                    type_at(x, y + 1, z), type_at(x, y, z - 1), type_at(x, y, z + 1),
                };
                bool exposed = false;
                bool kin = false;
                for (const VoxelTypeId near : around) {
                    if (near == kAir) {
                        exposed = true;
                    } else if (near == mine) {
                        kin = true;
                    }
                }
                // Buried voxels are not examined at all, rather than examined and found innocent.
                // What is under the surface is nobody's business and counting it would put the
                // fraction below on a denominator no one can see.
                if (!exposed) continue;

                ++out.surface;
                ++surface[mine];
                if (kin) continue;

                ++out.specks;
                ++specks[mine];
                // A FEW PER MATERIAL, not the first few found.
                //
                // Taken in scan order the whole list came back as twelve limestone voxels off one
                // cornice, because the scan starts at the bottom of the building and limestone is
                // most of it -- so the one material anybody already knew about crowded out the
                // twenty-six they did not. The point of a coordinate here is to put a camera on
                // the fault, and the faults are never all in one material.
                if (shown[mine] < examples_per_type) {
                    ++shown[mine];
                    Speck one;
                    one.type = mine;
                    one.at[0] = x;
                    one.at[1] = y;
                    one.at[2] = z;
                    out.examples.push_back(one);
                }
            }
        }
    }

    for (const auto& entry : specks) {
        TypeShare share;
        share.type = entry.first;
        share.count = entry.second;
        const auto seen = surface.find(entry.first);
        const u64 of = (seen == surface.end()) ? 0 : seen->second;
        share.fraction = (of > 0) ? static_cast<f64>(entry.second) / static_cast<f64>(of) : 0.0;
        out.by_type.push_back(share);
    }
    std::sort(out.by_type.begin(), out.by_type.end(),
              [](const TypeShare& a, const TypeShare& b) { return a.count > b.count; });
    return out;
}

StippleCounts stipple_counts(const Clip& clip) {
    StippleCounts out;
    if (clip.empty()) return out;
    const SpeckReport found = paint_specks(clip, 0);
    for (const TypeShare& share : found.by_type) {
        StippleCounts::Pair& pair = out.by_type[share.type];
        pair.specks = share.count;
        // `fraction` is specks over THAT material's own surface, so the denominator comes back out
        // of it. Rounded to nearest rather than truncated: the fraction is a double built from two
        // integers and 972/12303 does not come back as 12303 by flooring.
        pair.surface = (share.fraction > 0.0)
                           ? static_cast<u64>(static_cast<f64>(share.count) / share.fraction + 0.5)
                           : 0;
    }
    return out;
}

StippleVerdict stipple_verdict(const StippleCounts& counts, f64 stipple_share) {
    StippleVerdict out;
    // The same two tests as below, over counts from wherever they came. See the note there for why
    // a large SHARE is not enough on its own.
    constexpr u64 kStippleFloor = 16;
    for (const auto& [type, pair] : counts.by_type) {
        const f64 fraction = (pair.surface > 0) ? static_cast<f64>(pair.specks) /
                                                      static_cast<f64>(pair.surface)
                                                : 0.0;
        const bool stipple = fraction >= stipple_share && pair.specks >= kStippleFloor;
        out.allowed[type] = !stipple;
    }
    return out;
}

StippleVerdict stipple_verdict(const Clip& clip, f64 stipple_share) {
    StippleVerdict out;
    if (clip.empty()) return out;
    const SpeckReport found = paint_specks(clip, 0);
    // A STIPPLE IS A LARGE SHARE **AND** A LARGE NUMBER, and the second half was missing from the
    // first version of this. A material with one surface voxel in the whole clip and one speck has
    // a speck fraction of 100%, which read as "obviously a deliberate dither" and left the single
    // stray voxel exactly where it was. That is the case this whole pass exists for, and the
    // fraction test alone gets it precisely backwards -- the smaller the accident, the more
    // convinced the test was that it was on purpose. Caught by the test, not by reading it.
    //
    // So a material is only spared when its specks are both a large share of its own surface and
    // numerous in absolute terms. A weathering coat covers an area by definition: the facility's
    // limestone stipple is 972 specks at 7.9%. A dither of fewer than kStippleFloor voxels in an
    // entire clip is not a dither anybody can see, and whatever it is, it is not something the
    // author would notice losing.
    constexpr u64 kStippleFloor = 16;
    for (const TypeShare& share : found.by_type) {
        const bool stipple = share.fraction >= stipple_share && share.count >= kStippleFloor;
        out.allowed[share.type] = !stipple;
    }
    return out;
}

DespeckleReport despeckle(Clip& clip, f64 stipple_share, const StippleVerdict* verdict) {
    DespeckleReport out;
    if (clip.empty()) return out;

    // Which materials may be despeckled at all -- see StippleVerdict for why this is a question
    // about the whole clip and why a caller refining one node at a time has to bring its own.
    const StippleVerdict asked = (verdict == nullptr) ? stipple_verdict(clip, stipple_share)
                                                      : StippleVerdict{};
    const std::map<VoxelTypeId, bool>& allowed =
        (verdict == nullptr) ? asked.allowed : verdict->allowed;
    if (allowed.empty()) return out;

    const auto type_at = [&](i32 x, i32 y, i32 z) -> VoxelTypeId {
        if (x < 0 || y < 0 || z < 0 || x >= clip.size[0] || y >= clip.size[1] ||
            z >= clip.size[2]) {
            return kAir;
        }
        return clip.at(x, y, z);
    };

    // READ FROM A COPY AND WRITE TO THE CLIP, so that a repainted voxel cannot become one of the
    // neighbours the next voxel is judged against. In place, a run of three specks in a line would
    // repaint the first, then find the second no longer isolated, and leave it -- which makes the
    // result depend on the scan order. It has to be one simultaneous step or it is not a rule.
    const std::vector<VoxelTypeId> before = clip.voxels;
    std::map<VoxelTypeId, u64> repainted;

    for (i32 z = 0; z < clip.size[2]; ++z) {
        for (i32 y = 0; y < clip.size[1]; ++y) {
            for (i32 x = 0; x < clip.size[0]; ++x) {
                const VoxelTypeId mine = before[clip.index(x, y, z)];
                if (mine == kAir) continue;
                const auto may = allowed.find(mine);
                if (may == allowed.end()) continue;   // this material has no specks at all

                const VoxelTypeId around[6]{
                    type_at(x - 1, y, z), type_at(x + 1, y, z), type_at(x, y - 1, z),
                    type_at(x, y + 1, z), type_at(x, y, z - 1), type_at(x, y, z + 1),
                };
                bool exposed = false;
                bool kin = false;
                for (const VoxelTypeId near : around) {
                    if (near == kAir) {
                        exposed = true;
                    } else if (near == mine) {
                        kin = true;
                    }
                }
                if (!exposed || kin) continue;
                if (!may->second) {
                    ++out.left;   // a stipple, and it stays a stipple
                    continue;
                }

                // The material most of the neighbours wear. Ties go to the first in the fixed
                // order above rather than to whichever the map happens to iterate first, because
                // a clip must build identically every time on every machine.
                VoxelTypeId best = kAir;
                i32 best_count = 0;
                for (const VoxelTypeId near : around) {
                    if (near == kAir) continue;
                    i32 count = 0;
                    for (const VoxelTypeId other : around) {
                        if (other == near) ++count;
                    }
                    if (count > best_count) {
                        best_count = count;
                        best = near;
                    }
                }
                if (best == kAir) continue;   // a voxel with no solid neighbour at all: leave it

                clip.voxels[clip.index(x, y, z)] = best;
                ++out.repainted;
                ++repainted[mine];
            }
        }
    }

    for (const auto& entry : repainted) {
        TypeShare share;
        share.type = entry.first;
        share.count = entry.second;
        share.fraction = 0.0;
        out.by_type.push_back(share);
    }
    std::sort(out.by_type.begin(), out.by_type.end(),
              [](const TypeShare& a, const TypeShare& b) { return a.count > b.count; });
    if (out.repainted > 0) clip.build_coarse();
    return out;
}

}  // namespace forge
}  // namespace ws
