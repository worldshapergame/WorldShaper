#include "ui/shell.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>

#include "core/log.hpp"
#include "core/version.hpp"

namespace ws::ui {
namespace {

// A column of rows down a docked window. Every panel in this interface is one of these, which is
// why it is nine lines rather than a layout engine.
struct Column {
    Rect box;
    f32 y = 0.0f;
    f32 row = 0.0f;
    f32 gap = 0.0f;

    Rect next(f32 height = 0.0f) {
        const f32 tall = (height > 0.0f) ? height : row;
        const Rect out{box.x0, y, box.x1, y + tall};
        y += tall + gap;
        return out;
    }
    void skip(f32 by) { y += by; }
};

std::string spell_bytes(u64 bytes) {
    char text[32];
    if (bytes >= (1ull << 30)) {
        std::snprintf(text, sizeof(text), "%.1f GB", static_cast<f64>(bytes) / (1ull << 30));
    } else if (bytes >= (1ull << 20)) {
        std::snprintf(text, sizeof(text), "%.1f MB", static_cast<f64>(bytes) / (1ull << 20));
    } else if (bytes >= 1024) {
        std::snprintf(text, sizeof(text), "%.0f kB", static_cast<f64>(bytes) / 1024.0);
    } else {
        std::snprintf(text, sizeof(text), "%llu B", static_cast<unsigned long long>(bytes));
    }
    return text;
}

// The three tabs every library window has. What changes between libraries is what KIND of thing
// they are about, never the shape.
constexpr Icon kTabIcons[3]{Icon::Library, Icon::Community, Icon::Editor};
constexpr std::string_view kTabLabels[3]{"library", "community", "editor"};

}  // namespace

bool Preferences::read(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file) return false;
    // The plainest format that works, and forgiving on the way in: a key it does not know is
    // skipped and a damaged file costs the defaults. Same rule as settings.txt.
    std::string key;
    while (file >> key) {
        if (key == "username") {
            std::string name;
            std::getline(file, name);
            while (!name.empty() && (name.front() == ' ' || name.front() == '\t')) name.erase(0, 1);
            while (!name.empty() && (name.back() == '\r' || name.back() == ' ')) name.pop_back();
            if (!name.empty()) username = name;
        } else if (key == "accent") {
            f32 r = 0.0f, g = 0.0f, b = 0.0f;
            if (!(file >> r >> g >> b)) break;
            accent[0] = std::clamp(r, 0.0f, 1.0f);
            accent[1] = std::clamp(g, 0.0f, 1.0f);
            accent[2] = std::clamp(b, 0.0f, 1.0f);
        } else if (key == "logo") {
            u32 rgb = 0;
            if (!(file >> rgb)) break;
            logo_rgb = rgb & 0xFFFFFFu;
        } else if (key == "sound") {
            u32 on = 1;
            if (!(file >> on)) break;
            sound = (on != 0);
        } else if (key == "volume") {
            f32 value = 0.6f;
            if (!(file >> value)) break;
            volume = std::clamp(value, 0.0f, 1.0f);
        } else if (key == "share") {
            u32 on = 1;
            if (!(file >> on)) break;
            share = (on != 0);
        } else if (key == "interface_scale") {
            f32 value = 0.0f;
            if (!(file >> value)) break;
            interface_scale = std::clamp(value, 0.0f, 8.0f);
        } else {
            std::string rest;
            std::getline(file, rest);
        }
    }
    return true;
}

bool Preferences::write(const std::filesystem::path& path) const {
    std::ofstream file(path, std::ios::trunc);
    if (!file) return false;
    file << "accent " << accent[0] << " " << accent[1] << " " << accent[2] << "\n"
         << "logo " << logo_rgb << "\n"
         << "sound " << (sound ? 1 : 0) << "\n"
         << "volume " << volume << "\n"
         << "share " << (share ? 1 : 0) << "\n"
         << "interface_scale " << interface_scale << "\n"
         // Last, and on its own line, because a name may contain spaces and everything above may
         // not. A reader that has to guess where a value ends is a reader that eats the next key.
         << "username " << username << "\n";
    return file.good();
}

Shell::Shell() {
    window_settings_ = dock_.add("settings", Family::Parameters);
    window_worlds_ = dock_.add("worlds", Family::Library);
}

void Shell::set_stage(Stage stage) { stage_ = stage; }

void Shell::load(const std::filesystem::path& root) {
    root_ = root;
    library_ = Library(root);
    library_.ensure_folders();
    library_.open(shipped_kinds()[kind_]);
    preferences_.read(root / "shell.txt");
    dock_.read(root / "layout.txt");
    // Whatever the layout said, the shell starts with nothing open. A title that comes up with
    // two windows on it is not two buttons and nothing else (D442).
    dock_.set_open(window_settings_, false);
    dock_.set_open(window_worlds_, false);
}

