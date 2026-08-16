// clipcheck — the clip tool, without a window, a card, or a Vulkan SDK.
//
// `WorldShaper.exe --clip-file ...` already answers every question this answers, and it is the
// tool the brief tells a fragment author to run. It is also linked into the game executable, so
// asking it anything costs an SDL fetch, a Vulkan SDK and a Windows toolchain. That is the right
// trade on the machine the game is built on and the wrong one everywhere else: a person — or an
// agent — writing a `.clip` is editing text that is parsed and sampled by `ws_forge`, which
// depends on nothing but the standard library, and they should not need a graphics stack to find
// out that they typed `cylinder` where they meant `capsule`.
//
// So this is the same four calls the game makes — parse, sample, despeckle, measure — against the
// same libraries, with the report cut down to what an author acts on:
//
//   errors        with the file and line the author actually wrote, through the include splice
//   extent        and the worldbox, which is where the part really is
//   components    1, or a list of what is floating and where
//   materials     including THE RULES THAT NEVER FIRED, which is the failure that looks like success
//   spans         head height and doorway width, on request
//   a slice       when a number will not do
//
// Build it with tools/clipcheck.sh. It is not part of the CMake build and does not belong in it:
// the game's build already contains this tool, and a second copy of it in the same build system is
// two things to keep in step.
//
// Usage:
//   clipcheck <file.clip> [--part part_name] [--metre 8] [--slice x|y|z@2.0] [--quiet]
//             [--span y@0,0] [--gap x@1.5,-9.0] [--list]

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "core/jobs.hpp"
#include "forge/clip_script.hpp"
#include "forge/measure.hpp"
#include "forge/sample.hpp"
#include "world/voxel_type.hpp"
#include "world/tags.hpp"

using namespace ws;

