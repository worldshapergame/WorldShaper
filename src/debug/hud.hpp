#pragma once
// Developer HUD, plus the player-facing performance overlay.
//
// Answer O14: the overlay ships with the game, off by default, toggleable in settings.
// The full developer panels stay behind a key (F1) and are compiled into every build,
// because a performance problem that only reproduces on someone else's machine is
// exactly when you need them.

#include <string>

#include "core/time.hpp"
#include "gpu/profiler.hpp"
#include "gpu/swapchain.hpp"
#include "platform/window.hpp"

namespace ws {

// What the update check has to say. Plain strings, for the same reason StreamingReport uses
// plain numbers: the HUD should not have to know what an Updater is.
struct UpdateReport {
    bool show = false;
    bool offering = false;    // waiting on the player to say yes
    bool downloading = false;
    f32 progress = 0.0f;
    std::string headline;
};

// What the streaming system wants shown. Kept as plain numbers so the HUD does not pull
// the whole world module into every translation unit that includes it.
//
// These are the NODE POOL's numbers since R1e. They were chunk residency's, which is deleted:
// resident chunks and bricks, a payload pool sized from a chunk budget, and a cache hit rate
// over chunk requests. Everything here has the same meaning one structure further in.
struct StreamingReport {
    u64 world_chunks = 0;   // what the world holds, as the scale the rest is read against
    u64 nodes = 0;
    u64 leaves = 0;
    u64 payload_in_use = 0;
    u64 payload_capacity = 0;
    u64 resident_bytes = 0;
    // What the SCREEN is paying for: everything except the fixed-size entry table. R2b's rule is
    // about resident bytes following resolution, and a megabyte constant in the total is enough
    // to hide a threefold change in the part that moves.
    u64 screen_bytes = 0;
    u64 staged_bytes = 0;   // what went to the card this frame
    u64 builds = 0;
    u64 evictions = 0;
    // Evicted and then wanted again within kChurnWindow. The harm itself rather than a proxy for
    // it, and the number three separate flicker reports were eventually diagnosed from (D426).
    u64 churn = 0;
    u64 deferred = 0;       // wanted, and the frame's build budget ran out
    f64 hit_rate = 0.0;
    bool out_of_memory = false;

    // CPU time spent deciding and packing this frame's streaming work. This is separate
    // from the "streaming" GPU pass, which only measures the copies — and it is usually
    // the larger of the two, so leaving it off the HUD made the budget it is measured
    // against impossible to see.
    f64 update_ms = 0.0;
    f64 worst_update_ms = 0.0;
    u64 feedback_reports = 0;
    u64 feedback_dropped = 0;
    // Miss reports for a place the world has nothing at. A large number is D133.
    u64 feedback_phantom = 0;
};

// What the chisel wants shown. Same reasoning as StreamingReport: plain numbers, so the
// HUD does not drag the tool and world modules into everything that includes it.
struct ToolReport {
    bool active = false;
    bool dragging = false;
    bool holding = false;    // the clipboard has a clip following the camera
    bool too_large = false;
    bool carving = false;
    bool snapping = true;
    bool overwrite = true;
    bool against_face = true;
    bool grid_snap = false;
    i32 copies = 1;
    const char* paste_mode = "";
    const char* adjust_mode = "";
    char scale_text[32] = "1x";
    const char* active_tool = "";
    // What the chisel is painting with, in the clip author's own words. PROVISIONAL, and it is
    // here rather than pointed at because a `const char*` into the palette would be a dangling
    // pointer the day a world is opened while the belt is on screen — `scale_text` above is a
    // buffer for the same reason. See the belt in hud.cpp: the strip is a stand-in until the
    // interface stage gives it swatches, and a material with no name on screen is the one thing
    // about an edit a player cannot check before making it.
    char material[40]{};
    u32 material_index = 0;   // which entry of the palette, from nought
    u32 material_count = 0;   // how many Q and E step through
    u32 active_slot = 0;
    u32 slot_count[9]{};
    u32 slot_position[9]{};
    f64 turn_step = 0.0;
    f64 angles[3]{};
    f64 distance_metres = 0.0;
    i64 min[3]{};
    i64 max[3]{};
    u64 volume = 0;
    u32 constraints = 0;
    usize undo_depth = 0;
    usize redo_depth = 0;
    u64 history_bytes = 0;
    u64 last_edit_voxels = 0;
    f64 last_edit_ms = 0.0;
    u64 ops_logged = 0;
};

class Hud {
public:
    bool create(Device& device, Window& window, VkFormat colour_format);
    void destroy();

    void begin_frame();
    void set_streaming(const StreamingReport& report) { streaming_ = report; }
    void draw(const FrameStats& stats, const GpuProfiler& profiler,
              const DeviceCapabilities& caps, const Swapchain& swapchain);
    void render(VkCommandBuffer cmd, VkImageView target, VkExtent2D extent);

    // True when a panel is under the pointer, so clicking it does not also capture the
    // mouse into the world.
    bool wants_mouse() const;

    void set_tool(const ToolReport& report) { tool_ = report; }
    void set_update(const UpdateReport& report) { update_ = report; }

    void toggle_developer_panel() { show_developer_ = !show_developer_; }
    void toggle_overlay() { show_overlay_ = !show_overlay_; }
    // The same switch, said rather than flipped, so the settings window can say what it means
    // instead of having to know what state the key left it in.
    void set_overlay(bool on) { show_overlay_ = on; }
    bool overlay() const { return show_overlay_; }
    bool developer_panel_visible() const { return show_developer_; }

private:
    Device* device_ = nullptr;
    // The window this HUD hooked, so `destroy` can unhook it. The window outlives the HUD now
    // that a world is an object with a lifetime (D458); see the note in `destroy`.
    Window* window_ = nullptr;
    StreamingReport streaming_;
    ToolReport tool_;
    UpdateReport update_;
    bool initialised_ = false;
    bool reported_scale_ = false;
    bool show_developer_ = false;
    bool show_overlay_ = true;
};

}  // namespace ws