void Shell::seed_worlds(const std::filesystem::path& shipped) {
    std::error_code error;
    const std::filesystem::path shelf = root_ / "worlds";
    std::filesystem::create_directories(shelf, error);
    if (!std::filesystem::is_empty(shelf, error) || error) return;
    if (!std::filesystem::is_directory(shipped, error)) return;

    const Kind* worlds = nullptr;
    for (const Kind& kind : shipped_kinds()) {
        if (kind.folder == "worlds") worlds = &kind;
    }
    if (worlds == nullptr) return;

    u32 copied = 0;
    for (const std::filesystem::directory_entry& item :
         std::filesystem::directory_iterator(shipped, error)) {
        if (!item.is_regular_file(error)) continue;
        if (item.path().extension() != ".clip") continue;
        const std::string stem = item.path().stem().string();
        const std::filesystem::path target = shelf / (stem + worlds->extension);
        std::filesystem::copy_file(item.path(), target, error);
        if (error) {
            error.clear();
            continue;
        }

        // And its fragments, if it has any. A clip's `include` is resolved relative to the file
        // doing the including, so a building assembled out of twenty pieces copied without its
        // own folder is a building that loads as an empty sky — which is exactly what the first
        // attempt at this produced, silently, because a missing include is a warning and not a
        // failure.
        const std::filesystem::path pieces = item.path().parent_path() / stem;
        if (std::filesystem::is_directory(pieces, error)) {
            std::filesystem::copy(pieces, shelf / stem,
                                  std::filesystem::copy_options::recursive, error);
            error.clear();
        }

        // And the world already BUILT from it, if there is one beside it.
        //
        // A cached world is minutes of sampling that has already been paid for, and it is keyed on
        // the source text with the author tag excluded — so a stamped copy still matches it. Not
        // copying this is what made the first open of every world on the shelf a cold rebuild,
        // which reads as the fast loading path not being used at all, because it was not.
        for (const char* sidecar : {".world", ".load"}) {
            const std::filesystem::path from = item.path().string() + sidecar;
            if (!std::filesystem::exists(from, error)) {
                error.clear();
                continue;
            }
            std::filesystem::copy_file(from, shelf / (stem + worlds->extension + sidecar), error);
            error.clear();
        }

        // Stamped with the name it was actually made under, not with the player's. A file that
        // says who made it has to say the truth or it says nothing.
        write_author(target, *worlds, "worldshaper");
        ++copied;
    }
    if (copied > 0) WS_LOG_INFO("library", "put {} shipped worlds on the shelf", copied);
    library_.refresh(true);
}

void Shell::save() const {
    if (root_.empty()) return;
    preferences_.write(root_ / "shell.txt");
    dock_.write(root_ / "layout.txt");
}

void Shell::toggle_windows() {
    const bool any = windows_open();
    dock_.set_open(window_worlds_, !any);
    if (any) dock_.set_open(window_settings_, false);
    ui_.sound().say(any ? Cue::Close : Cue::Open);
}

void Shell::open_window(std::string_view which, bool open) {
    if (which == "worlds" || which == "both") dock_.set_open(window_worlds_, open);
    if (which == "settings" || which == "both") dock_.set_open(window_settings_, open);
}

bool Shell::windows_open() const {
    return dock_.is_open(window_worlds_) || dock_.is_open(window_settings_);
}

bool Shell::selected(const Entry& entry) const {
    return std::find(chosen_.begin(), chosen_.end(), entry.name) != chosen_.end();
}

void Shell::select_only(const Entry& entry) {
    chosen_.clear();
    chosen_.push_back(entry.name);
    last_clicked_ = entry.name;
}

void Shell::select_add(const Entry& entry) {
    const auto at = std::find(chosen_.begin(), chosen_.end(), entry.name);
    if (at == chosen_.end()) {
        chosen_.push_back(entry.name);
    } else {
        chosen_.erase(at);
    }
    last_clicked_ = entry.name;
}

void Shell::select_range(const std::vector<Entry>& entries, const Entry& to) {
    usize from_at = 0;
    usize to_at = 0;
    bool found_from = false;
    for (usize i = 0; i < entries.size(); ++i) {
        if (entries[i].name == last_clicked_) {
            from_at = i;
            found_from = true;
        }
        if (entries[i].name == to.name) to_at = i;
    }
    if (!found_from) {
        select_only(to);
        return;
    }
    chosen_.clear();
    const usize lo = std::min(from_at, to_at);
    const usize hi = std::max(from_at, to_at);
    for (usize i = lo; i <= hi; ++i) chosen_.push_back(entries[i].name);
}

std::vector<Entry> Shell::selection() const {
    std::vector<Entry> found;
    for (const Entry& entry : library_.entries()) {
        if (selected(entry)) found.push_back(entry);
    }
    return found;
}

void Shell::clear_selection() {
    chosen_.clear();
    last_clicked_.clear();
}

Verdict Shell::frame(const InputState& input, u32 width, u32 height, f64 seconds) {
    Verdict verdict;
    seconds_ = seconds;

    // Interface pixels are sized from the window's SHORT side, so a layout keeps its proportions
    // on a wide monitor instead of growing until the column runs off the sides. The same rule the
    // loading screen already uses, and the same divisor.
    const f32 across = static_cast<f32>(std::min(width, height));
    const f32 scale = (preferences_.interface_scale > 0.0f)
                          ? preferences_.interface_scale
                          : std::max(1.0f, std::floor(across / 420.0f));

    ui_.begin(input, width, height, seconds, scale);
    ui_.set_accent(Colour{preferences_.accent[0], preferences_.accent[1], preferences_.accent[2]});
    ui_.sound().set_enabled(preferences_.sound);
    ui_.sound().set_volume(preferences_.volume);

    library_.refresh();

    if (stage_ == Stage::Title) draw_title(verdict);

    dock_.layout(ui_, ui_.screen());
    if (dock_.is_open(window_settings_)) draw_settings(dock_.rect(window_settings_));
    if (dock_.is_open(window_worlds_)) draw_library(dock_.rect(window_worlds_), verdict);

    if (message_until_ > seconds_ && !message_.empty()) {
        const Rect screen = ui_.screen();
        const f32 size = ui_.metrics().text();
        const Rect at{screen.x0, screen.y1 - ui_.metrics().px(28.0f), screen.x1,
                      screen.y1 - ui_.metrics().px(8.0f)};
        ui_.label(at, message_, Align::Centre);
        (void)size;
    }

    // Escape at the title is how you leave, because a third button saying "quit" would be a third
    // of the interface spent on the one action every player already knows how to do (D442). It
    // only counts when nothing is being typed into: the same key backs out of a field.
    if (stage_ == Stage::Title && input.was_pressed(Key::Escape) && !ui_.wants_keys()) {
        if (windows_open()) {
            dock_.set_open(window_settings_, false);
            dock_.set_open(window_worlds_, false);
            ui_.sound().say(Cue::Close);
        } else {
            verdict.quit = true;
        }
    }

    ui_.end();
    return verdict;
}

