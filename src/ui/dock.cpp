#include "ui/dock.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>

namespace ws::ui {
namespace {

bool horizontal(Edge edge) { return edge == Edge::Left || edge == Edge::Right; }

}  // namespace

u32 Dock::add(std::string name, Family family) {
    Window window;
    window.name = std::move(name);
    window.family = family;
    window.edge = (family == Family::Library) ? Edge::Right : Edge::Left;
    windows_.push_back(window);
    return static_cast<u32>(windows_.size() - 1);
}

void Dock::set_open(u32 window, bool open) { windows_[window].open = open; }
bool Dock::is_open(u32 window) const { return windows_[window].open; }
void Dock::toggle(u32 window) { windows_[window].open = !windows_[window].open; }
Edge Dock::edge_of(u32 window) const { return windows_[window].edge; }
void Dock::dock_to(u32 window, Edge edge) { windows_[window].edge = edge; }
const Rect& Dock::rect(u32 window) const { return windows_[window].rect; }

std::vector<u32> Dock::on(Edge edge) const {
    std::vector<u32> found;
    for (u32 i = 0; i < windows_.size(); ++i) {
        if (windows_[i].open && windows_[i].edge == edge) found.push_back(i);
    }
    return found;
}

void Dock::layout(Ui& ui, const Rect& screen) {
    screen_ = screen;
    const f32 grab = ui.metrics().px(5.0f);

    // The bands first, then what is left. A band's size is the largest its members remember for
    // this edge: two windows sharing a side share a width, and the one that wanted more gets it.
    f32 size[static_cast<usize>(Edge::Count)]{};
    std::vector<u32> members[static_cast<usize>(Edge::Count)];
    for (u32 e = 0; e < static_cast<u32>(Edge::Count); ++e) {
        const Edge edge = static_cast<Edge>(e);
        members[e] = on(edge);
        if (members[e].empty()) continue;
        f32 fraction = 0.0f;
        for (u32 window : members[e]) fraction = std::max(fraction, windows_[window].fraction[e]);
        fraction = std::clamp(fraction, kBandSmallest, kBandLargest);
        size[e] = horizontal(edge) ? screen.width() * fraction : screen.height() * fraction;
    }

    // Left and right take the full height and the top and bottom take what is between them. It
    // has to be one way round or the other; this one keeps the two families that actually exist —
    // parameters left, libraries right — as full-height columns, which is the shape a list of
    // things and a column of numbers both want.
    centre_ = Rect{screen.x0 + size[static_cast<usize>(Edge::Left)],
                   screen.y0 + size[static_cast<usize>(Edge::Top)],
                   screen.x1 - size[static_cast<usize>(Edge::Right)],
                   screen.y1 - size[static_cast<usize>(Edge::Bottom)]};

    for (u32 e = 0; e < static_cast<u32>(Edge::Count); ++e) {
        const Edge edge = static_cast<Edge>(e);
        if (members[e].empty()) continue;

        Rect band;
        switch (edge) {
            case Edge::Left: band = Rect{screen.x0, screen.y0, screen.x0 + size[e], screen.y1}; break;
            case Edge::Right: band = Rect{screen.x1 - size[e], screen.y0, screen.x1, screen.y1}; break;
            case Edge::Top: band = Rect{centre_.x0, screen.y0, centre_.x1, screen.y0 + size[e]}; break;
            default: band = Rect{centre_.x0, screen.y1 - size[e], centre_.x1, screen.y1}; break;
        }

        // Two on one edge SPLIT it rather than stacking. Three or more share it evenly, which is
        // not a case the specification names and is the only answer that does not hide one.
        const usize count = members[e].size();
        for (usize i = 0; i < count; ++i) {
            Rect own = band;
            if (count == 2) {
                const f32 at = std::clamp(split_[e], 0.2f, 0.8f);
                if (horizontal(edge)) {
                    const f32 y = band.y0 + band.height() * at;
                    own = (i == 0) ? Rect{band.x0, band.y0, band.x1, y}
                                   : Rect{band.x0, y, band.x1, band.y1};
                } else {
                    const f32 x = band.x0 + band.width() * at;
                    own = (i == 0) ? Rect{band.x0, band.y0, x, band.y1}
                                   : Rect{x, band.y0, band.x1, band.y1};
                }
            } else if (count > 2) {
                const f32 lo = static_cast<f32>(i) / static_cast<f32>(count);
                const f32 hi = static_cast<f32>(i + 1) / static_cast<f32>(count);
                if (horizontal(edge)) {
                    own = Rect{band.x0, band.y0 + band.height() * lo, band.x1,
                               band.y0 + band.height() * hi};
                } else {
                    own = Rect{band.x0 + band.width() * lo, band.y0, band.x0 + band.width() * hi,
                               band.y1};
                }
            }
            windows_[members[e][i]].rect = own;
        }

        // --- the band's own size ---------------------------------------------------------------
        //
        // Dragged by the inner edge, which is the one facing the world. Resizing is silent and has
        // no animation: it is not a press, and nothing has happened yet.
        Rect handle;
        switch (edge) {
            case Edge::Left: handle = Rect{band.x1 - grab, band.y0, band.x1 + grab, band.y1}; break;
            case Edge::Right: handle = Rect{band.x0 - grab, band.y0, band.x0 + grab, band.y1}; break;
            case Edge::Top: handle = Rect{band.x0, band.y1 - grab, band.x1, band.y1 + grab}; break;
            default: handle = Rect{band.x0, band.y0 - grab, band.x1, band.y0 + grab}; break;
        }
        // The value being dragged is the band's own fraction of the screen, so a drag tracks the
        // pointer's MOVEMENT rather than jumping the edge to wherever the press landed — which is
        // the same rule every other drag in this interface follows, and the reason it is one call.
        // A right or bottom band grows as the pointer moves the other way, which `inverted` says.
        const u64 id = id_of("dock.size", e + 1);
        const bool along_x = horizontal(edge);
        const f32 extent = along_x ? screen.width() : screen.height();
        f64 fraction = static_cast<f64>(size[e] / std::max(extent, 1.0f));
        const bool inverted = (edge == Edge::Right || edge == Edge::Bottom);
        if (ui.drag_handle(hash_combine(id, 7), handle, along_x, inverted, extent, fraction)) {
            const f32 wanted = std::clamp(static_cast<f32>(fraction), kBandSmallest, kBandLargest);
            for (u32 window : members[e]) windows_[window].fraction[e] = wanted;
        }

        // --- the divider between two on one edge -------------------------------------------------
        if (count == 2) {
            Rect divider;
            const f32 at = std::clamp(split_[e], 0.2f, 0.8f);
            if (horizontal(edge)) {
                const f32 y = band.y0 + band.height() * at;
                divider = Rect{band.x0, y - grab, band.x1, y + grab};
            } else {
                const f32 x = band.x0 + band.width() * at;
                divider = Rect{x - grab, band.y0, x + grab, band.y1};
            }
            f64 as_value = static_cast<f64>(split_[e]);
            const f32 extent_along = horizontal(edge) ? band.height() : band.width();
            if (ui.drag_handle(hash_combine(id, 9), divider, !horizontal(edge), false, extent_along,
                               as_value)) {
                split_[e] = std::clamp(static_cast<f32>(as_value), 0.2f, 0.8f);
            }
            ui.divider(divider, !horizontal(edge));
        }
    }

    // --- a window being carried to another edge ------------------------------------------------
    if (carrying_ != 0) {
        const Rect band = [&] {
            switch (landing_) {
                case Edge::Left: return Rect{screen.x0, screen.y0, screen.x0 + screen.width() * 0.24f, screen.y1};
                case Edge::Right: return Rect{screen.x1 - screen.width() * 0.24f, screen.y0, screen.x1, screen.y1};
                case Edge::Top: return Rect{screen.x0, screen.y0, screen.x1, screen.y0 + screen.height() * 0.28f};
                default: return Rect{screen.x0, screen.y1 - screen.height() * 0.28f, screen.x1, screen.y1};
            }
        }();
        // What it would look like if you let go. Not a preview of the contents — an interface that
        // redraws a whole window under a dragging hand is an interface that stutters.
        ui.draw().ink(band, 0.14f);
        ui.draw().edge(band, 0.8f, std::max(2.0f, ui.metrics().scale));
    }
}

bool Dock::grip(Ui& ui, u32 window, const Rect& header) {
    const u32 handle = window + 1;
    bool landed = false;

    if (ui.pressed_in(header) && carrying_ == 0) carrying_ = handle;
    if (carrying_ != handle) return false;

    // The nearest edge to the pointer, by how far it is from each. A drag that has to reach the
    // outermost few pixels to dock is a drag that fails on a trackpad.
    const f32 x = ui.pointer_x();
    const f32 y = ui.pointer_y();
    const f32 to[static_cast<usize>(Edge::Count)]{x - screen_.x0, screen_.x1 - x, y - screen_.y0,
                                                  screen_.y1 - y};
    u32 nearest = 0;
    for (u32 e = 1; e < static_cast<u32>(Edge::Count); ++e) {
        if (to[e] < to[nearest]) nearest = e;
    }
    landing_ = static_cast<Edge>(nearest);

    if (ui.released()) {
        if (windows_[window].edge != landing_) {
            windows_[window].edge = landing_;
            landed = true;
            ui.sound().say(Cue::Open);
        }
        carrying_ = 0;
    }
    return landed;
}

bool Dock::read(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file) return false;

