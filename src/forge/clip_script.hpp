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
//   let tree  = branch 0 0 0 h=1.4 r=0.06 levels=5 count=3 spread=0.11 lean=0.3 seed=7
//   let bed   = scatter { pebble } x=0.07 z=0.07 nx=24 nz=24 jitter=0.45 turn=0.5
//   let arris = intersection { face_a face_b } chamfer=0.02   a FLAT cut on one seam
//   let bark  = fbm size=0.06 octaves=4 stretch=1,9,1         a grain that runs one way
//   let craze = cell_edge size=0.12 seed=4                    the seams, which is what a crack is
//   let cavity= occlusion { part_portico } r=0.22             0 in the open, 1 buried
//   let arris_wear = curvature { part_portico } r=0.10        + on an edge, - in a corner
//   let up    = facing { part_portico } axis=y                + up-facing, - down
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

    // What each paint rule was WRITTEN as, in step with `paint`. Carried for diagnostics only.
    //
    // A build that spends three quarters of itself on five of a hundred and thirty-eight rules has
    // to be able to say which five, in the words the author used. Without this the answer is a
    // rule index, and turning an index into a line means counting `paint` statements across
    // twenty included files by hand — which is how two of the measurements on this file were taken
    // against the wrong rule.
    std::vector<std::string> paint_source;

    std::vector<WeatherRequest> weather;

    // Where the clip's own origin should end up, from an `origin` statement. Applied to the solid
    // AND to every paint rule at once, because moving one without the other is the mistake this
    // exists to make impossible.
    f64 origin_shift[3]{0.0, 0.0, 0.0};
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
// How wide a union has to be before a hierarchy is built over its leaves' boxes, for every clip
// parsed after this is called. `Field::kAccelerateNever` is off, and off is the default.
//
// **D637 built this, measured it on the FACILITY, and refused it** -- 67.2 s against 53.6 at metre
// 16 -- for a reason that is about buildings rather than about trees: *the parts of a building are
// LAYERS and not regions*, so every part's box spans the block, a point in a wall is genuinely
// inside a dozen of them, and a hierarchy that cannot reject is a traversal paid on top of the scan
// it replaced. That reasoning is sound and it is why this is off.
//
// **It also says, in its own last paragraph, what would change it:** *"a clip of separated buildings
// is the case it was written for and nobody has authored one yet."* **Somebody has now** -- the
// estate is seven buildings on a site plan, each a `translate` joining one union of seven (D672),
// and those ARE regions rather than layers. So the refusal stays right about the facility and is an
// open question about the estate, which is the clip the game now ships.
//
// It had NO CALLER for that whole time, which is the part worth noticing: `accelerator_count()` has
// read nought on every clip ever built, and nothing anywhere said that was a switch rather than a
// result.
void accelerate_unions_from(usize leaves);

// Rewrite every clip parsed after this into an equivalent field that is cheaper to walk, for every
// clip parsed after it is called. `--compile-field`, and it is OFF.
//
// **`forge::compile_field` is 1.20x on the cost of asking the field a point** (11.68 µs → 9.92 on
// the estate) and bit-exact near every surface — see `forge/compile.hpp` for what it rewrites and
// what it measured. It is off by default because 1.20x is 1.20x of the 30x R12c needs (D687), so it
// is a step and not an answer, and because a pass that rewrites the expression a whole world is cut
// from wants both arms of one build available to whoever is comparing hashes.
//
// **What makes this a switch here rather than a line in the sampler**: a `Script` names MANY nodes —
// `solid`, `settings.bounds`, and every paint rule's `test` and `place` — and a compilation
// renumbers all of them at once. They are handed over together and come back together. Anything the
// script kept for DIAGNOSTICS rather than for building (`parts`, a weathering scope) is re-pointed
// through the compiler's own remap, and a name whose node did not survive as itself is dropped
// rather than left aimed at whatever now occupies its old index. So `--part` on a compiled clip
// says "does not name anything" instead of sampling the wrong shape: trap 7, a refusal and a wrong
// answer must not look alike.
void compile_fields(bool on);

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
//
// `beside` is where an include is looked for when it is not next to the file that asked for it —
// the folder of clips the game ships with (D494). It is a FALLBACK and not a search path: a file
// sitting next to the one including it always wins, so a player who copies the facility's parts
// into their own folder and edits them gets their own edits. Without it, a world assembled out of
// pieces could only ever be opened from the one folder those pieces were copied into, and deleting
// that folder — which looks like an ordinary folder — emptied the world.
std::string expand_includes(const std::string& path, std::vector<SourceLine>& origin,
                            std::vector<ScriptError>& errors,
                            const std::string& beside = {});

}  // namespace forge
}  // namespace ws