void Shell::draw_title(Verdict& verdict) {
    const Rect screen = ui_.screen();
    const Metrics& metrics = ui_.metrics();
    const f32 across = std::min(screen.width(), screen.height());

    // The logo, and it is the only surface on this screen with a hue of its own. Above the two
    // buttons rather than between them, because a mark between two controls reads as a third.
    //
    // High enough that the name below it clears the doorway the room's light comes through: the
    // ink rule handles a word crossing a bright patch correctly, per pixel, and correctly is not
    // the same as well — a title half inverted reads as a rendering fault to anybody who does not
    // already know the rule.
    const f32 mark = across * 0.18f;
    const f32 mark_top = screen.height() * 0.12f;
    const Rect mark_at{screen.mid_x() - mark * 0.5f, mark_top, screen.mid_x() + mark * 0.5f,
                       mark_top + mark};

    // Which combination it is drawing, and when that changes. A press on the mark asks for another
    // one; so does nobody being here for a while. Both live in `src/ui/logo.hpp` because *when the
    // picture changes* is a question about presses and idleness rather than about pixels.
    logo_.tick(seconds_, ui_.touched());
    if (ui_.pressable(id_of("title.logo"), mark_at,
                      "The mark, drawn from a seed. Press it for another one")) {
        logo_.change(seconds_);
    }
    // What the sort has reached, for the combination on screen and for the one it is replacing. Both,
    // because during a morph both are being drawn and each has its own slices in its own order.
    ui_.draw().logo(mark_at, preferences_.logo_rgb, logo_.seed(), logo_.previous(),
                    logo_.age(seconds_), logo_.morph(seconds_),
                    logo_frame(logo_.seed(), seconds_), logo_frame(logo_.previous(), seconds_));

    // The name, in the same face as everything else. Larger, because a title is allowed to be
    // large; not a second typeface, because there is not one.
    const f32 name_size = metrics.scale * 6.0f;
    ui_.draw().text(screen.mid_x(), mark_top + mark + metrics.px(10.0f), "WorldShaper", name_size,
                    kPlain, Align::Centre);

    // Two buttons. That is the whole screen.
    const f32 button_w = std::min(across * 0.26f, metrics.px(190.0f));
    const f32 button_h = button_w * 0.62f;
    const f32 gap = metrics.px(18.0f);
    const f32 row_y = screen.y0 + screen.height() * 0.62f;
    const Rect worlds{screen.mid_x() - button_w - gap * 0.5f, row_y,
                      screen.mid_x() - gap * 0.5f, row_y + button_h};
    const Rect settings{screen.mid_x() + gap * 0.5f, row_y, screen.mid_x() + gap * 0.5f + button_w,
                        row_y + button_h};

    // Stacked — the drawing above the word — because this is the one place with room enough that
    // the icon does not have to give any of it up.
    const bool want_worlds = ui_.button(id_of("title.worlds"), worlds, Icon::World, "worlds",
                                        "Your worlds: open one, make one, or organise them", true);
    const bool want_settings = ui_.button(id_of("title.settings"), settings, Icon::Settings,
                                          "settings",
                                          "Everything the game will let you change about itself",
                                          true);

    if (want_worlds) {
        dock_.set_open(window_worlds_, true);
        tab_ = 0;
        ui_.sound().say(Cue::Open);
    }
    if (want_settings) {
        dock_.set_open(window_settings_, true);
        ui_.sound().say(Cue::Open);
    }

    // What is running, small, in the corner where a version number belongs. Not furniture: it is
    // the first thing anybody is asked for when something goes wrong.
    ui_.draw().text(screen.x1 - metrics.px(10.0f), screen.y1 - metrics.px(18.0f),
                    std::string("v") + kVersion, metrics.text(), kPlain, Align::Right, 0.45f);
    (void)verdict;
}

void Shell::draw_header(const Rect& rect, u32 window, Icon icon, std::string_view title) {
    const Metrics& metrics = ui_.metrics();
    const Rect header{rect.x0, rect.y0, rect.x1, rect.y0 + metrics.row()};
    ui_.draw().ink(header, 0.10f);

    const f32 cell = metrics.icon();
    ui_.draw().icon(Rect{header.x0 + metrics.px(6.0f), header.mid_y() - cell * 0.5f,
                         header.x0 + metrics.px(6.0f) + cell, header.mid_y() + cell * 0.5f},
                    icon);
    ui_.label(Rect{header.x0 + metrics.px(10.0f) + cell, header.y0, header.x1, header.y1}, title);

    const Rect close{header.x1 - metrics.row(), header.y0, header.x1, header.y1};
    if (ui_.icon_button(id_of("window.close", window), close, Icon::Close, "Close this window")) {
        dock_.set_open(window, false);
        ui_.sound().say(Cue::Close);
    }
    // The rest of the header is the grip: drag it to any edge and the window goes there. There is
    // no other way to move a window, because there is nowhere else for one to be.
    dock_.grip(ui_, window, Rect{header.x0, header.y0, close.x0, header.y1});
    ui_.divider(Rect{header.x0, header.y1, header.x1, header.y1 + 1.0f});
}

