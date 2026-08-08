#pragma once
// The clip file: one text file that says everything about a clip.
//
// A clip is procedural. It is not a box of voxels somebody saved — it is a *description*, and
// the voxels are what you get when you evaluate it. That is what lets the same clip be stamped
// at a different size, re-cut at a different resolution, and have its numbers moved while you
// watch without any of it being rebuilt.
//
// So the file is the clip. Everything it needs is in it: its materials, its shapes, its
// patterns, which parts are painted with what, and how big a volume to cut it out of. Nothing
// refers to anything outside itself, which is what makes a clip a thing you can send someone.
//
// # The whole language
//
//   # anything after a hash is a comment
//
//   metre 32                     voxels to the metre when this is sampled
//   bounds -4 0 -4  4 3 4        the volume to cut, in metres
//   param  height 2.4            a number that can be moved later, by name
//
//   material stone rgb=120,120,116 rough=210
//   material lamp  rgb=255,240,200 emit=200
//
//   let slab  = box -4 0 -4  4 0.2 4 round=0.05
//   let post  = cylinder 0 1.2 0 r=0.15 h=1.2
//   let grain = fbm size=0.4 octaves=4
//   let all   = union { slab post }
//   let all   = displace { all grain } amount=0.02
//
//   let ring  = revolve { bead 0.15 0.9  0.24 1.05 } axis=y    a section turned about an axis
//   let curl  = spiral 0 2 0 r=0.3 tighten=0.6 tube=0.04 turns=2.5 axis=z
//
//   paint stone
//   paint moss  where=grain above=0.55 facing=y at=0.6
//   weather overgrown 0.4 on=post          only where that shape is; see WeatherRequest::scope
//
//   solid  all                   which expression is the matter
//   region all                   optional: which cells belong to the clip at all
//
// A name can be re-bound — `let all = displace { all grain }` reads as a pipeline and is the
// form most authoring actually takes. Numbers may be written as a literal or as the name of a
// `param`, and the latter is what makes a dial work later.
//
// # Why a text file and not a C++ function
//
// Because a player has to be able to write one, and because a visual editor has to be able to
// round-trip it. The file, the node graph and the boxes-and-wires a player drags are three views
// of the same list of nodes; the graph is the real one and this is its readable form. A clip
// authored by hand in a text editor and a clip authored by wiring nodes together produce the
// same array, and either can be saved as either.

#include <string>
#include <utility>
#include <vector>

#include "core/types.hpp"
#include "forge/field.hpp"
#include "forge/sample.hpp"
#include "world/voxel_type.hpp"

namespace ws {

class TagRegistry;

namespace forge {

struct ScriptError {
    u32 line = 0;
    std::string message;
};

// Where a line of the spliced text came from. A clip assembled out of fragments is one file to
// the parser and many files to the person who wrote it, and an error has to be reported to the
// second of those.
struct SourceLine {
    std::string file;
    u32 line = 0;
};

// How worn a clip is, and in what way.
//
// Weathering is not a texture laid over a surface. It is a consequence of the surface's own
// shape: sand piles where a wall meets the ground and blows off an exposed arris, moss grows
// where a corner stays damp, soot collects under an overhang and washes off a sill, cracks open
// across a face and branch where they meet. Every one of those is answerable from the geometry —
// which way a surface bends, how much sky it can see, which way it faces — so weathering here
// reads those and follows them, and a clip that changes shape weathers differently without
// anything being re-authored.
//
// Five kinds, each an amount from 0 to 1, and they compose: a burnt-out seaside ruin is
// `weather burnt 0.7` and `weather sea 0.5` and `weather cracks 0.4`, in any order.
enum class Weather : u8 {
    Desert,      // sand, bleaching, wind-scoured edges
    Overgrown,   // moss and growth in the damp and the dark
    Cracks,      // fissures that open and branch
    Burnt,       // char and soot, and edges softened by heat
    Sea,         // salt above the tide, barnacles and weed below it
    Count,
};

struct WeatherRequest {
    Weather kind = Weather::Desert;
    f64 amount = 0.0;
    f64 scale = 1.0;    // the size of its features, in metres
    u32 seed = 1;
    f64 level = 0.0;    // the datum it works from: the ground for sand, the tide for sea

    // Optional: a named shape the weathering is confined to. Without one it works on the whole
    // clip, which is only ever right when the whole clip is one material standing in one place.
    //
    // It was not optional in practice and pretending it was cost the feature. `weather desert` on
    // a building in a garden bleached the lawn as readily as the stone, because the coats it adds
    // go on after the author's and paint by a *value* rather than by a place — and the deformation
    // scoured the grass blades along with the arrises. The building went out without any
    // weathering at all rather than with that, which left the one system written specifically for
    // a stone building outdoors untested on the only stone building there is.
    //
    // Scoped, both halves are confined: the displacement is multiplied by how far inside the named
    // shape the point is, and every coat's test is pushed out of its own range wherever the shape
    // is not. So `weather desert 0.2 on=podium` weathers the podium and nothing that touches it.
    u32 scope = 0;
    bool has_scope = false;
};

struct Script {
    Field field;
    SampleSettings settings;
    std::vector<PaintRule> paint;
    std::vector<WeatherRequest> weather;
    Variation variation;

    u32 solid = 0;
    bool has_solid = false;

    // Every name the file bound, in order, with the node it ended up meaning. Kept so the tools
    // can talk about a clip in the author's own words — "the canopy is four centimetres off the
    // column below it" rather than "node 47 is off node 52".
    std::vector<std::pair<std::string, u32>> parts;

    // Names in the order they were declared, so a report can say "stone" rather than "type 3",
    // and a visual editor can show what the file called things.
    std::vector<std::string> material_names;
    std::vector<VoxelTypeId> material_types;

    std::vector<ScriptError> errors;
    std::vector<SourceLine> sources;   // one per line of the spliced text, when it was assembled
    bool ok() const { return errors.empty() && has_solid; }

    // The node a named part refers to, or false when nothing of that name was bound. Used to
    // build one piece of a clip on its own, which is how a fragment gets looked at without its
    // nineteen neighbours being built around it.
    bool part(const std::string& name, u32& out) const {
        for (const auto& entry : parts) {
            if (entry.first == name) {
                out = entry.second;
                return true;
            }
        }
        return false;
    }
};

// Parses a clip file. Materials are interned into `types` as they are declared.
//
// Errors are collected rather than thrown, and parsing continues past them, because an author
// wants to be told about all four mistakes in a file rather than the first one four times.
Script parse_clip_script(const std::string& text, VoxelTypeTable& types, const TagRegistry& tags);

// Reads the file and parses it. A missing file is an error like any other.
//
// A file may contain `include "other.clip"`, resolved relative to the file that says it. That is
// what lets one clip be written by many hands at once: each part lives in its own file, the
// manifest names them in order, and nobody is editing the same lines as anybody else. Errors are
// reported against the file and line the author actually wrote.
Script load_clip_script(const std::string& path, VoxelTypeTable& types, const TagRegistry& tags);

// Splices a file and everything it includes into one text, recording where each line came from.
// Exposed for tests; load_clip_script is what callers want.
std::string expand_includes(const std::string& path, std::vector<SourceLine>& origin,
                            std::vector<ScriptError>& errors);

}  // namespace forge
}  // namespace ws
