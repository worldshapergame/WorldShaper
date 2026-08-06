#pragma once
// The clipboard: select a box, get a ghost of it, put copies of it wherever you want.
//
// The selection is literally the chisel — a Chisel instance drives it, so the two cannot
// drift apart. Every part of that interaction (the drag, the camera-relative distance, the
// snap at distance zero, the constraint points) works here because it *is* that code, not
// because it was copied and kept in step by hand.
//
// After the drag the clip exists as a ghost. Scroll to slide it along whichever axis you
// are facing, "." and "," to fan out extra copies, arrow keys to turn it. Nothing has
// touched the world yet; the left button is what commits, and it can commit as many times
// as you like.
//
// # Copies carry a share of the transform
//
// A row of copies is not a row of identical stamps. Turning the ghost 90° with four copies
// gives 22.5°, 45°, 67.5° and 90° — the transform ramps along the row and arrives at the
// full amount on the last one. That is what makes a spiral staircase, a fanned arcade or a
// tapering spire one gesture instead of a dozen.
//
// The cost is that each copy is its own baked clip, and baking is proportional to volume.
// Two things keep that affordable: with no transform every copy is the *same* clip, so one
// bake serves all of them; and the baked set is capped by total volume, past which the
// remaining copies show as outlines rather than silently costing a second a keypress.

#include <array>
#include <vector>

#include "core/types.hpp"
#include "game/chisel.hpp"
#include "game/clip.hpp"

namespace ws {

class World;

// What "," and "." are pointed at, cycled with "/".
//
// In Resize the up and down arrows change meaning too: instead of turning the clip they
// stretch it along whichever axis you are facing, so "make this wall longer" is look at the
// wall and hold up rather than a menu of three numbers.
enum class AdjustMode : u8 {
    Copies = 0,
    Resize,
    Count,
};

const char* adjust_mode_name(AdjustMode mode);

struct ClipboardInput {
    bool left = false;
    bool right = false;
    bool middle = false;
    f32 wheel = 0.0f;
    bool big_step = false;          // move by a whole clip length instead of one voxel
    bool adjust_distance = false;   // G, handed through to the selection drag
    bool toggle_paste_mode = false; // P
    bool toggle_snap = false;       // O
    bool cycle_adjust = false;      // /
    bool clear_points = false;      // R
    bool cancel = false;            // C
    u32 increase = 0;               // "." — already resolved to a repeat count this frame
    u32 decrease = 0;               // ","
    u32 turn_left = 0;              // arrow keys, likewise
    u32 turn_right = 0;
    u32 turn_up = 0;
    u32 turn_down = 0;
};

// How many ghosts the renderer can draw at once — a limit of the parameter block the shader
// reads, not of the tool. There is no cap on how many copies you can have; past this many
// only some of them are drawn, and the one you are steering is always among them.
inline constexpr u32 kMaxPreviewInstances = 16;

struct ClipboardPreview {
    bool selecting = false;    // still dragging the selection box
    bool holding = false;      // a clip exists and is following the camera
    bool too_large = false;    // the last thing asked for would not fit in memory
    u32 instances = 0;         // how many are *drawn*, not how many there are
    u32 total = 0;             // how many there actually are
    i64 min[kMaxPreviewInstances][3]{};
    i64 max[kMaxPreviewInstances][3]{};
    // Which baked shape each drawn instance uses. Several share one when the transform is
    // the same for all of them, which is the usual case.
    u32 shape[kMaxPreviewInstances]{};
    // The selection box while the drag is still going.
    i64 select_min[3]{};
    i64 select_max[3]{};
};

// There is no cap on how large a clip may be, how far it may be resized, or how many copies
// it may have. What there is instead is a refusal to do something the machine cannot do:
// a clip costs five bytes a cell, so asking for one that does not fit in memory fails the
// allocation, and that is caught and reported rather than taking the game down with it.
//
// Two things keep the cost of *not* having caps in hand:
//
//  - Only the copies that are drawn are baked and held. A thousand copies do not mean a
//    thousand clips in memory; they mean at most kMaxPreviewInstances of them.
//  - Stamping bakes each copy in turn and lets it go again, so putting a thousand down costs
//    one clip of memory at a time, however many there are.
//
// What is left is time. Baking runs at roughly 16 ns a cell, so a very large clip is a
// visible pause on the keypress that asks for it — reported in the developer panel as the
// bake time, rather than hidden behind a limit that stops you doing it.

class Clipboard {
public:
    // Returns the ops to apply, as one undoable action, when the player commits a stamp.
    bool update(const World& world, const ClipboardInput& input, const f64 origin[3],
                const f64 direction[3], f64 dt, u64 tick, u32 player, std::vector<Op>& out);

    void drop();   // forget the clip and go back to selecting

    // Put a clip in hand without selecting one. This is how a clip loaded from the library
    // will arrive in Stage 16, and it is how a scripted run sets up a screenshot without a
    // person holding the mouse.
    void hold(Clip clip, i64 anchor_x, i64 anchor_y, i64 anchor_z);
    void set_offset(i64 dx, i64 dy, i64 dz);
    void set_copies(i32 copies);
    void turn(u32 axis, f64 degrees);
    void resize(f64 factor);                    // all three axes at once
    void stretch(u32 axis, f64 factor);         // one axis