void Shell::draw_settings(const Rect& rect) {
    const Metrics& metrics = ui_.metrics();
    ui_.panel(rect);
    draw_header(rect, window_settings_, Icon::Settings, "settings");

    const Rect body{rect.x0 + metrics.px(6.0f), rect.y0 + metrics.row() + metrics.px(6.0f),
                    rect.x1 - metrics.px(6.0f), rect.y1 - metrics.px(6.0f)};

    // Counted rather than measured: every row here is one row tall and the sections are known, so
    // the height of the content is arithmetic and does not have to be a frame late.
    constexpr u32 kRows = 17;
    constexpr u32 kSections = 4;
    const f32 content = static_cast<f32>(kRows) * (metrics.row() + metrics.px(4.0f)) +
                        static_cast<f32>(kSections) * metrics.px(22.0f);

    Column column;
    column.box = ui_.begin_scroll(id_of("settings.scroll"), body, content);
    column.y = column.box.y0;
    column.row = metrics.row();
    column.gap = metrics.px(4.0f);

    // A section heading is BOLD and at full strength. It was drawn at three quarters for a while,
    // which is the one thing that cancels a weight: a heavier letter that is also fainter than the
    // rows under it reads as a row that has been disabled, not as a heading.
    const auto section = [&](std::string_view name) {
        column.skip(metrics.px(8.0f));
        const Rect head = column.next(metrics.px(14.0f));
        ui_.label(head, name, Align::Left, kBold, 1.0f);
        ui_.divider(Rect{head.x0, head.y1, head.x1, head.y1 + 1.0f});
    };

    // --- you ------------------------------------------------------------------------------
    section("you");
    {
        const Rect row = column.next();
        const f32 label_room = row.width() * 0.36f;
        ui_.label(Rect{row.x0 + metrics.px(6.0f), row.y0, row.x0 + label_room, row.y1}, "name");
        std::string name = preferences_.username;
        if (ui_.field(id_of("settings.username"), Rect{row.x0 + label_room, row.y0, row.x1, row.y1},
                      name, "who made this",
                      "The name every file you make is stamped with, for ever")) {
            if (!name.empty()) preferences_.username = name;
        }
    }
    ui_.toggle(id_of("settings.share"), column.next(), "offer my library while online",
               preferences_.share,
               "While you are online, other players can browse what is in your library");

    // --- interface -------------------------------------------------------------------------
    section("interface");
    {
        // Every numeric value is a slider, and a colour is three of them. Not a colour wheel: a
        // wheel is a control nobody can type into, and a typed value is the whole of D444.
        static constexpr std::string_view kChannels[3]{"accent red", "accent green", "accent blue"};
        for (u32 i = 0; i < 3; ++i) {
            Number about;
            about.label = kChannels[i];
            about.tooltip = "Your own colour, used only where inverting has nothing to say";
            about.low = 0.0;
            about.high = 1.0;
            about.decimals = 2;
            f64 value = preferences_.accent[i];
            if (ui_.number(id_of("settings.accent", i), column.next(), about, value)) {
                preferences_.accent[i] = std::clamp(static_cast<f32>(value), 0.0f, 1.0f);
            }
        }
        const Rect swatch = column.next(metrics.px(10.0f));
        ui_.draw().hue(swatch.inset(metrics.px(2.0f)),
                       (static_cast<u32>(preferences_.accent[0] * 255.0f) << 16) |
                           (static_cast<u32>(preferences_.accent[1] * 255.0f) << 8) |
                           static_cast<u32>(preferences_.accent[2] * 255.0f));
    }
    {
        Number about;
        about.label = "interface size";
        about.tooltip = "How large everything is. Nought works it out from the window";
        about.low = 0.0;
        about.high = 6.0;
        about.step = 1.0;
        about.decimals = 0;
        f64 value = preferences_.interface_scale;
        if (ui_.number(id_of("settings.scale"), column.next(), about, value)) {
            preferences_.interface_scale = std::clamp(static_cast<f32>(value), 0.0f, 8.0f);
        }
    }

    // --- sound -----------------------------------------------------------------------------
    section("sound");
    ui_.toggle(id_of("settings.sound"), column.next(), "sound", preferences_.sound,
               "Off is exactly silent, not merely quiet");
    {
        Number about;
        about.label = "volume";
        about.tooltip = "How loud the interface is";
        about.low = 0.0;
        about.high = 1.0;
        about.decimals = 2;
        f64 value = preferences_.volume;
        if (ui_.number(id_of("settings.volume"), column.next(), about, value)) {
            preferences_.volume = std::clamp(static_cast<f32>(value), 0.0f, 1.0f);
        }
    }

    // --- picture ----------------------------------------------------------------------------
    section("picture");
    if (ui_.toggle(id_of("settings.auto"), column.next(), "hold the frame rate",
                   knobs_.auto_quality,
                   "Spend detail to keep the frame rate, measured on this machine")) {
        knobs_.changed = true;
    }
    {
        Number about;
        about.label = "frame rate to hold";
        about.tooltip = "Nought aims at whatever this monitor refreshes at";
        about.low = 0.0;
        about.high = 240.0;
        about.step = 1.0;
        about.decimals = 0;
        about.guard = [](f64 value) -> std::string {
            if (value < 0.0) return "a negative frame rate would aim at nothing";
            return {};
        };
        if (ui_.number(id_of("settings.fps"), column.next(), about, knobs_.target_fps)) {
            knobs_.changed = true;
        }
    }
    {
        Number about;
        about.label = "detail";
        about.tooltip = "Which rung of the quality ladder the renderer is on";
        about.low = 0.0;
        about.high = 7.0;
        about.step = 1.0;
        about.decimals = 0;
        if (ui_.number(id_of("settings.quality"), column.next(), about, knobs_.quality_level)) {
            knobs_.auto_quality = false;
            knobs_.changed = true;
        }
    }
    {
        Number about;
        about.label = "render scale";
        about.tooltip = "What fraction of the window the world is drawn at before being scaled up";
        about.low = 0.25;
        about.high = 1.0;
        about.decimals = 2;
        // The named example from D444: a value that would break the game is refused, and the
        // refusal says in one line what it would have done.
        about.guard = [](f64 value) -> std::string {
            if (value <= 0.0) return "a render scale of nought would draw no pixels at all";
            if (value > 4.0) return "over four times the window is more pixels than any card has";
            return {};
        };
        if (ui_.number(id_of("settings.render_scale"), column.next(), about, knobs_.render_scale)) {
            knobs_.changed = true;
        }
    }
    {
        Number about;
        about.label = "field of view";
        about.tooltip = "How wide the view is, in degrees";
        about.low = 50.0;
        about.high = 110.0;
        about.step = 1.0;
        about.decimals = 0;
        about.guard = [](f64 value) -> std::string {
            if (value <= 1.0 || value >= 179.0) {
                return "a view that wide or that narrow has no picture in it";
            }
            return {};
        };
        if (ui_.number(id_of("settings.fov"), column.next(), about, knobs_.field_of_view)) {
            knobs_.changed = true;
        }
    }
    if (ui_.toggle(id_of("settings.vsync"), column.next(), "wait for the display", knobs_.vsync,
                   "Draw in step with the monitor, so a frame is never torn in half")) {
        knobs_.changed = true;
    }
    if (ui_.toggle(id_of("settings.blur"), column.next(), "motion blur", knobs_.motion_blur,
                   "Smear fast movement the way a camera does")) {
        knobs_.changed = true;
    }
    if (ui_.toggle(id_of("settings.overlay"), column.next(), "performance overlay",
                   knobs_.overlay, "A small readout of what each frame is costing")) {
        knobs_.changed = true;
    }

    ui_.end_scroll();
}

