#pragma once
// Where a window is allowed to be, which in this interface is "docked to an edge" and nothing else.
//
// documentation/23-shell-and-libraries.md §2 (D443). Two families of window, and the side carries
// the meaning: **settings and parameters open on the left, libraries on the right**, because
// *changing a number* and *choosing a thing* are different tasks and a consistent side means the
// eye knows where to go without reading a title bar. That is the same argument the icons are made
// of. Where a window *opens* is the family; where it *is* is wherever the player last dragged it,
// and it may be any edge.
//
// Four rules fall out of docking, and each rules out a failure the floating version has:
//
//   **Nothing floats.** A game that makes the player manage overlapping windows has handed them a
//   desktop. Docked windows cannot be lost behind each other, cannot be dragged off-screen, and
//   never need a "reset layout".
//
//   **A window never covers what it is about.** The world stays in the middle. A parameters window
//   editing a tool's reach is worth nothing if it is over the thing being reached at.
//
//   **Two windows on one edge share it**, splitting along that edge with a draggable divider,
//   rather than stacking on top of each other.
//
//   **A window remembers its size per edge**, because the useful width of a library docked left is
//   not its useful height docked top.
//
// Sizes are stored per player alongside the settings, and a missing or damaged layout costs the
// defaults rather than a crash — the same rule documentation/19-auto-quality.md states for
// `settings.txt`.

#include <filesystem>
#include <string>
#include <vector>

#include "core/types.hpp"
#include "ui/widgets.hpp"

namespace ws::ui {

enum class Edge : u32 { Left = 0, Right = 1, Top = 2, Bottom = 3, Count = 4 };

// Which side a window opens on when it has never been dragged anywhere.
enum class Family : u32 {
    Parameters,   // numbers and switches: game settings, a tool's, a node's, a material's
    Library,      // things you can pick: worlds, clips, materials, mods, characters, scripts
};

class Dock {
public:
    // Adds a window and returns the handle everything else refers to it by. The name is what the
    // layout file keys on, so renaming one costs a player their remembered size for it and
    // nothing worse.
    u32 add(std::string name, Family family);

    void set_open(u32 window, bool open);
    bool is_open(u32 window) const;
    void toggle(u32 window);
    // Which edge it is on now, and moving it there. A move is what the grip does; this is what a
    // test and a settings row do.
    Edge edge_of(u32 window) const;
    void dock_to(u32 window, Edge edge);

    // Works out every rectangle for this screen and runs the drags that change them: the band's
    // own size, and the divider between two windows sharing one edge. Call once a frame, before
    // any window draws itself.
    void layout(Ui& ui, const Rect& screen);

    const Rect& rect(u32 window) const;
    // What is left in the middle, which is where the world is and what a window may never cover.
    const Rect& centre() const { return centre_; }

    // The handle a window puts in its own header. Dragging it picks the window up; while it is up,
    // the edge under the pointer is shown as a band, and letting go docks it there. Returns true
    // on the frame it lands somewhere new.
    bool grip(Ui& ui, u32 window, const Rect& header);

    // Per player, beside the settings. A layout that will not read costs the defaults.
    bool read(const std::filesystem::path& path);
    bool write(const std::filesystem::path& path) const;

    usize size() const { return windows_.size(); }
    const std::string& name_of(u32 window) const { return windows_[window].name; }

private:
    struct Window {
        std::string name;
        Family family = Family::Parameters;
        Edge edge = Edge::Left;
        bool open = false;
        // How much of the screen it takes on each edge, as a fraction of that edge's own
        // dimension. Four numbers because a window remembers its size per edge.
        f32 fraction[static_cast<usize>(Edge::Count)]{0.24f, 0.28f, 0.30f, 0.30f};
        Rect rect{};
    };

    // Which windows are on an edge, in the order they were opened, so that two of them split it
    // predictably rather than by whichever was added to the list first.
    std::vector<u32> on(Edge edge) const;

    std::vector<Window> windows_;
    // Where the divider between the two windows sharing an edge sits, along that edge.
    f32 split_[static_cast<usize>(Edge::Count)]{0.5f, 0.5f, 0.5f, 0.5f};
    Rect centre_{};
    Rect screen_{};

    // The window being carried, and where it would land if it were let go now. Zero is "nothing",
    // which is why handles are one-based inside this one field.
    u32 carrying_ = 0;
    Edge landing_ = Edge::Left;
};

// Smallest and largest a band may be, as a fraction of the screen. A window that can be dragged to
// nothing is a window a player loses; one that can take the whole screen has covered the thing it
// is about, which is the failure docking exists to prevent.
inline constexpr f32 kBandSmallest = 0.12f;
inline constexpr f32 kBandLargest = 0.55f;

}  // namespace ws::ui
