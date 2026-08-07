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
//   paint stone
//   paint moss  where=grain above=0.55 facing=y at=0.6
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

struct Script {
    Field field;
    SampleSettings settings;
    std::vector<PaintRule> paint;

    u32 solid = 0;
    bool has_solid = false;

    // Names in the order they were declared, so a report can say "stone" rather than "type 3",
    // and a visual editor can show what the file called things.
    std::vector<std::string> material_names;
    std::vector<VoxelTypeId> material_types;

    std::vector<ScriptError> errors;
    bool ok() const { return errors.empty() && has_solid; }
};

// Parses a clip file. Materials are interned into `types` as they are declared.
//
// Errors are collected rather than thrown, and parsing continues past them, because an author
// wants to be told about all four mistakes in a file rather than the first one four times.
Script parse_clip_script(const std::string& text, VoxelTypeTable& types, const TagRegistry& tags);

// Reads the file and parses it. A missing file is an error like any other.
Script load_clip_script(const std::string& path, VoxelTypeTable& types, const TagRegistry& tags);

}  // namespace forge
}  // namespace ws