void Shell::draw_library(const Rect& rect, Verdict& verdict) {
    const Metrics& metrics = ui_.metrics();
    ui_.panel(rect);
    const Kind& kind = shipped_kinds()[kind_];
    draw_header(rect, window_worlds_, kind.icon, kind.label);

    f32 top = rect.y0 + metrics.row();

    // The way out of a world, and the only one. It is here rather than on the title because
    // leaving is a thing you do TO a world, and the window that lists worlds is where a world is
    // a thing you can point at. Escape opens this window; it no longer ends the game, because a
    // key that loses a world nobody meant to leave is a key that costs somebody an afternoon.
    if (stage_ == Stage::World && !playing_.empty()) {
        const Rect row{rect.x0, top, rect.x1, top + metrics.row()};
        ui_.draw().ink(row, 0.08f);
        ui_.label(Rect{row.x0 + metrics.px(8.0f), row.y0, row.x1 - metrics.row(), row.y1},
                  "in " + playing_);
        const Rect leave{row.x1 - metrics.row(), row.y0, row.x1, row.y1};
        if (ui_.icon_button(id_of("library.leave"), leave, Icon::Up,
                            "Leave this world and go back to the title")) {
            verdict.leave_world = true;
            ui_.sound().say(Cue::Close);
        }
        top = row.y1;
    }

    const Rect tabs{rect.x0, top, rect.x1, top + metrics.row()};
    ui_.tabs(id_of("library.tabs"), tabs, kTabIcons, kTabLabels, 3, tab_);

    const Rect body{rect.x0, tabs.y1, rect.x1, rect.y1};
    if (tab_ == 0) draw_library_tab(body, verdict);
    else if (tab_ == 1) draw_community_tab(body);
    else draw_editor_tab(body);
}

