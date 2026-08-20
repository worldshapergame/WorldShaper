#pragma once
// Which voxels an edit is allowed to touch.
//
// Its own header because both ends of the engine need it: the op layer, where it is part of
// the record a peer replays, and the brick layer, where it decides whether a bulk write can
// take the fast path. Neither should have to include the other.

#include "core/types.hpp"
#include "world/voxel_type.hpp"

namespace ws {

enum class WriteMask : u8 {
    All = 0,     // every voxel in range
    IntoAir,     // only where the destination is empty; existing matter is never disturbed
    OntoSolid,   // only where the destination already holds something
};

constexpr bool mask_allows(WriteMask mask, VoxelTypeId destination) {
    switch (mask) {
        case WriteMask::IntoAir:   return destination == kAir;
        case WriteMask::OntoSolid: return destination != kAir;
        default:                   return true;
    }
}

// WHO is writing — R12d, and it is the whole of the stage's boundary in one byte.
//
// The point of R12 is that the card can derive the base world from the field, so the CPU need
// only STORE the difference a player has made to it. A brick nobody has touched is a brick the
// clip regenerates; a brick somebody chiselled is not, and there is nothing anywhere else that
// can tell the two apart. The voxels are identical: a wall the sampler wrote and a wall a person
// placed by hand out of the same material hash the same, occupy the same cells and encode the
// same way. The only difference is who put them there, so it has to be recorded at the moment of
// the write and can never be recovered afterwards.
//
// Hence a parameter with a DEFAULT of `Field`. Every existing writer — the ladder's paste, the
// cache reader laying a saved world down, a test building a fixture — keeps exactly the behaviour
// it had, and the edit path opts in by naming itself. That direction is deliberate: a writer that
// forgets to say it is an edit under-claims, and an under-claimed brick is one the field may
// regenerate; a default of `Edit` would over-claim, and an over-claimed world is one where nothing
// can be derived and the whole stage buys nothing. Wrong in the first direction is a lost carving
// and shows up the moment somebody looks; wrong in the second is silent for ever.
//
// It is NOT a mask: the mask says which destination voxels a write may touch, this says what the
// write MEANS. A carve is `Edit` whether it met matter or air.
enum class WriteOrigin : u8 {
    Field = 0,   // the sampler, re-deriving what the clip already describes
    Edit,        // a player: the chisel, a paste, an undo, a script's fill
};

// The control arm, and it is compile-time because the runtime flag it wants (`--no-edit-tracking`)
// is a line in `src/app/main.cpp`, which R12d was not allowed to write. False restores the
// behaviour exactly as it was before R12d: nothing is ever marked, `Chunk::brick_edited` always
// answers no, the re-sample overwrites whatever it lands on, and the cache carries no flag.
//
// It is a constant rather than a `#define` so both arms COMPILE in both settings — a dead branch
// that has not been through the compiler is not a control arm, it is a guess.
// R12d's control arm, and it is RUNTIME rather than `constexpr` so that both arms live in one
// binary -- which is the rule, and which a compile-time constant cannot satisfy. `--no-edit-tracking`
// clears it: nothing is marked and the field overwrites whatever it lands on, which is what every
// build before R12d did. In a running session the op replay after every paste hides that; across a
// reload it does not, and the carve comes back as stone.
inline bool g_edit_tracking = true;
inline bool edit_tracking() { return g_edit_tracking; }
inline void set_edit_tracking(bool on) { g_edit_tracking = on; }

}  // namespace ws