namespace {

// An error carries a line of the SPLICED text, and the author wrote a line of one of twenty-odd
// fragments. Reporting the first is reporting a line number in a file that does not exist.
void say_error(const forge::Script& script, const forge::ScriptError& error) {
    if (error.line > 0 && error.line <= script.sources.size()) {
        const forge::SourceLine& where = script.sources[error.line - 1];
        std::printf("ERROR  %s:%u: %s\n", where.file.c_str(), where.line, error.message.c_str());
    } else if (error.line > 0) {
        std::printf("ERROR  line %u: %s\n", error.line, error.message.c_str());
    } else {
        std::printf("ERROR  %s\n", error.message.c_str());
    }
}

struct Axis {
    u32 axis = 1;
    f64 a = 0.0;
    f64 b = 0.0;
    bool given = false;
};

// "y@1.5,-3.0" — an axis, and where on the other two. Written this way because a span is always
// asked as "how tall is it here", and "here" is two numbers in metres.
Axis parse_axis(const char* text) {
    Axis out;
    if (!text || !*text) return out;
    switch (text[0]) {
        case 'x': out.axis = 0; break;
        case 'y': out.axis = 1; break;
        case 'z': out.axis = 2; break;
        default: return out;
    }
    const char* at = std::strchr(text, '@');
    if (at) {
        out.a = std::atof(at + 1);
        const char* comma = std::strchr(at + 1, ',');
        if (comma) out.b = std::atof(comma + 1);
    }
    out.given = true;
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    std::string path;
    std::string part;
    i32 metre = 0;
    Axis slice;
    Axis span;
    Axis gap;
    bool quiet = false;
    bool list = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--part" && i + 1 < argc) {
            part = argv[++i];
        } else if (arg == "--metre" && i + 1 < argc) {
            metre = std::atoi(argv[++i]);
        } else if (arg == "--slice" && i + 1 < argc) {
            slice = parse_axis(argv[++i]);
        } else if (arg == "--span" && i + 1 < argc) {
            span = parse_axis(argv[++i]);
        } else if (arg == "--gap" && i + 1 < argc) {
            gap = parse_axis(argv[++i]);
        } else if (arg == "--quiet") {
            quiet = true;
        } else if (arg == "--list") {
            list = true;
        } else if (!arg.empty() && arg[0] != '-') {
            path = arg;
        }
    }
    if (path.empty()) {
        std::printf("usage: clipcheck <file.clip> [--part name] [--metre n] [--list]\n"
                    "                 [--slice y@2.0] [--span y@0,0] [--gap x@1.5,-9] [--quiet]\n");
        return 2;
    }

    VoxelTypeTable types;
    TagRegistry tags;
    forge::Script script = forge::load_clip_script(path, types, tags);
    for (const forge::ScriptError& error : script.errors) say_error(script, error);
    if (!script.errors.empty()) return 1;

    if (list) {
        std::printf("parts (%zu)\n", script.parts.size());
        for (const auto& entry : script.parts) std::printf("  %s\n", entry.first.c_str());
        if (!script.has_solid) return 1;
    }
    if (!script.has_solid) {
        std::printf("ERROR  no `solid` statement — nothing says what the matter is\n");
        return 1;
    }

    if (metre > 0) script.settings.voxels_per_metre = metre;

    if (!part.empty()) {
        u32 piece = 0;
        if (!script.part(part, piece)) {
            std::printf("ERROR  no part called '%s'\n", part.c_str());
            std::printf("parts ");
            for (const auto& entry : script.parts) std::printf(" %s", entry.first.c_str());
            std::printf("\n");
            return 1;
        }
        script.solid = piece;
    }

    JobSystem jobs;
    const forge::SampleResult built =
        forge::sample(script.field, script.solid, script.paint, script.settings, &jobs);

    // The game despeckles before it measures (D610) and so does this, or the numbers are about a
    // clip nobody will ever see.
    Clip clip = built.clip;
    forge::despeckle(clip);

    const forge::Measurement m = forge::measure(clip, script.settings.voxels_per_metre);
    const f64 per = static_cast<f64>(script.settings.voxels_per_metre);

    if (!quiet) std::printf("%s", forge::report(m, &script.material_names).c_str());

    if (!m.extent.any) {
        std::printf("EMPTY  the sample contains no matter at all%s\n",
                    part.empty() ? "" : " — check the part name and its bounds");
        return 1;
    }

    const f64 low[3] = {static_cast<f64>(built.origin_voxel[0] + m.extent.low[0]) / per,
                        static_cast<f64>(built.origin_voxel[1] + m.extent.low[1]) / per,
                        static_cast<f64>(built.origin_voxel[2] + m.extent.low[2]) / per};
    const f64 high[3] = {static_cast<f64>(built.origin_voxel[0] + m.extent.high[0] + 1) / per,
                         static_cast<f64>(built.origin_voxel[1] + m.extent.high[1] + 1) / per,
                         static_cast<f64>(built.origin_voxel[2] + m.extent.high[2] + 1) / per};
    std::printf("worldbox      %.3f %.3f %.3f   %.3f %.3f %.3f  m\n", low[0], low[1], low[2],
                high[0], high[1], high[2]);

    // Is it one thing? The brief's first test of "done", and the one that catches a part that
    // does not touch what it stands on.
    const forge::Connectivity joined = forge::connectivity(clip);
    std::printf("components    %llu (largest %llu voxels, %llu floating)\n",
                static_cast<unsigned long long>(joined.components),
                static_cast<unsigned long long>(joined.largest),
                static_cast<unsigned long long>(joined.floating_voxels));
    if (joined.components > 1) {
        usize shown = 0;
        for (const forge::Island& island : joined.islands) {
            if (island.voxels == joined.largest) continue;
            if (shown++ >= 6) break;
            std::printf("  floating    %llu voxels at %.2f %.2f %.2f m\n",
                        static_cast<unsigned long long>(island.voxels),
                        static_cast<f64>(built.origin_voxel[0] + island.low[0]) / per,
                        static_cast<f64>(built.origin_voxel[1] + island.low[1]) / per,
                        static_cast<f64>(built.origin_voxel[2] + island.low[2]) / per);
        }
    }

    // A paint rule that never fired is a coat somebody wrote and nothing wears. It costs nothing,
    // paints nothing, produces no error, and is invisible in every other number here.
    {
        std::vector<usize> idle;
        for (usize i = 0; i < built.rule_evaluations.size(); ++i) {
            if (built.rule_evaluations[i] == 0) idle.push_back(i);
        }
        if (!idle.empty()) {
            std::printf("never fired   %zu of %zu rules painted nothing%s\n", idle.size(),
                        built.rule_evaluations.size(),
                        part.empty() ? "" : " (one part only — most belong to other fragments)");
            for (usize n = 0; n < idle.size() && n < 16; ++n) {
                const usize i = idle[n];
                std::printf("              %s\n", i < script.paint_source.size()
                                                      ? script.paint_source[i].c_str()
                                                      : "?");
            }
        }
    }

    if (span.given) {
        const i32 a = static_cast<i32>(span.a * per) - built.origin_voxel[(span.axis + 1) % 3];
        const i32 b = static_cast<i32>(span.b * per) - built.origin_voxel[(span.axis + 2) % 3];
        const forge::Span s = forge::span_along(clip, span.axis, a, b);
        std::printf("span          %.3f m of matter along %c (%s)\n",
                    static_cast<f64>(s.length()) / per, "xyz"[span.axis],
                    s.contiguous ? "unbroken" : "BROKEN — there is a gap in it");
    }
    if (gap.given) {
        const i32 a = static_cast<i32>(gap.a * per) - built.origin_voxel[(gap.axis + 1) % 3];
        const i32 b = static_cast<i32>(gap.b * per) - built.origin_voxel[(gap.axis + 2) % 3];
        const forge::Span s = forge::gap_along(clip, gap.axis, a, b);
        std::printf("gap           %.3f m of air along %c (%s)\n",
                    static_cast<f64>(s.length()) / per, "xyz"[gap.axis],
                    s.contiguous ? "clear" : "BROKEN — there is matter in it");
    }
    if (slice.given) {
        const i32 at = static_cast<i32>(slice.a * per) - built.origin_voxel[slice.axis];
        const i32 size = m.size[slice.axis];
        const i32 step = std::max(1, size / 110);
        std::printf("%s", forge::slice_text(clip, slice.axis, at, step).c_str());
    }

    return 0;
}