void Shell::draw_library_tab(const Rect& rect, Verdict& verdict) {
    const Metrics& metrics = ui_.metrics();
    const f32 pad = metrics.px(6.0f);

    // --- what shelf ---------------------------------------------------------------------------
    //
    // One window, one shelf at a time, switched by a row of drawings. This is the many-worlds
    // manager and the clip library at once: the same window with a different shelf behind it.
    Column column;
    column.box = Rect{rect.x0 + pad, rect.y0 + pad, rect.x1 - pad, rect.y1 - pad};
    column.y = column.box.y0;
    column.row = metrics.row();
    column.gap = metrics.px(4.0f);

    {
        const std::vector<Kind>& kinds = shipped_kinds();
        std::vector<Icon> icons;
        std::vector<std::string_view> labels;
        std::vector<std::string_view> hints;
        for (const Kind& k : kinds) {
            icons.push_back(k.icon);
            labels.push_back({});
            hints.push_back(k.label);
        }
        u32 want = kind_;
        if (ui_.choice(id_of("library.kind"), column.next(), {}, icons.data(), labels.data(),
                       hints.data(), static_cast<u32>(kinds.size()), want)) {
            kind_ = want;
            clear_selection();
            library_.open(kinds[kind_]);
        }
    }

    // --- the toolbar --------------------------------------------------------------------------
    const std::vector<Entry> chosen = selection();
    {
        const Rect bar = column.next();
        const f32 each = bar.width() / 7.0f;
        const auto slot = [&](u32 i) {
            return Rect{bar.x0 + each * static_cast<f32>(i), bar.y0,
                        bar.x0 + each * static_cast<f32>(i + 1), bar.y1};
        };
        if (ui_.icon_button(id_of("library.up"), slot(0), Icon::Up, "Out of this folder")) {
            clear_selection();
            library_.up();
        }
        if (ui_.icon_button(id_of("library.newfolder"), slot(1), Icon::Folder, "A new folder")) {
            naming_folder_ = true;
            folder_buffer_ = "new folder";
        }
        if (ui_.icon_button(id_of("library.new"), slot(2), Icon::New,
                            "A new one of these, to edit")) {
            std::string name = "untitled";
            u32 nth = 2;
            while (!library_.create(name, preferences_.username).empty() && nth < 100) {
                name = "untitled " + std::to_string(nth);
                ++nth;
            }
            message_ = "made " + name;
            message_until_ = seconds_ + 2.0;
            ui_.sound().say(Cue::Commit);
        }
        if (ui_.icon_button(id_of("library.rename"), slot(3), Icon::Rename, "Rename")) {
            if (chosen.size() == 1) {
                renaming_ = chosen.front().name;
                rename_buffer_ = chosen.front().shown;
            } else {
                message_ = "rename works on exactly one thing";
                message_until_ = seconds_ + 2.0;
            }
        }
        if (ui_.icon_button(id_of("library.duplicate"), slot(4), Icon::Duplicate, "Duplicate")) {
            if (!chosen.empty()) {
                message_ = library_.duplicate(chosen);
                message_until_ = seconds_ + 2.5;
                ui_.sound().say(Cue::Commit);
            }
        }
        if (ui_.icon_button(id_of("library.delete"), slot(5), Icon::Delete,
                            "To the trash, where you can get it back")) {
            if (!chosen.empty()) {
                message_ = library_.erase(chosen);
                if (message_.empty()) message_ = "moved to the trash";
                message_until_ = seconds_ + 2.5;
                clear_selection();
                ui_.sound().say(Cue::Erase);
            }
        }
        {
            static constexpr std::string_view kSorts[4]{"name", "date", "author", "size"};
            u32 by = static_cast<u32>(library_.sort());
            const Rect cell = slot(6);
            if (ui_.icon_button(id_of("library.sort"), cell, Icon::Sort,
                                "Sort by name, date, author or size")) {
                by = (by + 1) % 4;
                library_.set_sort(static_cast<Sort>(by), library_.descending());
                message_ = std::string("sorted by ") + std::string(kSorts[by]);
                message_until_ = seconds_ + 1.5;
            }
        }
    }

    // --- where you are --------------------------------------------------------------------------
    {
        const Rect crumb = column.next(metrics.px(14.0f));
        ui_.label(crumb, library_.breadcrumb(), Align::Left, kPlain, 0.7f);
    }

    if (naming_folder_) {
        const Rect row = column.next();
        if (ui_.field(id_of("library.foldername"), row, folder_buffer_, "folder name",
                      "Enter makes it; escape leaves it unmade")) {
            const std::string why = library_.make_folder(folder_buffer_);
            if (!why.empty()) ui_.refuse(row, why);
            naming_folder_ = false;
        }
    }

    // --- the listing ---------------------------------------------------------------------------
    const std::vector<Entry>& entries = library_.entries();
    const Rect list{column.box.x0, column.y, column.box.x1, column.box.y1};
    const f32 row_height = metrics.row() + metrics.px(2.0f);
    const f32 content = static_cast<f32>(entries.size()) * row_height + metrics.px(8.0f);

    const Rect inner = ui_.begin_scroll(id_of("library.scroll"), list, content);
    const bool shift = ui_.input().is_down(Key::Shift);
    const bool ctrl = ui_.input().is_down(Key::Ctrl);

    // The rubber band, over the whole listing. Explorer's gesture on purpose: an interface that
    // spends its novelty budget on *selecting things* has spent it in the worst possible place.
    Rect box{};
    bool band_done = false;
    const bool banding = ui_.band(id_of("library.band"), list, box, band_done);
    // Recomputed from the box every frame rather than accumulated, so dragging the band back
    // over a row takes it out again. An accumulating band is one that can only ever select more.
    if (banding) chosen_.clear();
    bool hit_row = false;

    for (usize i = 0; i < entries.size(); ++i) {
        const Entry& entry = entries[i];
        const Rect row{inner.x0, inner.y0 + row_height * static_cast<f32>(i), inner.x1,
                       inner.y0 + row_height * static_cast<f32>(i + 1)};
        if (row.y1 < list.y0 || row.y0 > list.y1) continue;   // scrolled out of sight

        if (banding && box.y1 > row.y0 && box.y0 < row.y1) chosen_.push_back(entry.name);

        const bool is_selected = selected(entry);
        if (is_selected) ui_.draw().ink(row, 0.22f);

        if (renaming_ == entry.name) {
            if (ui_.field(id_of("library.rename.field"), row, rename_buffer_, entry.shown,
                          "Enter renames it")) {
                const std::string why = library_.rename(entry, rename_buffer_);
                if (!why.empty()) ui_.refuse(row, why);
                renaming_.clear();
                clear_selection();
            }
            continue;
        }

        const bool over = row.holds(ui_.pointer_x(), ui_.pointer_y()) &&
                          list.holds(ui_.pointer_x(), ui_.pointer_y());
        if (over && ui_.pressed_in(row)) {
            hit_row = true;
            if (ctrl) select_add(entry);
            else if (shift) select_range(entries, entry);
            else select_only(entry);
            ui_.sound().say(Cue::Step);

            // Open — the double-click, which means "use this": a world loads, a folder is
            // entered, anything else goes to the editor.
            if (ui_.input().click_count >= 2) {
                if (entry.folder) {
                    clear_selection();
                    library_.enter(entry);
                    ui_.sound().say(Cue::Open);
                    break;
                }
                if (shipped_kinds()[kind_].folder == "worlds") {
                    verdict.open_world = true;
                    verdict.world = entry.path;
                    ui_.sound().say(Cue::Open);
                } else {
                    open_document(entry.path);
                    tab_ = 2;
                    ui_.sound().say(Cue::Open);
                }
            }
        }
        if (over) ui_.draw().ink(row, 0.06f);

        const f32 cell = metrics.icon();
        ui_.draw().icon(Rect{row.x0 + metrics.px(4.0f), row.mid_y() - cell * 0.5f,
                             row.x0 + metrics.px(4.0f) + cell, row.mid_y() + cell * 0.5f},
                        entry.folder ? Icon::Folder : shipped_kinds()[kind_].icon);

        // Every file says who made it, for ever, in your library and in anyone's you pass it to.
        // A file with no tag says nothing rather than guessing that it was you.
        //
        // The aside is measured FIRST and the name is clipped to what is left, because the name is
        // the thing a player is reading and the two running into each other makes neither legible.
        const std::string aside =
            entry.folder ? std::string()
                         : (entry.author.empty() ? spell_bytes(entry.bytes)
                                                 : entry.author + "  " + spell_bytes(entry.bytes));
        const f32 right = row.x1 - metrics.px(6.0f);
        const f32 aside_width =
            aside.empty() ? 0.0f : DrawList::measure(aside, metrics.small_text()) + metrics.px(8.0f);

        const f32 text_x = row.x0 + metrics.px(8.0f) + cell;
        ui_.draw().push_clip(Rect{text_x, row.y0, right - aside_width, row.y1});
        ui_.label(Rect{text_x, row.y0, right, row.y1}, entry.shown);
        ui_.draw().pop_clip();

        if (!aside.empty()) {
            ui_.draw().text(right, row.mid_y() - DrawList::cap_height(metrics.small_text()) * 0.5f,
                            aside, metrics.small_text(), kPlain, Align::Right, 0.55f);
        }
    }

    if (band_done) ui_.sound().say(Cue::Step);
    ui_.end_scroll();

    // Clicking the empty space of a listing clears the selection, which is what a file manager
    // does and what a player will try first.
    if (ui_.pressed_in(list) && !hit_row && !ctrl && !shift) clear_selection();
}

