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

}  // namespace forge
}  // namespace ws