    bool holding() const { return !source_.empty(); }
    const ClipboardPreview& preview() const { return preview_; }
    const Clip& clip() const { return shapes_.empty() ? source_ : shapes_.back(); }
    const std::vector<Clip>& shapes() const { return shapes_; }
    PasteMode paste_mode() const { return paste_mode_; }
    AdjustMode adjust_mode() const { return adjust_mode_; }
    // O. On, the clip moves in whole clip lengths, so it carries its own grid rather than
    // the world's: copies tile edge to edge and a repeated piece lands where the last one
    // ended. Off, it moves a voxel at a time.
    //
    // This is as far as "the object has its own grid" goes for something that becomes world
    // voxels. A voxel world has exactly one lattice — that is what makes a voxel world a
    // voxel world — so a stamped clip is on it by definition. A clip that keeps its own
    // lattice at an arbitrary angle and offset is a *free-standing object*, which is a
    // different thing the engine is designed for but does not have yet: decision D56 in the
    // log, arriving with rigid bodies.
    bool grid_snap() const { return snap_; }
    // How many copies, and which side of the ghost they fall on.
    //
    //  positive — the copies fill the gap *between* the original and the ghost, evenly, with
    //             the last one landing exactly on the ghost. Aiming the ghost aims the row.
    //  negative — the ghost is the first step and the copies carry on *past* it, repeating
    //             that step. Where the ghost is a straight move from the original the row
    //             runs straight; where the ghost is also turned, each step turns the move
    //             as well, so the row bends round rather than continuing straight. The
    //             copies are not being rotated about some pivot — each one is placed by
    //             taking the same step again from where the last one ended.
    i32 copies() const { return copies_; }
    u32 instance_count() const;
    const f64* scale() const { return scale_; }

    // Degrees per key press: an eighth of a quarter turn when snapped, a twelfth when free.
    // Both divide 90 evenly, so a right angle is exactly reachable either way.
    f64 turn_step_degrees() const { return snap_ ? 11.25 : 7.5; }

    // How much one press of "." or "," changes a size by. Small on purpose: a size of
    // 4.5× or 9.843× has to be reachable, and holding the key is how you get there fast.
    static constexpr f64 kResizeStep = 1.02;

    // Ten turns each way. The accumulated angle is not wrapped — the copies carry fractions
    // of it, so wrapping snaps the whole row back at once — but it does need a stop.
    static constexpr f64 kMaxTurnDegrees = 3600.0;
    const f64* angles_degrees() const { return angles_; }
    const i64* offset() const { return offset_; }
    // Where the clip was lifted from. anchor + offset is where the ghost sits, which is what
    // the last copy of a chain has to land on.
    const i64* anchor() const { return anchor_; }
    u64 last_stamp_ops() const { return last_stamp_ops_; }
    f64 last_bake_ms() const { return last_bake_ms_; }
    u64 baked_cells() const { return baked_cells_; }
    bool baking_truncated() const { return baking_truncated_; }

    // Bumped whenever the baked shapes change — a new capture, a rotation, a resize, or a
    // change in how many copies share the ramp. Not bumped by a move. The renderer uploads
    // when this moves, which has to be O(1) to test: hashing eight million cells every
    // frame to find out nothing changed is the mistake chunk staleness made in Stage 2.
    u64 revision() const { return revision_; }

    // Where instance `n` of `count` lands. Exposed for the tests, which are the only way to
    // check the spacing without a camera.
    void instance_origin(u32 n, i64 out[3]) const;

    void refresh_preview();

private:
    void rebake();
    void apply_snap();
    void stretch_axis(u32 axis, f64 factor);
    // Builds copy `n`'s shape. False when the machine had no room for it.
    bool bake_instance(u32 n, Clip& out) const;
    // The translation of one link of a bent chain, chosen so the last copy lands on the ghost.
    void solve_step(f64 out[3]) const;
    // The share of the full transform copy `n` carries.
    f64 share(u32 n) const;

    Chisel selector_;
    Clip source_;              // exactly as captured, never re-transformed
    std::vector<Clip> shapes_; // one per *drawn* copy; one entry when they all match
    std::vector<u32> instance_shape_;   // indexed by preview slot, not by copy
    std::vector<u32> drawn_instances_;  // which copies those slots refer to
    i64 anchor_[3]{};          // where the source box started, in the world
    i64 offset_[3]{};
    f64 angles_[3]{0.0, 0.0, 0.0};       // degrees, accumulated, for the last copy
    f64 scale_[3]{1.0, 1.0, 1.0};        // per axis, for the last copy
    i32 copies_ = 1;                     // never 0; negative extrapolates past the ghost
    PasteMode paste_mode_ = PasteMode::SolidOnly;
    AdjustMode adjust_mode_ = AdjustMode::Copies;
    bool snap_ = false;
    bool prev_left_ = false;
    bool prev_right_ = false;
    bool prev_middle_ = false;
    // Scroll acceleration. A wheel does not report continuously — it reports a burst of
    // separate notches with idle frames between them — so the run is kept alive by a timer
    // rather than by the wheel being non-zero on consecutive frames. Decaying the speed on
    // every idle frame was the bug: at sixty frames a second, the gap between two notches
    // of a fast scroll was enough to wipe it, and it never sped up at all.
    f64 wheel_heat_ = 0.0;
    f64 wheel_idle_ = 0.0;
    f64 wheel_last_sign_ = 0.0;
    u64 last_stamp_ops_ = 0;
    f64 last_bake_ms_ = 0.0;
    u64 baked_cells_ = 0;
    bool baking_truncated_ = false;
    u64 revision_ = 0;
    ClipboardPreview preview_;
};

}  // namespace ws