void Shell::draw_community_tab(const Rect& rect) {
    const Metrics& metrics = ui_.metrics();
    const Rect body = rect.inset(metrics.px(10.0f));
    // A tab that is missing teaches a player it will never exist; a tab that says what it is
    // waiting for is a roadmap they can read without opening a document.
    ui_.markdown(body,
                 "### community\n"
                 "\n"
                 "This browses what every player who is **online right now** has in their own "
                 "library, over the same connections the game itself uses. There is no server "
                 "anywhere in it.\n"
                 "\n"
                 "It needs the multiplayer stage, which is where those connections are built.\n");
}

void Shell::draw_editor_tab(const Rect& rect) {
    const Metrics& metrics = ui_.metrics();
    const Rect body = rect.inset(metrics.px(8.0f));

    // The editor asks for a file first (D455). Not a dialog: it sends you to the library tab,
    // with *new* sitting where the cursor already is. There is no editing without something to
    // edit, and an editor that opens on an untitled nothing has to invent a place to put it.
    if (editing_.empty()) {
        ui_.markdown(body,
                     "### editor\n"
                     "\n"
                     "Open something first. The *library* tab is where things are, and *new* is "
                     "in its toolbar.\n"
                     "\n"
                     "The **script** view is here. The **visual** view — the same document as "
                     "nodes and wires, live — is the node editor's, and there is exactly one node "
                     "editor in this game so that you learn it once.\n");
        const Rect go{body.x0, body.y1 - metrics.row(), body.x0 + metrics.px(120.0f), body.y1};
        if (ui_.button(id_of("editor.go"), go, Icon::Library, "to the library",
                       "Choose something to edit")) {
            tab_ = 0;
        }
        return;
    }

    Column column;
    column.box = body;
    column.y = body.y0;
    column.row = metrics.row();
    column.gap = metrics.px(4.0f);

    {
        const Rect bar = column.next();
        const f32 each = bar.width() / 4.0f;
        const auto slot = [&](u32 i) {
            return Rect{bar.x0 + each * static_cast<f32>(i), bar.y0,
                        bar.x0 + each * static_cast<f32>(i + 1), bar.y1};
        };
        ui_.label(Rect{slot(0).x0, bar.y0, slot(2).x1, bar.y1},
                  editing_.filename().string() + (dirty_ ? " *" : ""));
        if (ui_.icon_button(id_of("editor.save"), slot(3), Icon::Tick, "Write it back to the file")) {
            save_document();
        }
    }

    // The two sub-tabs, which are two views of the SAME document. The visual one is Stage 20's
    // and says so rather than being absent (D456) — a view that is missing teaches a player it
    // will never exist.
    {
        static constexpr Icon kViews[2]{Icon::Editor, Icon::Mod};
        static constexpr std::string_view kViewLabels[2]{"script", "visual"};
        static constexpr std::string_view kViewHints[2]{
            "The document as text, in the language a clip is written in",
            "The same document as nodes and wires - the node editor's, in Stage 20"};
        const Rect views = column.next();
        u32 want = view_;
        if (ui_.choice(id_of("editor.view"), views, {}, kViews, kViewLabels, kViewHints, 2, want)) {
            if (want == 1) {
                // Said once, on the press, rather than every frame: a refusal that repeats sixty
                // times a second is a refusal nobody reads and a sound nobody forgives.
                ui_.refuse(views, "the visual view waits for the node editor, in Stage 20");
            } else {
                view_ = want;
            }
        }
    }

    // The parse, which happens on every keystroke and whose failure is not an error.
    const Rect status = column.next(metrics.px(16.0f));
    if (report_.ok) {
        ui_.label(status, "reads", Align::Left, kPlain, 0.55f);
    } else {
        ui_.label(status,
                  "line " + std::to_string(report_.line) + ": " + report_.message, Align::Left,
                  kPlain, 0.85f);
    }

    // --- the text ------------------------------------------------------------------------------
    const Rect page{column.box.x0, column.y, column.box.x1, column.box.y1};
    const f32 size = metrics.text();
    const f32 line_height = DrawList::line_height(size) + metrics.px(2.0f);
    const f32 content = static_cast<f32>(lines_.size() + 2) * line_height;
    const Rect inner = ui_.begin_scroll(id_of("editor.scroll"), page, content);

    const f32 gutter = DrawList::measure("0000", size, kMono) + metrics.px(6.0f);
    for (usize i = 0; i < lines_.size(); ++i) {
        const f32 y = inner.y0 + line_height * static_cast<f32>(i);
        if (y + line_height < page.y0 || y > page.y1) continue;
        // The line number, quieter than the code, because it is not part of the document.
        ui_.draw().text(inner.x0 + gutter - metrics.px(6.0f), y, std::to_string(i + 1), size,
                        kMono, Align::Right, 0.35f);
        if (!report_.ok && report_.line == static_cast<u32>(i + 1)) {
            ui_.draw().ink(Rect{inner.x0, y, inner.x1, y + line_height}, 0.14f);
        }
        ui_.draw().text(inner.x0 + gutter, y, lines_[i], size, kMono);

        if (caret_line_ == i && ui_.wants_keys()) {
            const std::string_view before(lines_[i].c_str(),
                                          std::min<usize>(caret_column_, lines_[i].size()));
            const f32 at = inner.x0 + gutter + DrawList::measure(before, size, kMono);
            if (std::fmod(seconds_, 1.0) < 0.5) {
                ui_.draw().ink(Rect{at, y, at + std::max(1.0f, metrics.scale), y + line_height},
                               1.0f);
            }
        }
    }

    // Clicking puts the caret where you clicked, which is the one thing a text view has to do
    // before it is a text view at all.
    if (ui_.pressed_in(page)) {
        const f32 local_y = ui_.pointer_y() - inner.y0;
        caret_line_ = static_cast<u32>(
            std::clamp(std::floor(local_y / line_height), 0.0f,
                       static_cast<f32>(lines_.empty() ? 0 : lines_.size() - 1)));
        const std::string& line = lines_.empty() ? rename_buffer_ : lines_[caret_line_];
        const f32 local_x = ui_.pointer_x() - inner.x0 - gutter;
        usize column_at = 0;
        while (column_at < line.size() &&
               DrawList::measure(std::string_view(line).substr(0, column_at + 1), size, kMono) <
                   local_x) {
            ++column_at;
        }
        caret_column_ = static_cast<u32>(column_at);
        ui_.stop_typing();
    }
    ui_.end_scroll();

    // The script view eats characters, so the platform has to be composing them — and it takes
    // them only when nothing else has the keyboard, because a rename in the library tab and a
    // caret in the editor would otherwise both receive every letter typed.
    ui_.request_text_input();
    if (!ui_.wants_keys()) edit_keys(ui_.input());
}