    // Deliberately the plainest format that works, and deliberately forgiving: a line it does not
    // understand is skipped, a window it has never heard of is ignored, and a number out of range
    // is clamped. A damaged layout costs the defaults and never a crash.
    std::string word;
    while (file >> word) {
        if (word == "split") {
            u32 edge = 0;
            f32 at = 0.5f;
            if (!(file >> edge >> at)) break;
            if (edge < static_cast<u32>(Edge::Count)) split_[edge] = std::clamp(at, 0.2f, 0.8f);
        } else if (word == "window") {
            std::string name;
            u32 edge = 0;
            u32 open = 0;
            f32 fractions[4]{};
            if (!(file >> name >> edge >> open >> fractions[0] >> fractions[1] >> fractions[2] >>
                  fractions[3])) {
                break;
            }
            for (Window& window : windows_) {
                if (window.name != name) continue;
                window.edge = static_cast<Edge>(std::min(edge, 3u));
                window.open = (open != 0);
                for (u32 i = 0; i < 4; ++i) {
                    window.fraction[i] = std::clamp(fractions[i], kBandSmallest, kBandLargest);
                }
            }
        } else {
            std::string rest;
            std::getline(file, rest);
        }
    }
    return true;
}

bool Dock::write(const std::filesystem::path& path) const {
    std::ofstream file(path, std::ios::trunc);
    if (!file) return false;
    for (u32 e = 0; e < static_cast<u32>(Edge::Count); ++e) {
        file << "split " << e << " " << split_[e] << "\n";
    }
    for (const Window& window : windows_) {
        file << "window " << window.name << " " << static_cast<u32>(window.edge) << " "
             << (window.open ? 1 : 0);
        for (u32 i = 0; i < 4; ++i) file << " " << window.fraction[i];
        file << "\n";
    }
    return file.good();
}

}  // namespace ws::ui
