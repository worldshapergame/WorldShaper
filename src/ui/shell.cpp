#include "ui/shell.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>

#include "core/log.hpp"
#include "core/version.hpp"
#include "platform/desktop.hpp"

namespace ws::ui {
namespace {

// A column of rows down a docked window. Every panel in this interface is one of these, which is
// why it is nine lines rather than a layout engine.
struct Column {
    Rect box;
    f32 y = 0.0f;
    f32 row = 0.0f;
    f32 gap = 0.0f;
    // How far in the rows under an open section sit, so that a section inside a section reads as
    // one. Set by the caller around a fold's contents rather than tracked here, because a column
    // that knew about folds would be two things.
    f32 indent = 0.0f;

    Rect next(f32 height = 0.0f) {
        const f32 tall = (height > 0.0f) ? height : row;
        const Rect out{box.x0 + indent, y, box.x1, y + tall};
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
// In the order Sort declares them, because the button steps through it by index.
constexpr std::string_view kSortNames[4]{"name", "date", "author", "size"};

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

void Shell::load(const std::filesystem::path& root, const std::filesystem::path& game) {
    root_ = root;
    library_ = Library(root);
    library_.ensure_folders();
    library_.set_shipped_root(game);
    library_.open(shipped_kinds()[kind_]);
    // One file, and it is the only one (D496).
    //
    // There were two — `shell.txt` for the preferences and `layout.txt` for where the windows sit
    // — plus the renderer's own `settings.txt` beside them, so "my settings" was three files a
    // player had to know the names of and a reset had to remember all of. They are one file now,
    // and the old two are read once and swept up so nothing anybody had chosen is lost.
    preferences_.read(root / "settings.txt");
    dock_.read(root / "settings.txt");
    adopt_old_settings_files();
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

        // Its fragments are NOT copied, and neither is the world already built from it (D493,
        // D494).
        //
        // Both used to be, and both were mistakes of the same shape: things that are not the
        // player's work, sitting in the folder the player's work is in, on a shelf that shows
        // everything in that folder. The pieces looked like an ordinary folder and were deleted —
        // three times — leaving a world that opened as an empty sky; the `.world` was nineteen
        // megabytes of derived data next to a five-kilobyte file. Includes now fall back to the
        // clips the game ships with, so the pieces are found where they already are, and a built
        // world lives under `cache\` keyed by the world's path.

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
    // Both halves into one file, preferences first and the layout after, because both readers skip
    // keys they do not know and neither cares what else is in there.
    preferences_.write(root_ / "settings.txt");
    dock_.append(root_ / "settings.txt");
    saved_ = snapshot();
}

// What is in the settings, as one string, so that "has anything changed" is a comparison rather
// than a flag every control has to remember to set. It is a few hundred bytes built once a second;
// the alternative is a `dirty` somebody forgets to set in the one place it mattered.
std::string Shell::snapshot() const {
    std::string out;
    out += preferences_.username;
    out += ";";
    for (u32 i = 0; i < 3; ++i) out += std::to_string(preferences_.accent[i]) + ",";
    out += std::to_string(preferences_.logo_rgb) + ";";
    out += (preferences_.sound ? "1" : "0");
    out += std::to_string(preferences_.volume) + ";";
    out += std::to_string(preferences_.interface_scale) + ";";
    out += dock_.state();
    return out;
}

// Written whenever it has actually changed, rather than only on the way out.
//
// A settings file that is only written when the game closes is a settings file that a crash, a
// driver reset or a task-manager kill throws away — and this game has a crash handler precisely
// because those happen. Compared rather than flagged, and at most once a second, so dragging a
// slider is one write when the hand comes off rather than sixty a second while it moves.
void Shell::save_if_changed() {
    if (root_.empty() || seconds_ < save_check_at_) return;
    save_check_at_ = seconds_ + 1.0;
    if (snapshot() == saved_) return;
    save();
}

// The two files this replaced, read once and taken away.
void Shell::adopt_old_settings_files() {
    std::error_code error;
    const std::filesystem::path shell_txt = root_ / "shell.txt";
    const std::filesystem::path layout_txt = root_ / "layout.txt";
    bool found = false;
    if (std::filesystem::exists(shell_txt, error) && !error) {
        preferences_.read(shell_txt);
        found = true;
    }
    error.clear();
    if (std::filesystem::exists(layout_txt, error) && !error) {
        dock_.read(layout_txt);
        found = true;
    }
    error.clear();
    if (!found) return;
    save();
    std::filesystem::remove(shell_txt, error);
    error.clear();
    std::filesystem::remove(layout_txt, error);
    error.clear();
    WS_LOG_INFO("shell", "settings moved into one file");
}

void Shell::open_settings() {
    // Folded away every time it opens, not only the first. What a settings panel opens as is its
    // answer to *what can I change here*, and five words is that answer where four screens of rows
    // is the question asked again in more detail.
    if (!dock_.is_open(window_settings_)) ui_.close_all_sections();
    dock_.set_open(window_settings_, true);
}

void Shell::open_windows() {
    // BOTH of them. The two families are two halves of one answer to "what can I change here" —
    // parameters on the left, the library on the right — and opening one of them was the interface
    // deciding which half the player meant. It cannot know, and the side is what carries the
    // meaning (D443), so it opens both and lets the eye go where it already knows to go.
    if (windows_open()) return;
    open_settings();
    dock_.set_open(window_worlds_, true);
    ui_.sound().say(Cue::Open);
}

void Shell::shut_windows() {
    dock_.set_open(window_settings_, false);
    dock_.set_open(window_worlds_, false);
    // A field that was being typed into has gone with the window it was in, and a caret that
    // outlives its own row keeps the keyboard for ever — which under D477 means the game stays
    // deaf to every key with nothing on screen to explain why.
    ui_.stop_typing();
    ui_.close_menu();
}

void Shell::close_windows() {
    if (!windows_open()) return;
    shut_windows();
    ui_.sound().say(Cue::Close);
}

void Shell::toggle_windows() {
    if (windows_open()) {
        close_windows();
    } else {
        open_windows();
    }
}

void Shell::open_window(std::string_view which, bool open) {
    if (which == "worlds" || which == "both") dock_.set_open(window_worlds_, open);
    if (which == "settings" || which == "both") {
        if (open) {
            open_settings();
        } else {
            dock_.set_open(window_settings_, false);
        }
    }
}

bool Shell::open_shelf(std::string_view folder) {
    const std::vector<Kind>& kinds = shipped_kinds();
    for (u32 i = 0; i < static_cast<u32>(kinds.size()); ++i) {
        if (kinds[i].folder != folder) continue;
        kind_ = i;
        clear_selection();
        library_.open(kinds[i]);
        return true;
    }
    return false;
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

    // Only while a library is being LOOKED at.
    //
    // This ran every frame, in the world as well as on the title, with no window open and nothing
    // to draw from it — one `last_write_time` on `%LOCALAPPDATA%\WorldShaper\worlds` per frame,
    // for ever. A stat is cheap and a stat several hundred times a second on a directory a virus
    // scanner is watching is not: it is a synchronous call into the file system on the render
    // thread, and what it costs is not a cost this process controls or can predict. A shelf that
    // nothing is showing does not need re-reading, and the frames it was spent on were every frame
    // of playing.
    if (dock_.is_open(window_worlds_)) library_.refresh();

    // The windows are laid out BEFORE the title is drawn, and their rectangles are claimed.
    //
    // An immediate-mode interface hit-tests in the order it draws, so anything drawn first answers
    // the pointer first — and the title's two buttons are drawn under the docked windows. Pressing
    // a row of the settings panel that happened to lie over *worlds* pressed *worlds* as well:
    // reported as *you can click the menu buttons of settings or worlds through the library windows
    // or settings window*. The layout has to happen first for the claim to know where the windows
    // are; the claims are dropped again below, so the windows' own controls are not blocked by the
    // claim their own window made.
    dock_.layout(ui_, ui_.screen());
    if (dock_.is_open(window_settings_)) ui_.claim(dock_.rect(window_settings_));
    if (dock_.is_open(window_worlds_)) ui_.claim(dock_.rect(window_worlds_));

    if (stage_ == Stage::Title) {
        if (icons_) {
            draw_icon_sheet();
        } else {
            draw_title(verdict);
        }
    }

    // Before the windows, so the windows are over it. That is the whole of what makes it behave
    // like part of the world rather than like a sticker on the glass.
    draw_overlay();

    ui_.clear_claims();
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
        open_settings();
        ui_.sound().say(Cue::Open);
    }

    // What is running, small, in the corner where a version number belongs. Not furniture: it is
    // the first thing anybody is asked for when something goes wrong.
    ui_.draw().text(screen.x1 - metrics.px(10.0f), screen.y1 - metrics.px(18.0f),
                    std::string("v") + kVersion, metrics.text(), kPlain, Align::Right, 0.45f);
    (void)verdict;
}

// What the frame is costing, in the corner, drawn BEFORE the windows so a window covers it.
//
// It was an ImGui window and it was drawn over everything, because ImGui is rendered onto the
// swapchain after the interface has already been composited. That is backwards: this is a readout
// about the world, and a docked window is in front of the world. Here it is a mark like any other
// — lettered in the game's own face, inverting against whatever is behind it, and under the glass
// of anything opened over it.
void Shell::draw_overlay() {
    if (!overlay_.on) return;
    const Metrics& metrics = ui_.metrics();
    const f32 size = metrics.text();
    const f32 x = metrics.px(10.0f);
    f32 y = metrics.px(10.0f);
    const f32 step = DrawList::line_height(size) + metrics.px(2.0f);

    char line[96];
    std::snprintf(line, sizeof(line), "%.0f fps   %.2f ms", overlay_.fps, overlay_.frame_ms);
    ui_.draw().text(x, y, line, size);
    y += step;
    std::snprintf(line, sizeof(line), "worst %.2f   gpu %.2f ms", overlay_.worst_ms,
                  overlay_.gpu_ms);
    ui_.draw().text(x, y, line, size);
    y += step;
    std::snprintf(line, sizeof(line), "%ux%u", overlay_.width, overlay_.height);
    ui_.draw().text(x, y, line, size);
}

// Every drawing in the vocabulary, at four sizes and across five steps of its own animation.
//
// This is an instrument and not a screen: `--icon-sheet --title-shot FILE` is the whole of its
// interface, and nothing in the game reaches it. It exists because an icon is only ever on screen
// where a window happens to put it, so most of these were drawn by nothing an automated run ever
// looked at — and the size that matters most, sixteen device pixels, is the size a small desktop
// sets every row at and the size nobody ever checked them at. Whether a drawing survives being that
// small is not a thing an argument settles.
void Shell::draw_icon_sheet() {
    // In the order src/ui/draw.hpp declares them, because a sheet that reorders the list is a sheet
    // you cannot check the list against.
    static constexpr std::string_view kNames[]{
        "world",  "settings", "library", "community", "editor",    "folder",
        "up",     "new",      "rename",  "duplicate", "delete",    "trash",
        "sort",   "search",   "private", "shared",    "close",     "clip",
        "material", "mod",    "character", "script",  "tick",      "play",
        "reset",  "collapsed", "expanded",
    };
    constexpr u32 kCount = static_cast<u32>(std::size(kNames));
    static_assert(kCount + 1 == static_cast<u32>(Icon::Count), "one name per drawing");

    const Rect screen = ui_.screen();
    const Metrics& metrics = ui_.metrics();
    ui_.draw().glass(screen, 0.9f);

    constexpr u32 kColumns = 6;
    const u32 rows = (kCount + kColumns - 1) / kColumns;
    const f32 cell_w = screen.width() / static_cast<f32>(kColumns);
    const f32 cell_h = screen.height() / static_cast<f32>(rows);

    // The four sizes, in DEVICE pixels and deliberately not in interface pixels: the question this
    // sheet answers is "does it survive sixteen pixels", and sixteen interface pixels is a
    // different number on every window.
    const f32 sizes[4]{16.0f, 24.0f, 36.0f, 56.0f};
    // And the animation, sampled where it is worth looking: at rest, at the press, and the three
    // steps between — including the one just past rest, which is the counter-swing.
    const f32 phases[5]{1.0f, 0.72f, 0.42f, 0.12f, 0.0f};

    for (u32 i = 0; i < kCount; ++i) {
        const Icon icon = static_cast<Icon>(i + 1);
        const f32 x0 = cell_w * static_cast<f32>(i % kColumns) + metrics.px(8.0f);
        const f32 y0 = cell_h * static_cast<f32>(i / kColumns) + metrics.px(8.0f);

        // The sizes, on one baseline so that four drawings of one shape can be compared.
        f32 x = x0;
        const f32 baseline = y0 + sizes[3];
        for (f32 size : sizes) {
            ui_.draw().icon(Rect{x, baseline - size, x + size, baseline}, icon);
            x += size + metrics.px(4.0f);
        }

        // The animation, at one size, left to right in the order it runs.
        x = x0;
        const f32 strip = y0 + sizes[3] + metrics.px(6.0f);
        for (f32 phase : phases) {
            ui_.draw().icon(Rect{x, strip, x + 30.0f, strip + 30.0f}, icon, 1.0f, phase);
            x += 30.0f + metrics.px(3.0f);
        }

        ui_.draw().text(x0, strip + 30.0f + metrics.px(5.0f), kNames[i], metrics.text());
    }
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

    // What the game shipped as. Both structs carry their defaults as in-class initialisers, so
    // this is *the* definition of default rather than a second copy of it that can drift.
    static const Preferences kFresh{};
    static const Knobs kFreshKnobs{};

    // Measured last frame rather than counted.
    //
    // It used to be arithmetic — so many rows, so many headings — which worked while every row was
    // always drawn and stopped the moment a section could be folded away. Counting it correctly now
    // would mean walking the whole structure twice, once to measure and once to draw, and two walks
    // of one list is one place for them to disagree. A frame-old height is only ever the scrollbar's
    // extent, and the frame it is wrong on is the frame a fold was opened.
    Column column;
    column.box = ui_.begin_scroll(id_of("settings.scroll"), body, settings_height_);
    column.y = column.box.y0;
    column.row = metrics.row();
    column.gap = metrics.px(4.0f);
    const f32 started_at = column.y;

    // Every row gives up its right-hand end to the gutter the reset lives in, whether or not this
    // row has one — a column that moved when a value was changed would be a column that jumps as
    // you use it. `field` is the width the control gets; `gutter` is where the button goes.
    const auto field_of = [&](const Rect& row) {
        return Rect{row.x0, row.y0, row.x1 - metrics.row(), row.y1};
    };
    const auto gutter_of = [&](const Rect& row) {
        return Rect{row.x1 - metrics.row(), row.y0, row.x1, row.y1};
    };

    const auto same = [](f32 a, f32 b) { return std::abs(a - b) < 1e-4f; };
    const auto same64 = [](f64 a, f64 b) { return std::abs(a - b) < 1e-6; };

    // What is off its default, worked out BEFORE anything is drawn, because a heading has to say
    // whether there is a changed value under it while that value is folded out of sight. That is
    // the whole reason the reset is on the heading as well as on the row: otherwise finding what
    // you changed means opening every fold and looking.
    const bool colour_off = !same(preferences_.accent[0], kFresh.accent[0]) ||
                            !same(preferences_.accent[1], kFresh.accent[1]) ||
                            !same(preferences_.accent[2], kFresh.accent[2]);
    const bool you_off = preferences_.username != kFresh.username;
    const bool interface_off = colour_off ||
                               !same(preferences_.interface_scale, kFresh.interface_scale) ||
                               knobs_.overlay != kFreshKnobs.overlay;
    const bool sound_off = preferences_.sound != kFresh.sound ||
                           !same(preferences_.volume, kFresh.volume);
    const bool resolution_off = !same64(knobs_.render_scale, kFreshKnobs.render_scale) ||
                                knobs_.vsync != kFreshKnobs.vsync;
    const bool camera_off = !same64(knobs_.field_of_view, kFreshKnobs.field_of_view) ||
                            knobs_.motion_blur != kFreshKnobs.motion_blur;
    const bool picture_off = resolution_off || camera_off ||
                             knobs_.auto_quality != kFreshKnobs.auto_quality ||
                             !same64(knobs_.target_fps, kFreshKnobs.target_fps) ||
                             (!knobs_.auto_quality &&
                              !same64(knobs_.quality_level, kFreshKnobs.quality_level));

    // A heading, with its own gutter and its own indent.
    const auto heading = [&](const char* key, std::string_view name, bool off, u32 depth) {
        column.skip(metrics.px(depth == 0 ? 10.0f : 4.0f));
        const f32 was = column.indent;
        column.indent = metrics.px(10.0f) * static_cast<f32>(depth);
        const Ui::Fold fold = ui_.section(id_of(key), column.next(metrics.px(17.0f)), name, off,
                                          0);
        column.indent = was;
        return fold;
    };

    // --- you ------------------------------------------------------------------------------
    {
        const Ui::Fold you = heading("settings.you", "you", you_off, 0);
        if (you.reset) preferences_.username = kFresh.username;
        if (you.open) {
            {
                const Rect row = column.next();
                const Rect line = field_of(row);
                const f32 label_room = line.width() * 0.36f;
                ui_.label(Rect{line.x0 + metrics.px(6.0f), line.y0, line.x0 + label_room, line.y1},
                          "name");
                std::string name = preferences_.username;
                if (ui_.field(id_of("settings.username"),
                              Rect{line.x0 + label_room, line.y0, line.x1, line.y1}, name,
                              "who made this",
                              "The name every file you make is stamped with, for ever")) {
                    if (!name.empty()) preferences_.username = name;
                }
                if (ui_.reset_button(id_of("settings.username.reset"), gutter_of(row),
                                     preferences_.username != kFresh.username,
                                     "Back to how it shipped")) {
                    preferences_.username = kFresh.username;
                }
            }
        }
    }

    // --- interface -------------------------------------------------------------------------
    {
        const Ui::Fold face = heading("settings.interface", "interface", interface_off, 0);
        if (face.reset) {
            for (u32 i = 0; i < 3; ++i) preferences_.accent[i] = kFresh.accent[i];
            preferences_.interface_scale = kFresh.interface_scale;
            knobs_.overlay = kFreshKnobs.overlay;
            knobs_.changed = true;
        }
        if (face.open) {
            {
                Number about;
                about.label = "interface size";
                about.tooltip = "How large everything is. Nought works it out from the window";
                about.low = 0.0;
                about.high = 6.0;
                about.step = 1.0;
                about.decimals = 0;
                const Rect row = column.next();
                f64 value = preferences_.interface_scale;
                if (ui_.number(id_of("settings.scale"), field_of(row), about, value)) {
                    preferences_.interface_scale = std::clamp(static_cast<f32>(value), 0.0f, 8.0f);
                }
                if (ui_.reset_button(id_of("settings.scale.reset"), gutter_of(row),
                                     !same(preferences_.interface_scale, kFresh.interface_scale),
                                     "Back to how it shipped")) {
                    preferences_.interface_scale = kFresh.interface_scale;
                }
            }
            {
                const Rect row = column.next();
                if (ui_.toggle(id_of("settings.overlay"), field_of(row), "performance overlay",
                               knobs_.overlay, "A small readout of what each frame is costing")) {
                    knobs_.changed = true;
                }
                if (ui_.reset_button(id_of("settings.overlay.reset"), gutter_of(row),
                                     knobs_.overlay != kFreshKnobs.overlay,
                                     "Back to how it shipped")) {
                    knobs_.overlay = kFreshKnobs.overlay;
                    knobs_.changed = true;
                }
            }

            const Ui::Fold colour = heading("settings.colour", "colour", colour_off, 1);
            if (colour.reset) {
                for (u32 i = 0; i < 3; ++i) preferences_.accent[i] = kFresh.accent[i];
            }
            if (colour.open) {
                const f32 was = column.indent;
                column.indent = metrics.px(10.0f);
                // Every numeric value is a slider, and a colour is three of them. Not a colour
                // wheel: a wheel is a control nobody can type into, and a typed value is D444.
                static constexpr std::string_view kChannels[3]{"red", "green", "blue"};
                for (u32 i = 0; i < 3; ++i) {
                    Number about;
                    about.label = kChannels[i];
                    about.tooltip =
                        "Your own colour, used only where inverting has nothing to say";
                    about.low = 0.0;
                    about.high = 1.0;
                    about.decimals = 2;
                    const Rect row = column.next();
                    f64 value = preferences_.accent[i];
                    if (ui_.number(id_of("settings.accent", i), field_of(row), about, value)) {
                        preferences_.accent[i] = std::clamp(static_cast<f32>(value), 0.0f, 1.0f);
                    }
                    if (ui_.reset_button(id_of("settings.accent.reset", i), gutter_of(row),
                                         !same(preferences_.accent[i], kFresh.accent[i]),
                                         "Back to how it shipped")) {
                        preferences_.accent[i] = kFresh.accent[i];
                    }
                }
                const Rect swatch = field_of(column.next(metrics.px(10.0f)));
                ui_.draw().hue(swatch.inset(metrics.px(2.0f)),
                               (static_cast<u32>(preferences_.accent[0] * 255.0f) << 16) |
                                   (static_cast<u32>(preferences_.accent[1] * 255.0f) << 8) |
                                   static_cast<u32>(preferences_.accent[2] * 255.0f));
                column.indent = was;
            }
        }
    }

    // --- sound -----------------------------------------------------------------------------
    {
        const Ui::Fold sound = heading("settings.sound", "sound", sound_off, 0);
        if (sound.reset) {
            preferences_.sound = kFresh.sound;
            preferences_.volume = kFresh.volume;
        }
        if (sound.open) {
            {
                const Rect row = column.next();
                ui_.toggle(id_of("settings.sound"), field_of(row), "sound", preferences_.sound,
                           "Off is exactly silent, not merely quiet");
                if (ui_.reset_button(id_of("settings.sound.reset"), gutter_of(row),
                                     preferences_.sound != kFresh.sound,
                                     "Back to how it shipped")) {
                    preferences_.sound = kFresh.sound;
                }
            }
            {
                Number about;
                about.label = "volume";
                about.tooltip = "How loud the interface is";
                about.low = 0.0;
                about.high = 1.0;
                about.decimals = 2;
                const Rect row = column.next();
                f64 value = preferences_.volume;
                if (ui_.number(id_of("settings.volume"), field_of(row), about, value)) {
                    preferences_.volume = std::clamp(static_cast<f32>(value), 0.0f, 1.0f);
                }
                if (ui_.reset_button(id_of("settings.volume.reset"), gutter_of(row),
                                     !same(preferences_.volume, kFresh.volume),
                                     "Back to how it shipped")) {
                    preferences_.volume = kFresh.volume;
                }
            }
        }
    }

    // --- picture ----------------------------------------------------------------------------
    {
        const Ui::Fold picture = heading("settings.picture", "picture", picture_off, 0);
        if (picture.reset) {
            knobs_.auto_quality = kFreshKnobs.auto_quality;
            knobs_.target_fps = kFreshKnobs.target_fps;
            knobs_.quality_level = kFreshKnobs.quality_level;
            knobs_.render_scale = kFreshKnobs.render_scale;
            knobs_.field_of_view = kFreshKnobs.field_of_view;
            knobs_.vsync = kFreshKnobs.vsync;
            knobs_.motion_blur = kFreshKnobs.motion_blur;
            knobs_.changed = true;
        }
        if (picture.open) {
            {
                const Rect row = column.next();
                if (ui_.toggle(id_of("settings.auto"), field_of(row), "hold the frame rate",
                               knobs_.auto_quality,
                               "Spend detail to keep the frame rate, measured on this machine")) {
                    knobs_.changed = true;
                }
                if (ui_.reset_button(id_of("settings.auto.reset"), gutter_of(row),
                                     knobs_.auto_quality != kFreshKnobs.auto_quality,
                                     "Back to how it shipped")) {
                    knobs_.auto_quality = kFreshKnobs.auto_quality;
                    knobs_.changed = true;
                }
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
                const Rect row = column.next();
                if (ui_.number(id_of("settings.fps"), field_of(row), about, knobs_.target_fps)) {
                    knobs_.changed = true;
                }
                if (ui_.reset_button(id_of("settings.fps.reset"), gutter_of(row),
                                     !same64(knobs_.target_fps, kFreshKnobs.target_fps),
                                     "Back to how it shipped")) {
                    knobs_.target_fps = kFreshKnobs.target_fps;
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
                const Rect row = column.next();
                if (ui_.number(id_of("settings.quality"), field_of(row), about,
                               knobs_.quality_level)) {
                    knobs_.auto_quality = false;
                    knobs_.changed = true;
                }
                // No reset of its own while the controller owns it: the number in that row is
                // what the machine decided this second, and a button offering to put it back is
                // a button offering to undo a measurement.
                if (ui_.reset_button(id_of("settings.quality.reset"), gutter_of(row),
                                     !knobs_.auto_quality &&
                                         !same64(knobs_.quality_level, kFreshKnobs.quality_level),
                                     "Back to how it shipped")) {
                    knobs_.quality_level = kFreshKnobs.quality_level;
                    knobs_.changed = true;
                }
            }

            const Ui::Fold resolution =
                heading("settings.resolution", "resolution", resolution_off, 1);
            if (resolution.reset) {
                knobs_.render_scale = kFreshKnobs.render_scale;
                knobs_.vsync = kFreshKnobs.vsync;
                knobs_.changed = true;
            }
            if (resolution.open) {
                const f32 was = column.indent;
                column.indent = metrics.px(10.0f);
                {
                    Number about;
                    about.label = "render scale";
                    about.tooltip =
                        "What fraction of the window the world is drawn at before being scaled up";
                    about.low = 0.25;
                    about.high = 1.0;
                    about.decimals = 2;
                    // The named example from D444: a value that would break the game is refused,
                    // and the refusal says in one line what it would have done.
                    about.guard = [](f64 value) -> std::string {
                        if (value <= 0.0) return "a render scale of nought would draw no pixels at all";
                        if (value > 4.0) {
                            return "over four times the window is more pixels than any card has";
                        }
                        return {};
                    };
                    const Rect row = column.next();
                    if (ui_.number(id_of("settings.render_scale"), field_of(row), about,
                                   knobs_.render_scale)) {
                        knobs_.changed = true;
                    }
                    if (ui_.reset_button(id_of("settings.render_scale.reset"), gutter_of(row),
                                         !same64(knobs_.render_scale, kFreshKnobs.render_scale),
                                         "Back to how it shipped")) {
                        knobs_.render_scale = kFreshKnobs.render_scale;
                        knobs_.changed = true;
                    }
                }
                {
                    const Rect row = column.next();
                    if (ui_.toggle(id_of("settings.vsync"), field_of(row), "wait for the display",
                                   knobs_.vsync,
                                   "Draw in step with the monitor, so a frame is never torn in half")) {
                        knobs_.changed = true;
                    }
                    if (ui_.reset_button(id_of("settings.vsync.reset"), gutter_of(row),
                                         knobs_.vsync != kFreshKnobs.vsync,
                                         "Back to how it shipped")) {
                        knobs_.vsync = kFreshKnobs.vsync;
                        knobs_.changed = true;
                    }
                }
                column.indent = was;
            }

            const Ui::Fold camera = heading("settings.camera", "camera", camera_off, 1);
            if (camera.reset) {
                knobs_.field_of_view = kFreshKnobs.field_of_view;
                knobs_.motion_blur = kFreshKnobs.motion_blur;
                knobs_.changed = true;
            }
            if (camera.open) {
                const f32 was = column.indent;
                column.indent = metrics.px(10.0f);
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
                    const Rect row = column.next();
                    if (ui_.number(id_of("settings.fov"), field_of(row), about,
                                   knobs_.field_of_view)) {
                        knobs_.changed = true;
                    }
                    if (ui_.reset_button(id_of("settings.fov.reset"), gutter_of(row),
                                         !same64(knobs_.field_of_view, kFreshKnobs.field_of_view),
                                         "Back to how it shipped")) {
                        knobs_.field_of_view = kFreshKnobs.field_of_view;
                        knobs_.changed = true;
                    }
                }
                {
                    const Rect row = column.next();
                    if (ui_.toggle(id_of("settings.blur"), field_of(row), "motion blur",
                                   knobs_.motion_blur,
                                   "Smear fast movement the way a camera does")) {
                        knobs_.changed = true;
                    }
                    if (ui_.reset_button(id_of("settings.blur.reset"), gutter_of(row),
                                         knobs_.motion_blur != kFreshKnobs.motion_blur,
                                         "Back to how it shipped")) {
                        knobs_.motion_blur = kFreshKnobs.motion_blur;
                        knobs_.changed = true;
                    }
                }
                column.indent = was;
            }
        }
    }

    // --- data --------------------------------------------------------------------------------
    //
    // Where everything a player owns actually is, and the one button that takes it all away. Both
    // belong in the interface rather than in a document: a folder somebody has to be told the path
    // of is a folder they cannot find, and a reset somebody has to do by deleting files by hand is
    // a reset they will do wrong.
    {
        const Ui::Fold data = heading("settings.data", "data", false, 0);
        if (data.open) {
            {
                const Rect row = column.next();
                if (ui_.button(id_of("settings.folder"), field_of(row), Icon::Folder,
                               "open my files",
                               "Your worlds, clips, mods and settings, in a window")) {
                    if (open_in_file_manager(root_)) {
                        say("opened " + root_.string(), 4.0);
                    } else {
                        say("could not open " + root_.string(), 4.0);
                    }
                }
            }
            {
                // Two presses, and the second one is the decision.
                //
                // This deletes every world a player has made. A confirmation that is a second
                // dialog is a dialog people learn to dismiss; a button that changes into a
                // different button, says exactly what it is about to destroy, and goes back on its
                // own after a few seconds is one that cannot be pressed by accident and cannot be
                // pressed by habit either.
                const Rect row = column.next();
                const bool armed = wipe_armed_until_ > seconds_;
                const std::string label =
                    armed ? "press again: this deletes everything" : "reset everything";
                if (ui_.button(id_of("settings.wipe"), field_of(row), Icon::Delete, label,
                               "Every setting back to how it shipped, and every world, clip and "
                               "mod you have made to the recycle bin")) {
                    if (armed) {
                        wipe_armed_until_ = 0.0;
                        say(wipe_everything(), 8.0);
                    } else {
                        wipe_armed_until_ = seconds_ + 6.0;
                        ui_.sound().say(Cue::Refuse);
                    }
                }
                if (armed) {
                    // A destructive decision is red, which is the one of the five permitted colours
                    // that means exactly this.
                    ui_.draw().hue(Rect{row.x0, row.y1 - metrics.px(2.0f), row.x1, row.y1},
                                   0xD03A2Bu, 1.0f);
                }
            }
        }
    }

    settings_height_ = column.y - started_at + metrics.px(12.0f);
    ui_.end_scroll();
}

// Everything back to how the game shipped: the settings, and the files.
//
// The files go to the recycle bin rather than away, for the same reason a delete does (D491) — a
// player who presses this and then realises has somewhere to go, and a "reset everything" that is
// genuinely unrecoverable is a button nobody should be offered. What the game ships with is not
// touched at all: it is not in here, it is beside the executable, and it comes back by existing
// rather than by being restored.
std::string Shell::wipe_everything() {
    preferences_ = Preferences{};
    knobs_ = Knobs{};
    knobs_.changed = true;
    dock_ = Dock{};
    window_settings_ = dock_.add("settings", Family::Parameters);
    window_worlds_ = dock_.add("worlds", Family::Library);

    u32 gone = 0;
    u32 failed = 0;
    std::error_code error;
    for (const Kind& kind : shipped_kinds()) {
        const std::filesystem::path shelf = root_ / kind.folder;
        if (!std::filesystem::is_directory(shelf, error)) {
            error.clear();
            continue;
        }
        for (const std::filesystem::directory_entry& item :
             std::filesystem::directory_iterator(shelf, error)) {
            if (send_to_recycle_bin(item.path())) {
                ++gone;
            } else {
                ++failed;
            }
        }
        error.clear();
    }
    // The built worlds go outright: they are this machine's own workings rather than anybody's
    // files, they are the largest thing in here by far, and a recycle bin with a third of a
    // gigabyte of them in it is not a kindness.
    std::filesystem::remove_all(root_ / "cache", error);
    error.clear();

    save();
    library_.ensure_folders();
    library_.open(shipped_kinds()[kind_]);
    clear_selection();
    ui_.close_all_sections();
    ui_.sound().say(Cue::Erase);
    WS_LOG_INFO("shell", "reset everything: {} things recycled, {} refused", gone, failed);
    if (failed > 0) {
        return "settings reset; " + std::to_string(gone) + " things recycled, " +
               std::to_string(failed) + " could not be";
    }
    return "settings reset and " + std::to_string(gone) + " things sent to the recycle bin";
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

    // --- where you are, and the one control that is not an operation --------------------------
    //
    // The toolbar is gone (D488). It was seven drawings — up, new folder, new, rename, duplicate,
    // delete, sort — and six of them are what a right-click now puts under the pointer, on the
    // thing pointed at, with the count in the label. Two ways to do one thing is two places for it
    // to be wrong, and the row they took is a row of the listing back.
    //
    // *Up* stays, because it is not an operation on a selection: it is where you are, and it
    // belongs beside the breadcrumb that says so. There is nothing to right-click to reach it — a
    // folder you are inside is not an entry in its own listing.
    {
        const Rect where = column.next();
        const Rect up{where.x0, where.y0, where.x0 + metrics.row(), where.y1};
        if (!library_.at_top() &&
            ui_.icon_button(id_of("library.up"), up, Icon::Up, "Out of this folder")) {
            clear_selection();
            library_.up();
        }
        // The breadcrumb takes the left of the row and the search takes the right of it. One row
        // for both, because *where you are* and *what you are looking for* are the same question
        // asked two ways, and a shelf of four hundred clips is answered by the second one.
        const f32 search_w = std::min(where.width() * 0.5f, metrics.px(110.0f));
        const Rect crumb{up.x1 + metrics.px(4.0f), where.y0, where.x1 - search_w - metrics.px(4.0f),
                         where.y1};
        ui_.draw().push_clip(crumb);
        ui_.label(crumb, library_.breadcrumb(), Align::Left, kPlain, 0.7f);
        ui_.draw().pop_clip();

        const Rect find{where.x1 - search_w, where.y0, where.x1, where.y1};
        // Committed on every keystroke rather than on enter. A search that only searches when you
        // press a key you were not told about is a search box that appears not to work — and the
        // listing is a directory read of a folder that is already in the page cache, so there is
        // nothing here to be spared.
        if (ui_.field(id_of("library.search"), find, search_, "search",
                      "Show only the ones whose name has this in it")) {
            // committed; the live text below is what actually filters
        }
        library_.set_filter(ui_.text_of(id_of("library.search"), search_));
        // The folder's own menu — new folder, new, sort — is what a right-click on the empty space
        // below the listing gives, and a full folder has no empty space. The row that says which
        // folder you are in is the one target that is always there.
        if (!ui_.any_menu_open() && ui_.right_pressed_in(where)) {
            clear_selection();
            menu_of_.clear();
            ui_.open_menu(id_of("library.menu"), ui_.pointer_x(), ui_.pointer_y());
        }
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
    // While a menu is up, nothing under it acts on the pointer.
    //
    // Reported as *the buttons of the right click menu each do something they shouldn't ... rename
    // duplicates, delete or duplicate don't do anything, it's just inconsistent* — and every one of
    // those is one cause. The menu is drawn LAST so that it sits over the listing, which means the
    // listing had already hit-tested the same press: clicking an item that happened to float over
    // a row re-selected that row, and clicking one that floated over the empty space below the
    // rows CLEARED the selection — so by the time the item ran, it ran on nothing, or on somebody
    // else. Hence *delete does nothing* and *rename duplicates*: the selection had changed under
    // it and the item list is rebuilt from the selection every frame, so the indices moved too.
    const bool menu_up = ui_.any_menu_open();

    // The rubber band, over the whole listing. Explorer's gesture on purpose: an interface that
    // spends its novelty budget on *selecting things* has spent it in the worst possible place.
    //
    // Not while a menu is up, and this is the half of that bug that gating the ROWS did not close.
    // A press on a menu item is a press inside the listing, so the band started on it, cleared the
    // selection, and then re-picked whichever row its own zero-height box happened to touch — so
    // the item that ran a moment later ran on a row nobody had pointed at. Reported as *the right
    // click menu affects the last thing on the list*, and it is the same one cause as the rest:
    // the menu is drawn over the listing, so the listing must stop listening while it is there.
    Rect box{};
    bool band_done = false;
    const bool banding = !menu_up && ui_.band(id_of("library.band"), list, box, band_done);
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
        if (over && !menu_up && ui_.pressed_in(row)) {
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
                open_entry(entry, verdict);
            }
        }
        // A right-click is the other way to reach everything on the toolbar, and it is the way
        // somebody who has used a computer will try first. On something already selected it keeps
        // the selection, so *delete* on four files is one gesture; on anything else it selects
        // that one first, because a menu about a thing you did not point at is a menu that acts on
        // the wrong file.
        if (over && !menu_up && ui_.right_pressed_in(row)) {
            hit_row = true;
            if (!selected(entry)) select_only(entry);
            // What the menu is about, fixed the moment it opens. Reading the selection each frame
            // meant the menu could be about something else by the time an item was chosen.
            menu_of_ = selection();
            ui_.open_menu(id_of("library.menu"), ui_.pointer_x(), ui_.pointer_y());
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

        // The world you are standing in is BOLD.
        //
        // A library listing every world you have looks the same from inside one as it does from
        // the title, so the one question it could not answer was the one you are most likely to be
        // asking: which of these am I in. Weight rather than a wash or a mark, because it is a
        // property of the row's own name rather than a thing that has happened to the row.
        const bool playing_this =
            stage_ == Stage::World && !entry.folder && !playing_.empty() && entry.shown == playing_;
        // And what came with the game is bold too. Same weight for the same reason: it is a
        // property of the name rather than something that has happened to the row, and a player who
        // wonders why *delete* is greyed on one row and not another has the answer in the row.
        const f32 text_x = row.x0 + metrics.px(8.0f) + cell;
        const Rect words{text_x, row.y0, right - aside_width, row.y1};
        ui_.draw().push_clip(words);
        ui_.label(Rect{text_x + ui_.scroll_overflow(id_of("library.row", i), words, entry.shown),
                       row.y0, right, row.y1},
                  entry.shown, Align::Left, (playing_this || entry.shipped) ? kBold : kPlain);
        ui_.draw().pop_clip();

        if (!aside.empty()) {
            ui_.draw().text(right, row.mid_y() - DrawList::cap_height(metrics.small_text()) * 0.5f,
                            aside, metrics.small_text(), kPlain, Align::Right, 0.55f);
        }
    }

    if (band_done) ui_.sound().say(Cue::Step);
    ui_.end_scroll();

    // Clicking the empty space of a listing clears the selection, which is what a file manager
    // does and what a player will try first. Not while a menu is up: that press belongs to the
    // menu, and clearing the selection under it is what made *delete* do nothing.
    if (!menu_up && ui_.pressed_in(list) && !hit_row && !ctrl && !shift) clear_selection();
    // And right-clicking it is the menu about the FOLDER rather than about anything in it.
    if (!menu_up && ui_.right_pressed_in(list) && !hit_row) {
        clear_selection();
        menu_of_.clear();
        ui_.open_menu(id_of("library.menu"), ui_.pointer_x(), ui_.pointer_y());
    }

    draw_library_menu(verdict);
}

// Everything a selection can do, at the pointer.
//
// Drawn last, after the listing, because the order marks are added is the order they composite in
// and a menu belongs over the thing it is about. The items are the toolbar's, in the same order,
// so the two never disagree about what a library can do — and each says how many things it is
// about, because *delete* over four files and *delete* over one are different decisions and only
// one of them needs thinking about.
void Shell::draw_library_menu(Verdict& verdict) {
    const u64 id = id_of("library.menu");
    if (!ui_.menu_open(id)) return;

    // What it was opened ON, not what is selected now. Those were the same thing until a click on
    // the menu started changing the selection underneath it, and reading it fresh each frame is
    // what let the item list — which is built from it — change shape between the press and the
    // release, so the index that came back meant a different row of a different menu.
    const std::vector<Entry> chosen = menu_of_;
    const bool one = chosen.size() == 1;
    const bool any = !chosen.empty();
    const bool folder = one && chosen.front().folder;
    const bool worlds = shipped_kinds()[kind_].folder == "worlds";

    // Counted into the label rather than into a second line, because a menu with a count in it is
    // a menu you can act on without going back to look at the list.
    const std::string many = any ? (" " + std::to_string(chosen.size())) : std::string();
    const std::string open_label =
        folder ? "open" : (worlds ? "enter this world" : "edit");
    const std::string duplicate_label = one ? "duplicate" : ("duplicate" + many);
    const std::string delete_label = one ? "delete" : ("delete" + many);

    // A built-in one can be opened and copied and nothing else. It is not the player's to rename or
    // to lose, and a delete that came back on the next launch would look like the delete failed.
    bool any_shipped = false;
    for (const Entry& entry : chosen) {
        if (entry.shipped) any_shipped = true;
    }

    std::vector<Ui::MenuItem> items;
    if (any) {
        items.push_back({folder ? Icon::Folder : (worlds ? Icon::Play : Icon::Editor), open_label,
                         one});
        items.push_back({Icon::Rename, "rename", one && !any_shipped});
        items.push_back({Icon::Duplicate, duplicate_label, true});
        items.push_back({Icon::Delete, delete_label, !any_shipped});
    } else {
        items.push_back({Icon::Folder, "new folder", true});
        items.push_back({Icon::New, "new one of these", true});
        items.push_back({Icon::Sort, "sort", true});
    }

    const i32 picked = ui_.menu(id, items.data(), static_cast<u32>(items.size()));
    if (picked < 0) return;

    if (!any) {
        if (picked == 0) {
            naming_folder_ = true;
            folder_buffer_ = "new folder";
        } else if (picked == 1) {
            make_new_file();
        } else {
            const u32 by = (static_cast<u32>(library_.sort()) + 1) % 4;
            library_.set_sort(static_cast<Sort>(by), library_.descending());
            say(std::string("sorted by ") + std::string(kSortNames[by]), 1.5);
        }
        return;
    }

    switch (picked) {
        case 0:
            if (one) {
                if (chosen.front().folder) {
                    clear_selection();
                    library_.enter(chosen.front());
                    ui_.sound().say(Cue::Open);
                } else {
                    open_entry(chosen.front(), verdict);
                }
            }
            break;
        case 1:
            if (one) {
                renaming_ = chosen.front().name;
                rename_buffer_ = chosen.front().shown;
            }
            break;
        case 2:
            say(library_.duplicate(chosen), 2.5);
            ui_.sound().say(Cue::Commit);
            break;
        case 3: {
            std::string why = library_.erase(chosen);
            if (why.empty()) why = "moved to the trash";
            say(why, 2.5);
            clear_selection();
            ui_.sound().say(Cue::Erase);
            break;
        }
        default:
            break;
    }
}

// What *use this* means, for one entry, wherever it was asked for — a double-click, the menu, or
// anything added later. One place, because "open" meaning three things is exactly the sort of rule
// that ends up spelled differently in each of them.
void Shell::open_entry(const Entry& entry, Verdict& verdict) {
    if (entry.folder) {
        clear_selection();
        library_.enter(entry);
        ui_.sound().say(Cue::Open);
        return;
    }
    if (shipped_kinds()[kind_].folder == "worlds") {
        verdict.open_world = true;
        verdict.world = entry.path;
        // Entering a world closes the interface. It is a departure and not a setting: the windows
        // that were up were about choosing where to go, and once you are going they are over the
        // thing you went to look at. This is the same rule Escape's *playing* state is made of.
        shut_windows();
        ui_.sound().say(Cue::Open);
        return;
    }
    open_document(entry.path);
    tab_ = 2;
    ui_.sound().say(Cue::Open);
}

void Shell::make_new_file() {
    // A new world is not an empty file.
    //
    // It was, and an empty clip script builds to nothing — so *new* on the worlds shelf made a
    // world that opened as an empty sky, which is indistinguishable from the failure this project
    // has now chased three times. Until there is something to generate a world FROM, a new one is
    // the facility: one line, resolved through the shipped clips (D494), and a line a player can
    // read and delete once they have something of their own to put there.
    const bool worlds = shipped_kinds()[kind_].folder == "worlds";
    const std::string contents =
        worlds ? "# A new world. It starts as the building the game ships with; delete this line\n"
                 "# and put your own shapes here, or edit it into something else entirely.\n"
                 "include \"facility.clip\"\n"
               : std::string();

    std::string name = "untitled";
    u32 nth = 2;
    while (!library_.create(name, preferences_.username, contents).empty() && nth < 100) {
        name = "untitled " + std::to_string(nth);
        ++nth;
    }
    say("made " + name, 2.0);
    ui_.sound().say(Cue::Commit);
}

void Shell::say(std::string line, f64 seconds) {
    if (line.empty()) return;
    message_ = std::move(line);
    message_until_ = seconds_ + seconds;
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