std::string Shell::document() const {
    std::string text;
    for (usize i = 0; i < lines_.size(); ++i) {
        text += lines_[i];
        if (i + 1 < lines_.size()) text += "\n";
    }
    return text;
}

void Shell::open_document(const std::filesystem::path& path) {
    editing_ = path;
    lines_.clear();
    std::ifstream file(path, std::ios::binary);
    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines_.push_back(line);
    }
    if (lines_.empty()) lines_.push_back({});
    caret_line_ = 0;
    caret_column_ = 0;
    dirty_ = false;
    if (parse_) report_ = parse_(document());
}

void Shell::save_document() {
    if (editing_.empty()) return;
    std::ofstream file(editing_, std::ios::binary | std::ios::trunc);
    if (!file) {
        message_ = "could not write that file";
        message_until_ = seconds_ + 2.5;
        return;
    }
    const std::string text = document();
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    dirty_ = false;
    ui_.sound().say(Cue::Commit);
}

void Shell::edit_keys(const InputState& input) {
    if (editing_.empty() || lines_.empty()) return;
    // The editor takes the keyboard only when its own view is the one showing, which is the check
    // the caller has already made by drawing it.
    std::string& line = lines_[std::min<usize>(caret_line_, lines_.size() - 1)];
    caret_column_ = static_cast<u32>(std::min<usize>(caret_column_, line.size()));

    bool touched = false;
    if (!input.typed.empty()) {
        line.insert(caret_column_, input.typed);
        caret_column_ += static_cast<u32>(input.typed.size());
        touched = true;
    }
    if (input.fired(Key::Enter)) {
        const std::string tail = line.substr(caret_column_);
        line.erase(caret_column_);
        lines_.insert(lines_.begin() + static_cast<isize>(caret_line_) + 1, tail);
        ++caret_line_;
        caret_column_ = 0;
        touched = true;
    } else if (input.fired(Key::Backspace)) {
        if (caret_column_ > 0) {
            usize back = caret_column_ - 1;
            while (back > 0 && (static_cast<u8>(line[back]) & 0xC0u) == 0x80u) --back;
            line.erase(back, caret_column_ - back);
            caret_column_ = static_cast<u32>(back);
            touched = true;
        } else if (caret_line_ > 0) {
            const usize joined = lines_[caret_line_ - 1].size();
            lines_[caret_line_ - 1] += line;
            lines_.erase(lines_.begin() + static_cast<isize>(caret_line_));
            --caret_line_;
            caret_column_ = static_cast<u32>(joined);
            touched = true;
        }
    } else if (input.fired(Key::Delete)) {
        if (caret_column_ < line.size()) {
            line.erase(caret_column_, 1);
            touched = true;
        } else if (caret_line_ + 1 < lines_.size()) {
            line += lines_[caret_line_ + 1];
            lines_.erase(lines_.begin() + static_cast<isize>(caret_line_) + 1);
            touched = true;
        }
    }

    if (input.fired(Key::Left) && caret_column_ > 0) --caret_column_;
    if (input.fired(Key::Right) && caret_column_ < line.size()) ++caret_column_;
    if (input.fired(Key::Up) && caret_line_ > 0) --caret_line_;
    if (input.fired(Key::Down) && caret_line_ + 1 < lines_.size()) ++caret_line_;
    if (input.was_pressed(Key::Home)) caret_column_ = 0;
    if (input.was_pressed(Key::End)) {
        caret_column_ = static_cast<u32>(lines_[std::min<usize>(caret_line_, lines_.size() - 1)].size());
    }

    if (touched) {
        dirty_ = true;
        // Re-parsed on every keystroke, and a script that does not parse is NOT an error (D453):
        // the status line says where it stopped and nothing pops up, because a parser that
        // interrupts you halfway through typing a word is a parser you fight.
        if (parse_) report_ = parse_(document());
    }
}

}  // namespace ws::ui
