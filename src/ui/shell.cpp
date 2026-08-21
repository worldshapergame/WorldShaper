#include "ui/shell.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <map>
#include <unordered_set>

#include "core/hash.hpp"
#include "core/log.hpp"
#include "core/time.hpp"
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
    // Before anything reads it, and not inside whichever panel happens to read it first.
    //
    // The parameters window is drawn BEFORE the editor tab, and it is the one that writes: a slider
    // rewrites the bytes of a number, which makes the graph stale, and the panel that drew from the
    // stale graph on the next frame would hand `write_clip_number` the OLD text for a span that now
    // holds the new one. That comparison is what stops a stale graph writing through — so it
    // refuses, correctly, and the slider stops moving after the first frame of the drag. One line
    // here rather than a call in each reader, because a reader that forgets it is a slider that
    // half works.
    // A file that is not there any more is not a file this tab can edit.
    //
    // Reported directly: *if i delete something you can still go into its editing tab even if you
    // deleted it and therefore have nothing selected.* D772 taught the tab to notice that the file
    // under a path had been REPLACED; this is the simpler half it did not cover — the path leading
    // nowhere at all. Checked here rather than in the tab that draws it, because the tab is not
    // drawn when the window is shut and the document is still open behind it.
    if (!editing_.empty()) {
        std::error_code gone;
        if (!std::filesystem::exists(editing_, gone) || gone) {
            close_document("that file is not there any more");
        }
    }
    if (!waiting_.empty()) {
        std::error_code gone;
        if (!std::filesystem::exists(waiting_, gone) || gone) waiting_.clear();
    }
    refresh_graph();
    // And the verdict, when the text has been still long enough to be worth asking about.
    if (parse_wanted_ && seconds_ >= parse_at_) reparse();
    // The left-hand window is the parameters family, and a NODE's parameters are a parameters
    // window (`23-shell-and-libraries.md` §5c) — which is the whole reason there are two families.
    // So while a node is selected in the visual view, this side is that node; the rest of the time
    // it is the game's own settings.
    if (dock_.is_open(window_settings_)) {
        if (a_node_is_selected()) {
            draw_node_parameters(dock_.rect(window_settings_));
        } else {
            draw_settings(dock_.rect(window_settings_));
        }
    }
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
        "reset",  "collapsed", "expanded",  "pattern", "graph",   "inside",
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

    // Which shelf, ABOVE which tab. Asked for directly, and it is the right way round: the shelf
    // says what KIND of thing this window is about and the tabs say what you are doing with it, so
    // the shelf is the broader question and the one that reads first. It also means the row of
    // drawings a player picks a kind from is in the same place whichever tab is showing, rather
    // than appearing and disappearing under it.
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
        const Rect shelves{rect.x0 + metrics.px(6.0f), top, rect.x1 - metrics.px(6.0f),
                           top + metrics.row()};
        if (ui_.choice(id_of("library.kind"), shelves, {}, icons.data(), labels.data(),
                       hints.data(), static_cast<u32>(kinds.size()), want)) {
            kind_ = want;
            clear_selection();
            library_.open(kinds[kind_]);
        }
        top = shelves.y1 + metrics.px(2.0f);
    }

    const Rect tabs{rect.x0, top, rect.x1, top + metrics.row()};
    const u32 was_tab = tab_;
    ui_.tabs(id_of("library.tabs"), tabs, kTabIcons, kTabLabels, 3, tab_);
    // Choosing something and then opening the editor is a player asking to edit that thing. It
    // used to open on *open something first*, which is the question they had just answered.
    if (tab_ == 2 && was_tab != 2) {
        follow_selection();
        // And the window is given room, ONCE, the first time the editor is opened in a session.
        //
        // A library window is a quarter of the screen because that is the right size for a list of
        // names. A document is not a list of names: the graph of `clips/sampler.clip` fitted a
        // quarter-screen panel at 38%, which is a size at which a box's name is a smudge. Once, and
        // only wider — a player who drags it back has said what they want and is not argued with
        // again.
        give_editor_room();
    }

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
    draw_new_world_menu();
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
    // A world has TWO things you can do to it and *open* can only be one of them. Entering it is
    // what a double-click means and is what the row is mostly for; editing it is how you get at
    // what it is made of, and before this there was no way to reach that at all — the one file
    // kind the editor could not open was the one the game makes when a player presses *new*.
    const bool world_file = worlds && one && !folder;

    // A built-in one can be opened and copied and nothing else. It is not the player's to rename or
    // to lose, and a delete that came back on the next launch would look like the delete failed.
    bool any_shipped = false;
    for (const Entry& entry : chosen) {
        if (entry.shipped) any_shipped = true;
    }

    // What each row DOES, kept beside the row rather than worked out from its index. The menu is
    // one item longer on a world than on a clip, and a switch over positions is how a menu that
    // changes shape comes to run the wrong thing — which is a fault this menu has already had once
    // (D488, the selection changing under an open menu).
    enum class Act : u32 { Open, Edit, Rename, Duplicate, Delete, NewFolder, NewFile, Sort };
    std::vector<Act> acts;
    std::vector<Ui::MenuItem> items;
    if (any) {
        items.push_back({folder ? Icon::Folder : (worlds ? Icon::Play : Icon::Editor), open_label,
                         one});
        acts.push_back(Act::Open);
        if (world_file) {
            items.push_back({Icon::Editor, "edit", true});
            acts.push_back(Act::Edit);
        }
        items.push_back({Icon::Rename, "rename", one && !any_shipped});
        acts.push_back(Act::Rename);
        items.push_back({Icon::Duplicate, duplicate_label, true});
        acts.push_back(Act::Duplicate);
        items.push_back({Icon::Delete, delete_label, !any_shipped});
        acts.push_back(Act::Delete);
    } else {
        items.push_back({Icon::Folder, "new folder", true});
        acts.push_back(Act::NewFolder);
        items.push_back({Icon::New, "new one of these", true});
        acts.push_back(Act::NewFile);
        items.push_back({Icon::Sort, "sort", true});
        acts.push_back(Act::Sort);
    }

    const i32 picked = ui_.menu(id, items.data(), static_cast<u32>(items.size()));
    if (picked < 0 || static_cast<usize>(picked) >= acts.size()) return;

    switch (acts[static_cast<usize>(picked)]) {
        case Act::NewFolder:
            naming_folder_ = true;
            folder_buffer_ = "new folder";
            break;
        case Act::NewFile:
            make_new_file();
            break;
        case Act::Sort: {
            const u32 by = (static_cast<u32>(library_.sort()) + 1) % 4;
            library_.set_sort(static_cast<Sort>(by), library_.descending());
            say(std::string("sorted by ") + std::string(kSortNames[by]), 1.5);
            break;
        }
        case Act::Open:
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
        case Act::Edit:
            if (one) open_editor(chosen.front().path);
            break;
        case Act::Rename:
            if (one) {
                renaming_ = chosen.front().name;
                rename_buffer_ = chosen.front().shown;
            }
            break;
        case Act::Duplicate:
            say(library_.duplicate(chosen), 2.5);
            ui_.sound().say(Cue::Commit);
            break;
        case Act::Delete: {
            std::string why = library_.erase(chosen);
            if (why.empty()) why = "moved to the trash";
            say(why, 2.5);
            clear_selection();
            ui_.sound().say(Cue::Erase);
            break;
        }
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
    open_editor(entry.path);
    ui_.sound().say(Cue::Open);
}

void Shell::make_new_file() {
    // **A new world is not the facility.**
    //
    // It was: one line, `include "facility.clip"`, on the reasoning that an empty clip script builds
    // to nothing and a world that opens as an empty sky is indistinguishable from a world that
    // failed. That was a stopgap and it did the damage a stopgap does — every new world was the
    // same building, so a player who edited one and made another could not tell their edit from the
    // default and reported it as the edit having been lost. Asked for directly: *make it so that the
    // facility complex is no longer a fallback for an empty world, it's just a world we can use for
    // testing and benchmarking.*
    //
    // So a new world asks what it is made from, and nothing is written until it has been answered.
    const bool worlds = shipped_kinds()[kind_].folder == "worlds";
    if (worlds) {
        making_world_ = 1;
        gather_parts(1);
        ui_.open_menu(id_of("library.newworld"), ui_.pointer_x(), ui_.pointer_y());
        return;
    }
    const std::string contents;

    std::string name = "untitled";
    u32 nth = 2;
    while (!library_.create(name, preferences_.username, contents).empty() && nth < 100) {
        name = "untitled " + std::to_string(nth);
        ++nth;
    }
    // And it asks what it is called, now, on the row it was just made on.
    //
    // A shelf fills up with `untitled`, `untitled 2`, `untitled 3` otherwise — and a name typed a
    // week later is a name typed after the thing has stopped being obvious. The rename is the one
    // that already exists (in place, on the row, because a dialog for a name is a dialog); all this
    // does is start it. Escape leaves the name it was given, which is what escape does everywhere.
    library_.refresh(true);
    renaming_ = name + shipped_kinds()[kind_].extension;
    rename_buffer_ = name;
    // And the caret is IN it, with `untitled` chosen, so the first key typed is the name. A field
    // that has to be clicked before it takes a letter is a field that waits to be noticed.
    ui_.type_into(id_of("library.rename.field"), name);
    ui_.sound().say(Cue::Commit);
}

// A world made out of a clip on the shelf, or out of nothing at all.
std::string Shell::make_world_from(const std::filesystem::path& clip) {
    std::string contents;
    if (clip.empty()) {
        // Empty, and it says so IN the file rather than leaving a player looking at an empty sky
        // wondering whether it failed. A comment is what the script view shows and what the graph
        // carries as one box, so there is something on the screen either way.
        contents =
            "# A world of your own. Nothing in it yet.\n"
            "#\n"
            "# Put shapes here, or press the right mouse button in the visual view and take a clip\n"
            "# off your shelf -- a world is a document made of documents.\n"
            "metre 32\n";
    } else {
        contents = "# A world built from " + clip.stem().string() +
                   ".\nmetre 32\ninclude \"" + clip.filename().string() + "\"\n";
    }

    std::string name = clip.empty() ? std::string("untitled") : clip.stem().string();
    std::string why = library_.create(name, preferences_.username, contents);
    for (u32 nth = 2; !why.empty() && nth < 100; ++nth) {
        name = (clip.empty() ? std::string("untitled") : clip.stem().string()) + " " +
               std::to_string(nth);
        why = library_.create(name, preferences_.username, contents);
    }
    if (!why.empty()) return why;
    library_.refresh(true);
    renaming_ = name + shipped_kinds()[kind_].extension;
    rename_buffer_ = name;
    ui_.type_into(id_of("library.rename.field"), name);
    ui_.sound().say(Cue::Commit);
    return {};
}

// What a new world is made from, asked before anything is written.
void Shell::draw_new_world_menu() {
    const u64 id = id_of("library.newworld");
    if (making_world_ == 0) return;
    if (!ui_.menu_open(id)) {
        // Escaped out of, or clicked away from. Nothing was made, which is the point of asking
        // first: a world called `untitled` that a player did not choose is the thing this replaced.
        making_world_ = 0;
        return;
    }

    enum class Act : u32 { Terrain, Clips, Empty, FromClip };
    std::vector<Act> acts;
    std::vector<i32> args;
    std::vector<Ui::MenuItem> items;
    std::vector<std::string> labels;

    if (making_world_ == 1) {
        labels.push_back("from terrain");
        items.push_back({Icon::World, labels.back(), false});
        acts.push_back(Act::Terrain);
        args.push_back(0);
        labels.push_back("from a clip");
        items.push_back({Icon::Clip, labels.back(), !part_choices_.empty()});
        acts.push_back(Act::Clips);
        args.push_back(0);
        labels.push_back("empty");
        items.push_back({Icon::New, labels.back(), true});
        acts.push_back(Act::Empty);
        args.push_back(0);
    } else {
        for (const std::filesystem::path& one : part_choices_) {
            labels.push_back(shown_name(one));
            items.push_back({Icon::Clip, labels.back(), true});
            acts.push_back(Act::FromClip);
            args.push_back(static_cast<i32>(&one - part_choices_.data()));
        }
    }
    for (usize i = 0; i < items.size(); ++i) items[i].label = labels[i];

    const i32 picked = ui_.menu(id, items.data(), static_cast<u32>(items.size()));
    if (picked < 0 || static_cast<usize>(picked) >= acts.size()) return;
    const usize at = static_cast<usize>(picked);

    switch (acts[at]) {
        case Act::Terrain:
            // Greyed out above, so this cannot be reached. Left here so that the day terrain
            // arrives there is one place to put it.
            making_world_ = 0;
            return;
        case Act::Clips:
            making_world_ = 2;
            ui_.open_menu(id, ui_.pointer_x(), ui_.pointer_y());
            return;
        case Act::Empty: {
            making_world_ = 0;
            const std::string why = make_world_from({});
            if (!why.empty()) {
                say(why, 3.0);
                ui_.sound().say(Cue::Refuse);
            }
            return;
        }
        case Act::FromClip: {
            making_world_ = 0;
            const usize which = static_cast<usize>(args[at]);
            if (which >= part_choices_.size()) return;
            const std::string why = make_world_from(part_choices_[which]);
            if (!why.empty()) {
                say(why, 3.0);
                ui_.sound().say(Cue::Refuse);
            }
            return;
        }
    }
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

// --- the editor: two views of one document ------------------------------------------------------
//
// `23-shell-and-libraries.md` §5c and D452: the visual editor and the script editor are two views
// of the same document, and editing either updates the other, live. Neither is the master. What
// they both read and write is `lines_` — the document as the author wrote it — and the graph in
// between is `game/clip_graph.hpp`, re-read whenever the text changes.
//
// **Both views are coloured by the same three things** (D755). The script's words and the graph's
// wires are `ui::tint_of`'s three rotations of the player's own ink, and the legend over the graph
// says what each means: a shape, a value, a material. Two colour schemes would be two things to
// learn for one document.

namespace {

// How big a node's own square of the layout is, in interface pixels before the zoom. A node is
// drawn inside it with room to spare, and the room is where the wires run.
// A cell, and how much of it a box fills. What is LEFT is where the wires run (D781), so these four
// numbers are the width of every channel and every lane in the picture.
//
// They were 106/38 at 0.80/0.62, which leaves a channel of 21 interface pixels. Reported with a
// photograph of a world of forty-two parts: *sometimes wires are too close together*. Forty-two
// wires in twenty-one pixels is half a pixel each, and no routing rule fixes a channel that narrow.
// Now the box gives up a tenth of its width and a fourteenth of its height, and the cell is a
// little larger, which is 33 pixels of channel and 19 of lane — a little over half as much again.
//
// The other half of that report is answered in `cells_tall`: a box gets taller when it has more
// wires arriving than its edge has room for.
constexpr f32 kCellWide = 118.0f;
constexpr f32 kCellTall = 42.0f;
constexpr f32 kNodeWide = 0.74f;   // of a cell
constexpr f32 kNodeTall = 0.56f;
// How much edge a wire needs to arrive at, in interface pixels. A box with more inputs than fit at
// this pitch grows until they do. Four rather than eight because the box has to grow by a cell for
// every seven wires, and a document of forty parts would otherwise be a picture mostly of one box.
constexpr f32 kPortPitch = 4.0f;
constexpr f32 kZoomLeast = 0.30f;
constexpr f32 kZoomMost = 2.40f;
// Below this the words on a node are not words any more, so the node draws its name and nothing
// else. That is what stops a hundred-node document being a hundred smudges.
// Below this a box is its drawing and its name and nothing else — which at a whole document's worth
// of boxes is what there is room for, and is still enough to read the SHAPE of the clip. The second
// line and the settings mark come back on the way in.
constexpr f32 kZoomForDetail = 0.62f;
// The tabs a wire is made from are small, and a small target is a target nobody hits. You zoom in
// to join two things up, which is the one job that needs the pointer to be accurate.
constexpr f32 kZoomForPorts = 0.55f;
// The smallest a document is allowed to OPEN at.
//
// **This is D760 reversed, and it is worth saying so.** That decision opened a document at no less
// than 80% on the reasoning that fitting fifty boxes into a docked panel makes every name a smudge.
// It does not: at a third of full size a name is still set at `small_text`, which is the size the
// loading screen letters itself at, and what a player loses is the second line rather than the
// word. What the 80% floor actually cost was the RIGHT-HAND END of every document — the `solid`,
// which is the whole answer of a clip — off the side of the panel and reported as those nodes being
// missing, which is what an off-screen thing is from a chair.
//
// So a document opens whole wherever whole is legible at all, and the wheel is for looking closer
// rather than for finding out what is there.
constexpr f32 kZoomOpen = 0.32f;

// A node's drawing. The icon is the control and the label is the fallback (14-ui-style.md), which
// here means: what kind of thing this is has to be legible before the name is read.
Icon icon_of(const ClipNode& node) {
    if (node.opaque) return Icon::Editor;          // lines this reader could not read, carried whole
    if (node.head == "material") return Icon::Material;
    if (node.head == "paint") return Icon::Rename;  // a pen over a line, which is what a coat is
    if (node.head == "include") return Icon::Library;
    if (node.head == "solid" || node.head == "region") return Icon::World;
    if (node.head == "metre" || node.head == "meter" || node.head == "bounds" ||
        node.head == "param" || node.head == "origin" || node.head == "variation") {
        return Icon::Settings;
    }
    return (node.carries == ClipCarries::Shape) ? Icon::Clip : Icon::Pattern;
}

// What the node says on one line, in the author's own numbers. Words first, because the word is
// usually the subject — `paint stone where=grain` reads in that order and the numbers qualify it.
std::string summary_of(const ClipNode& node) {
    if (node.opaque) {
        const usize first = node.source.find('\n');
        return (first == std::string::npos) ? node.source : node.source.substr(0, first);
    }
    std::string out;
    for (const ClipWord& word : node.words) {
        if (!out.empty()) out += " ";
        out += word.key.empty() ? word.text : (word.key + "=" + word.text);
    }
    std::string last_key;
    for (const ClipNumber& number : node.numbers) {
        if (!number.key.empty() && number.key == last_key) {
            out += "," + number.text;
            continue;
        }
        if (!out.empty()) out += " ";
        out += number.key.empty() ? number.text : (number.key + "=" + number.text);
        last_key = number.key;
    }
    return out;
}

// What a positional number is called, which the language does not say and the shapes agree about
// anyway: every position in a clip is a triple, and a solid written as two opposite corners is two
// of them. Anything else is numbered, because a made-up name is worse than an index.
std::string positional_name(const ClipNode& node, usize index, usize count) {
    static constexpr const char* kAxes[3]{"x", "y", "z"};
    if (count == 1) return node.head;   // `metre 32`, `weather desert 0.6`
    if (count == 3) return kAxes[index % 3];
    if (count == 6) return std::string(kAxes[index % 3]) + ((index < 3) ? "0" : "1");
    return std::to_string(index + 1);
}

// Which shelf a file belongs to, by its own extension. A document is opened from four places and
// only one of them is a row of the shelf it matches.
Icon icon_for_file(const std::filesystem::path& path) {
    const std::string extension = path.extension().string();
    for (const Kind& kind : shipped_kinds()) {
        if (extension == kind.extension || (!kind.also.empty() && extension == kind.also)) {
            return kind.icon;
        }
    }
    return Icon::Editor;
}

}  // namespace

void Shell::open_editor(const std::filesystem::path& path) {
    dock_.set_open(window_worlds_, true);
    give_editor_room();
    tab_ = 2;
    if (same_file(path) != editing_ || !document_is_current()) open_document(path);
    waiting_.clear();
}

bool Shell::open_editor_view(std::string_view which) {
    if (which == "script") {
        view_ = 0;
        return true;
    }
    if (which == "visual") {
        view_ = 1;
        return true;
    }
    return false;
}

std::string Shell::enter_node(std::string_view name) {
    if (editing_.empty()) return "nothing is open";
    refresh_graph();
    for (const ClipNode& node : graph_.nodes) {
        if (node.target.empty() || node.name != name) continue;
        const std::filesystem::path went = follow_include(node.target);
        if (went.empty()) return "there is no '" + node.target + "' beside this";
        enter_document(went);
        return {};
    }
    return std::string(name) + " is not a door in this document";
}

std::string Shell::add_node(std::string_view head) {
    if (editing_.empty()) return "nothing is open";
    refresh_graph();
    remember("add");
    std::string made;
    const std::string why =
        add_clip_node(lines_, graph_, std::string(head), menu_x_, menu_y_, made);
    document_changed(why);
    if (why.empty()) {
        for (usize i = 0; i < graph_.nodes.size(); ++i) {
            if (graph_.nodes[i].name == made && graph_.nodes[i].placed) {
                choose(static_cast<u32>(i));
                break;
            }
        }
        say("added " + made, 2.0);
    }
    return why;
}

bool Shell::choose_node(std::string_view names) {
    refresh_graph();
    // A comma-separated list, so a choice of SEVERAL — which is what a dragged box makes, and which
    // changes the drawing and the whole of the menu — is a thing a scripted run can put on screen.
    // Without it the one state a box-select exists to produce is a state no photograph ever shows.
    std::vector<u32> want;
    usize at = 0;
    while (at <= names.size()) {
        const usize comma = names.find(',', at);
        const std::string_view one =
            names.substr(at, (comma == std::string_view::npos) ? std::string_view::npos : comma - at);
        for (usize i = 0; i < graph_.nodes.size(); ++i) {
            if (graph_.nodes[i].name == one) {
            reveal(static_cast<u32>(i));
            want.push_back(static_cast<u32>(i));
        }
        }
        if (comma == std::string_view::npos) break;
        at = comma + 1;
    }
    if (want.empty()) return false;
    choose_many(want);
    view_ = 1;
    open_settings();
    return true;
}

// Where an `include` points, by the rule the game itself resolves one with (D494): beside the file
// that says it, and — only when there is nothing there — the folder of clips the game ships with.
// Beside always wins, because that is what lets a player copy a building's parts next to their own
// world and edit them.
std::filesystem::path Shell::follow_include(const std::string& named) const {
    // The same places, in the same order, as the splicer looks in — because a door that opens a
    // different file from the one the world is built out of is worse than a door that does not
    // open. `ui::where_includes_live` is the one list.
    std::error_code error;
    const std::filesystem::path beside = (editing_.parent_path() / named).lexically_normal();
    if (std::filesystem::exists(beside, error) && !error) return beside;
    for (const std::string& where : where_includes_live(library_.root())) {
        error.clear();
        const std::filesystem::path there =
            (std::filesystem::path(where) / named).lexically_normal();
        if (std::filesystem::exists(there, error) && !error) return there;
    }
    return {};
}

// Choosing something in the library and then opening the editor is a player asking to edit that
// thing. An editor that answers with *open something first* has asked the question they have just
// finished answering, and that is what this is for (D455 is unchanged: the editor still asks for a
// file first — a selection is now one of the ways of telling it).
void Shell::follow_selection() {
    const std::vector<Entry> chosen = selection();
    if (chosen.size() != 1 || chosen.front().folder) return;
    // The same PATH is not the same FILE. Deleting a world while it is open and making another of
    // that name gives the second one the first one's path — and answering "already open" there put
    // the dead world back on the screen and, on the next save, back on the disk.
    if (same_file(chosen.front().path) == editing_ && document_is_current()) return;
    if (dirty_) {
        // Nothing is thrown away and nothing pops up. The editor says which file is waiting and
        // opens it the moment this one is saved — which is one press, on the button that is
        // already there.
        waiting_ = chosen.front().path;
        return;
    }
    open_document(chosen.front().path);
}

// A library window is a quarter of the screen because that is the right size for a list of names.
// A document is not a list of names: the graph of `clips/sampler.clip` fitted a quarter-screen panel
// at 38%, which is a size at which a box's name is a smudge.
//
// Once a session, and only ever WIDER. A player who drags it back has said what they want, and an
// interface that argues with that on the next press is an interface with an opinion.
void Shell::give_editor_room() {
    if (widened_for_editor_) return;
    widened_for_editor_ = true;
    dock_.widen(window_worlds_, 0.42f);
}

void Shell::refresh_graph() {
    if (!graph_stale_) return;
    graph_stale_ = false;
    graph_ = read_clip_graph(lines_);
    // The one place the key is turned back into an index. A node that has gone — its line deleted,
    // its name typed over — deselects itself here, which is the only sane thing to do with a
    // selection whose subject no longer exists.
    // Every key back to an index, once. A node that has gone — its line deleted, its name typed
    // over — drops out of the selection here, which is the only sane thing to do with a choice
    // whose subject no longer exists.
    chosen_set_.clear();
    std::vector<std::string> still;
    for (const std::string& key : chosen_nodes_) {
        const u32 at = graph_.find(key);
        if (at == ClipGraph::kNone) continue;
        chosen_set_.push_back(at);
        still.push_back(key);
    }
    chosen_nodes_ = still;
    chosen_index_ = chosen_node_.empty() ? ClipGraph::kNone : graph_.find(chosen_node_);
    if (chosen_index_ == ClipGraph::kNone && !chosen_set_.empty()) {
        chosen_index_ = chosen_set_.front();
        chosen_node_ = chosen_nodes_.front();
    }
    lay_out_graph();
    // And how far the script view has to be able to travel sideways. Measured here rather than in
    // the draw, because it is a pass over every line of the document and the draw happens sixty
    // times a second to a document that changes on a keystroke.
    script_wide_ = 0.0f;
    for (const std::string& line : lines_) {
        script_wide_ = std::max(script_wide_, DrawList::measure(line, ui_.metrics().text()));
    }
}

void Shell::choose(u32 index) {
    if (index >= graph_.nodes.size()) {
        chosen_node_.clear();
        chosen_nodes_.clear();
        chosen_set_.clear();
        chosen_index_ = ClipGraph::kNone;
        return;
    }
    chosen_node_ = ClipGraph::key_of(graph_.nodes[index]);
    chosen_nodes_.assign(1, chosen_node_);
    chosen_set_.assign(1, index);
    chosen_index_ = index;
}

void Shell::choose_many(const std::vector<u32>& indices) {
    chosen_nodes_.clear();
    chosen_set_.clear();
    for (u32 index : indices) {
        if (index >= graph_.nodes.size()) continue;
        chosen_nodes_.push_back(ClipGraph::key_of(graph_.nodes[index]));
        chosen_set_.push_back(index);
    }
    if (chosen_set_.empty()) {
        choose(ClipGraph::kNone);
        return;
    }
    chosen_index_ = chosen_set_.front();
    chosen_node_ = chosen_nodes_.front();
}

bool Shell::is_chosen(u32 index) const {
    return std::find(chosen_set_.begin(), chosen_set_.end(), index) != chosen_set_.end();
}

bool Shell::a_node_is_selected() const {
    return dock_.is_open(window_worlds_) && tab_ == 2 && view_ == 1 &&
           chosen_index_ < graph_.nodes.size();
}

// --- one box, one cell ---------------------------------------------------------------------
//
// **Two boxes may not stand in the same place, and a wire may not cross one.** Asked for directly:
// *make wires or nodes never be able to overlap any of each other even if it requires them moving
// and adjusting a little.* Both halves come out of one rule — **every box is on a whole cell** —
// and the picture is then guaranteed rather than tidied up afterwards:
//
// - a box occupies the top-left `kNodeWide` x `kNodeTall` of its cell, so the strip down the right
//   of every column and the strip along the bottom of every row are clear of boxes for ever;
// - a wire that only ever turns inside those strips cannot cross a box, whatever the layout does.
//
// The alternative was a repulsion pass — push boxes apart until nothing overlaps — which is what a
// physics engine does to a graph and gives a different picture every time it is run.
namespace {

u64 cell_key(i32 x, i32 y) {
    return (static_cast<u64>(static_cast<u32>(x)) << 32) | static_cast<u32>(y);
}

// The nearest whole cell nothing is standing on, taken and remembered. A ring at a time outwards,
// so a box pushed off its place lands beside it rather than at the end of the document — and along
// the row before down it, because a graph is read left to right and a gap in a row is cheaper to
// follow than a gap in a column.
void take_free_cell(std::unordered_set<u64>& taken, f32& x, f32& y, u32 tall = 1, u32 wide = 1) {
    const i32 cx = static_cast<i32>(std::lround(x));
    const i32 cy = static_cast<i32>(std::lround(y));
    const i32 deep = static_cast<i32>(std::max(1u, tall));
    const i32 across = static_cast<i32>(std::max(1u, wide));
    const auto all_free = [&](i32 at_x, i32 at_y) {
        for (i32 c = 0; c < across; ++c) {
            for (i32 k = 0; k < deep; ++k) {
                if (taken.count(cell_key(at_x + c, at_y + k)) != 0) return false;
            }
        }
        return true;
    };
    const auto claim = [&](i32 at_x, i32 at_y) {
        for (i32 c = 0; c < across; ++c) {
            for (i32 k = 0; k < deep; ++k) taken.insert(cell_key(at_x + c, at_y + k));
        }
        x = static_cast<f32>(at_x);
        y = static_cast<f32>(at_y);
    };
    for (i32 ring = 0; ring < 96; ++ring) {
        for (i32 dy = -ring; dy <= ring; ++dy) {
            for (i32 dx = -ring; dx <= ring; ++dx) {
                if (std::max(std::abs(dx), std::abs(dy)) != ring) continue;
                if (!all_free(cx + dx, cy + dy)) continue;
                claim(cx + dx, cy + dy);
                return;
            }
        }
    }
    claim(cx, cy);
}

}  // namespace

// --- where every box sits ------------------------------------------------------------------------
//
// Worked out once when the document is re-read rather than every frame, and worked out from the
// DOCUMENT rather than from anything the interface remembers — so the picture is the same every
// time a clip is opened, on any machine, and there is no layout file to lose (D445).
//
// A column is how far a node is from a leaf, which is fixed by what is made of what. A row is the
// part that can be chosen, and choosing it well is most of what makes a graph readable: the first
// version put every node with no inputs in one column in the order they were written, and a
// fifty-line clip came out as a wall of thirty boxes with wires crossing it in every direction.
// This sorts each column by where its neighbours sit and repeats, which is the ordinary barycentre
// heuristic — six passes, alternating direction, because that is where it stops improving on the
// documents in this repository.
bool Shell::node_is_open(u32 index) const {
    if (index >= graph_.nodes.size()) return false;
    const std::string key = ClipGraph::key_of(graph_.nodes[index]);
    return std::find(opened_.begin(), opened_.end(), key) != opened_.end();
}

void Shell::open_node(u32 index, bool open) {
    if (index >= graph_.nodes.size()) return;
    const std::string key = ClipGraph::key_of(graph_.nodes[index]);
    const auto at = std::find(opened_.begin(), opened_.end(), key);
    if (open && at == opened_.end()) {
        opened_.push_back(key);
    } else if (!open && at != opened_.end()) {
        opened_.erase(at);
    }
    lay_out_graph();
}

// How many cells a box covers. Two rows to a cell when it is open, which at full size is about a
// row's worth of pixels each — the same height they have in the panel on the left, because they
// are the same rows.
u32 Shell::cells_tall(u32 index) const {
    if (index >= graph_.nodes.size()) return 1;
    u32 tall = 1;
    if (node_is_open(index)) {
        usize rows = 0;
        for (const ClipProperty& offer : clip_properties_of(graph_.nodes[index].head)) {
            rows += offer.parts;
        }
        if (rows > 0) tall = 1 + static_cast<u32>((rows + 1) / 2);
    }
    // And tall enough for its wires to land apart from one another. A box takes as many inputs as
    // the author wrote and its edge is one cell high, so a union of twenty gets twenty wires into
    // twenty-three pixels — which is the crowding that was reported, and it is not a routing fault
    // but a box that is the wrong size for what arrives at it.
    usize wires = 0;
    for (u32 input : graph_.nodes[index].inputs) {
        if (input < node_shown_.size() && node_shown_[input]) ++wires;
    }
    if (index < implied_.size()) {
        for (u32 input : implied_[index]) {
            if (input < node_shown_.size() && node_shown_[input]) ++wires;
        }
    }
    if (wires > 1) {
        const f32 want = static_cast<f32>(wires) * kPortPitch;
        tall = std::max(tall, static_cast<u32>(std::ceil(want / (kCellTall * kNodeTall))));
    }
    return std::min(tall, 24u);
}

u32 Shell::cells_wide(u32 index) const {
    return (index < graph_.nodes.size() && node_is_open(index)) ? 2u : 1u;
}

void Shell::go_inside(u32 index) {
    if (index >= graph_.nodes.size()) return;
    inside_.push_back(ClipGraph::key_of(graph_.nodes[index]));
    graph_pan_x_ = 0.0f;
    graph_pan_y_ = 0.0f;
    graph_fitted_ = false;
    lay_out_graph();
}

void Shell::reveal(u32 index) {
    if (index >= graph_.nodes.size()) return;
    // The shortest way in that puts this one on screen: the chain of things that use it, outermost
    // first. Bounded by the node count, because a document being typed into can name itself in a
    // ring — and a chain of parents is what "inside" is made of, so it is the same walk the mark
    // does one press at a time.
    std::vector<u32> chain;
    std::vector<bool> seen(graph_.nodes.size(), false);
    u32 at = index;
    while (at < graph_.nodes.size() && !seen[at] && chain.size() < graph_.nodes.size()) {
        seen[at] = true;
        if (at >= used_by_.size() || used_by_[at].empty()) break;
        const u32 user = used_by_[at].front();
        chain.push_back(user);
        at = user;
    }
    inside_.clear();
    for (usize i = chain.size(); i-- > 0;) {
        inside_.push_back(ClipGraph::key_of(graph_.nodes[chain[i]]));
    }
    graph_fitted_ = false;
    lay_out_graph();
}

void Shell::lay_out_graph() {
    const usize count = graph_.nodes.size();
    graph_x_.assign(count, 0.0f);
    graph_y_.assign(count, 0.0f);
    node_shown_.assign(count, false);
    used_by_.assign(count, {});
    graph_wide_ = 0.0f;
    graph_tall_ = 0.0f;
    if (count == 0) return;

    // --- the wires the language has ------------------------------------------------------------
    //
    // A coat, a weathering and a variation act on the SOLID, and none of them says so: the language
    // applies them to whatever the document turns out to be. Drawn anyway, because a picture that
    // leaves out the one relationship a reader is looking for is a picture of a list.
    //
    // Whichever solid comes AFTER them in the file, or the last one if they come after all of them —
    // which is the order the language applies them in, so it is the one a reader can predict.
    implied_.assign(count, {});
    {
        std::vector<u32> solids;
        for (usize i = 0; i < count; ++i) {
            if (graph_.nodes[i].statement && graph_.nodes[i].head == "solid") {
                solids.push_back(static_cast<u32>(i));
            }
        }
        if (!solids.empty()) {
            for (usize i = 0; i < count; ++i) {
                const ClipNode& node = graph_.nodes[i];
                if (!node.statement) continue;
                if (node.head != "paint" && node.head != "weather" && node.head != "variation") {
                    continue;
                }
                u32 onto = solids.back();
                for (u32 solid : solids) {
                    if (graph_.nodes[solid].line > node.line) {
                        onto = solid;
                        break;
                    }
                }
                if (onto != i) implied_[onto].push_back(static_cast<u32>(i));
            }
        }
    }

    // Who is made of whom, the other way round. Both the fold and the sort going right to left
    // need it, so it is worked out once and kept.
    for (usize i = 0; i < count; ++i) {
        for (u32 input : graph_.nodes[i].inputs) {
            if (input < count) used_by_[input].push_back(static_cast<u32>(i));
        }
        for (u32 input : implied_[i]) used_by_[input].push_back(static_cast<u32>(i));
    }
    // --- what is on screen ---------------------------------------------------------------------
    //
    // Outside anything: the document's own ANSWERS — every statement nothing else uses. That is the
    // `solid`, the coats, the settings, and whatever nobody reads. Six boxes on a fifty-box clip.
    //
    // Inside a box: that box and WHAT IT IS MADE OF, one level down, and nothing else. Going into
    // one of those goes a level further. That is the difference between this and the fold it
    // replaced (D772): a fold showed everything under everything opened, which on any real document
    // is the wall it was meant to prevent.
    //
    // A key that no longer names anything — a line deleted, a name typed over — takes the walk back
    // out to wherever it still holds, which is the only sane thing to do with a place that has
    // stopped existing.
    while (!inside_.empty() && graph_.find(inside_.back()) == ClipGraph::kNone) inside_.pop_back();
    if (inside_.empty()) {
        // **An answer, and what it is made of.** The answers alone was the picture that was
        // reported: *i only see everything separately connected to weather demo instead of one node
        // connected to another because it defines things of it.* And it was exactly backwards — a
        // box is an answer when NOTHING uses it, so the answers-only view hides every box that is
        // joined to something and shows only the ones that are not. A `paint stone` stood alone
        // because the material it names was used, and a `solid block` stood alone because the shape
        // it names was used.
        //
        // One level below each answer fixes that and costs almost nothing: `weather_demo` goes from
        // seven boxes to nine and every wire in the document is on the screen; `clips/sampler.clip`
        // from nine to seventeen of its fifty statements. Two levels is where it stops being a
        // picture — a union of thirty parts is a wall — and going INTO a box is what that is for.
        for (usize i = 0; i < count; ++i) node_shown_[i] = used_by_[i].empty();
        std::vector<bool> answers = node_shown_;
        for (usize i = 0; i < count; ++i) {
            if (!answers[i]) continue;
            for (u32 input : graph_.nodes[i].inputs) {
                if (input < count) node_shown_[input] = true;
            }
            for (u32 input : implied_[i]) node_shown_[input] = true;
        }
    } else {
        const u32 here = graph_.find(inside_.back());
        node_shown_[here] = true;
        for (u32 input : graph_.nodes[here].inputs) {
            if (input < count) node_shown_[input] = true;
        }
        for (u32 input : implied_[here]) node_shown_[input] = true;
    }

    // How far a SHOWN node is from a shown leaf, which is not what the document's own depth says
    // once things are folded away: a closed node stands at the left because nothing it is made of
    // is there to stand left of it.
    std::vector<u32> depth(count, 0);
    for (usize round = 0; round < count && round < 64; ++round) {
        bool moved = false;
        for (usize i = 0; i < count; ++i) {
            if (!node_shown_[i]) continue;
            u32 want = 0;
            for (u32 input : graph_.nodes[i].inputs) {
                if (input < count && node_shown_[input]) want = std::max(want, depth[input] + 1);
            }
            for (u32 input : implied_[i]) {
                if (node_shown_[input]) want = std::max(want, depth[input] + 1);
            }
            if (want > depth[i]) {
                depth[i] = want;
                moved = true;
            }
        }
        if (!moved) break;
    }

    u32 columns = 1;
    for (usize i = 0; i < count; ++i) {
        if (node_shown_[i]) columns = std::max(columns, depth[i] + 1);
    }
    std::vector<std::vector<u32>> column(columns);
    for (usize i = 0; i < count; ++i) {
        if (node_shown_[i]) column[depth[i]].push_back(static_cast<u32>(i));
    }

    std::vector<f32> row(count, 0.0f);
    for (const std::vector<u32>& in : column) {
        for (usize k = 0; k < in.size(); ++k) row[in[k]] = static_cast<f32>(k);
    }

    const std::vector<std::vector<u32>>& used_by = used_by_;

    const auto settle = [&](std::vector<u32>& in, const std::vector<std::vector<u32>>& toward) {
        std::vector<std::pair<f32, u32>> want;
        want.reserve(in.size());
        for (u32 node : in) {
            f32 sum = 0.0f;
            u32 seen = 0;
            for (u32 other : toward[node]) {
                sum += row[other];
                ++seen;
            }
            want.emplace_back(seen > 0 ? sum / static_cast<f32>(seen) : row[node], node);
        }
        // Stable, so a node with no neighbours to be pulled by keeps the place it had rather than
        // being shuffled about by ties.
        std::stable_sort(want.begin(), want.end(),
                         [](const std::pair<f32, u32>& a, const std::pair<f32, u32>& b) {
                             return a.first < b.first;
                         });
        for (usize k = 0; k < want.size(); ++k) {
            in[k] = want[k].second;
            row[in[k]] = static_cast<f32>(k);
        }
    };

    std::vector<std::vector<u32>> inputs_of(count);
    for (usize i = 0; i < count; ++i) {
        if (!node_shown_[i]) continue;
        for (u32 input : graph_.nodes[i].inputs) {
            if (input < count && node_shown_[input]) inputs_of[i].push_back(input);
        }
        for (u32 input : implied_[i]) {
            if (node_shown_[input]) inputs_of[i].push_back(input);
        }
    }
    for (u32 pass = 0; pass < 6; ++pass) {
        for (u32 c = 1; c < columns; ++c) settle(column[c], inputs_of);
        for (u32 c = columns; c-- > 1;) settle(column[c - 1], used_by);
    }

    // --- and a column that is too tall spills sideways -------------------------------------
    //
    // Everything a clip declares before it starts joining things up — its metre, its parameters,
    // its materials and every primitive shape — is made of nothing, so it all lands in the first
    // column. On `clips/sampler.clip` that is twenty-eight boxes in a stack four columns wide, and
    // the picture that comes out is a ribbon: it fits the window at 38%, which is a size at which a
    // name is a smudge.
    //
    // So a column is allowed to be at most `kMostRows` tall and takes as many sub-columns as it
    // needs. Everything of one depth still stands left of everything deeper, so the reading order
    // is untouched; what changes is that the picture is roughly square, which is what a window is.
    constexpr u32 kMostRows = 14;
    std::vector<u32> starts_at(columns, 0);
    u32 across = 0;
    for (u32 c = 0; c < columns; ++c) {
        starts_at[c] = across;
        const u32 tall = static_cast<u32>(column[c].size());
        across += std::max(1u, (tall + kMostRows - 1) / kMostRows);
    }

    for (u32 c = 0; c < columns; ++c) {
        const u32 wide = std::max(1u, static_cast<u32>(column[c].size() + kMostRows - 1) / kMostRows);
        const u32 per = std::max(1u, (static_cast<u32>(column[c].size()) + wide - 1) / wide);
        for (usize k = 0; k < column[c].size(); ++k) {
            const u32 node = column[c][k];
            graph_x_[node] = static_cast<f32>(starts_at[c] + k / per);
            graph_y_[node] = static_cast<f32>(k % per);
        }
    }

    // And what the author dragged wins over all of it. It is in the document, so it survives the
    // file being sent to somebody else (D756).
    //
    // Placed boxes are settled FIRST and in document order, so an authored position is the one that
    // keeps its cell and the layout's own guess is what gives way. Two authored boxes on one cell
    // — a hand-written `#@`, or a file merged from two — is settled by which line comes first,
    // which is at least an answer a reader can predict.
    node_tall_.assign(count, 1);
    node_wide_.assign(count, 1);
    for (usize i = 0; i < count; ++i) {
        if (!node_shown_[i]) continue;
        node_tall_[i] = cells_tall(static_cast<u32>(i));
        node_wide_[i] = cells_wide(static_cast<u32>(i));
    }
    std::unordered_set<u64> taken;
    for (usize i = 0; i < count; ++i) {
        if (!node_shown_[i] || !graph_.nodes[i].placed) continue;
        graph_x_[i] = graph_.nodes[i].at_x;
        graph_y_[i] = graph_.nodes[i].at_y;
        take_free_cell(taken, graph_x_[i], graph_y_[i], node_tall_[i], node_wide_[i]);
    }
    for (usize i = 0; i < count; ++i) {
        if (!node_shown_[i] || graph_.nodes[i].placed) continue;
        take_free_cell(taken, graph_x_[i], graph_y_[i], node_tall_[i], node_wide_[i]);
    }
    // The document's own box, one column past everything and level with the middle of it. Only
    // outside a node: inside one, what is on the canvas is that box and its parts, and the file is
    // not what they answer.
    doc_shown_ = inside_.empty();
    if (doc_shown_) {
        f32 most_x = 0.0f;
        f32 sum_y = 0.0f;
        u32 shown = 0;   // how many wire INTO it, which is what decides how tall it has to be
        for (usize i = 0; i < count; ++i) {
            if (!node_shown_[i]) continue;
            most_x = std::max(most_x, graph_x_[i]);
            if (!used_by_[i].empty()) continue;
            sum_y += graph_y_[i];
            ++shown;
        }
        doc_shown_ = shown > 0;
        doc_x_ = most_x + 1.0f;
        doc_y_ = (shown > 0) ? std::round(sum_y / static_cast<f32>(shown)) : 0.0f;
        // Everything that is an answer wires into it, so it is the box with the most arriving and
        // the one the crowding was photographed on. Tall enough for all of them, and where the
        // author put it if the author has put it anywhere.
        doc_tall_ = 1;
        if (shown > 1) {
            const f32 want = static_cast<f32>(shown) * kPortPitch;
            doc_tall_ = std::min(24u, static_cast<u32>(std::ceil(want / (kCellTall * kNodeTall))));
        }
        // Centred on the middle of what wires into it rather than starting there, so growing it
        // does not push the whole picture down and shrink the size it opens at.
        doc_y_ = std::max(0.0f, doc_y_ - std::floor(static_cast<f32>(doc_tall_ - 1) * 0.5f));
        if (graph_.doc_placed) {
            doc_x_ = graph_.doc_x;
            doc_y_ = graph_.doc_y;
        }
        if (doc_shown_) take_free_cell(taken, doc_x_, doc_y_, doc_tall_);
    }

    for (usize i = 0; i < count; ++i) {
        if (!node_shown_[i]) continue;
        graph_wide_ = std::max(graph_wide_, graph_x_[i] + static_cast<f32>(node_wide_[i]));
        graph_tall_ = std::max(graph_tall_, graph_y_[i] + static_cast<f32>(node_tall_[i]));
    }
    if (doc_shown_) {
        graph_wide_ = std::max(graph_wide_, doc_x_ + 1.0f);
        graph_tall_ = std::max(graph_tall_, doc_y_ + static_cast<f32>(doc_tall_));
    }
}

std::string Shell::add_part(const std::filesystem::path& from) {
    if (editing_.empty()) return "nothing is open";
    std::error_code error;
    if (!std::filesystem::exists(from, error) || error) {
        return "there is no " + from.filename().string() + " to put in";
    }
    refresh_graph();
    menu_x_ = 0.0f;
    menu_y_ = 0.0f;
    return take_part(from);
}

std::string Shell::new_part(std::string_view kind, std::string_view name) {
    if (editing_.empty()) return "nothing is open";
    const u32 which = (kind == "clip") ? 1u : ((kind == "material") ? 2u : 0u);
    if (which == 0) return "a part is a clip or a material";
    refresh_graph();
    menu_x_ = 0.0f;
    menu_y_ = 0.0f;
    return make_part(which, std::string(name));
}

bool Shell::open_node_named(std::string_view name) {
    if (editing_.empty()) return false;
    refresh_graph();
    for (usize i = 0; i < graph_.nodes.size(); ++i) {
        if (graph_.nodes[i].name != name) continue;
        if (clip_properties_of(graph_.nodes[i].head).empty()) return false;
        open_node(static_cast<u32>(i), true);
        return true;
    }
    return false;
}

std::string Shell::new_world(const std::filesystem::path& from) {
    if (shipped_kinds()[kind_].folder != "worlds") return "that shelf does not hold worlds";
    return make_world_from(from);
}

u32 Shell::boxes_overlapping() const {
    std::unordered_set<u64> taken;
    u32 twice = 0;
    for (usize i = 0; i < graph_.nodes.size(); ++i) {
        if (i >= node_shown_.size() || !node_shown_[i]) continue;
        const i32 at_x = static_cast<i32>(std::lround(graph_x_[i]));
        const i32 at_y = static_cast<i32>(std::lround(graph_y_[i]));
        const u32 deep = (i < node_tall_.size()) ? std::max(1u, node_tall_[i]) : 1u;
        const u32 across = (i < node_wide_.size()) ? std::max(1u, node_wide_[i]) : 1u;
        // Every cell an open box covers, not only the one its name is on: a tall box standing on
        // another one is the same fault and looks worse.
        for (u32 c = 0; c < across; ++c) {
            for (u32 k = 0; k < deep; ++k) {
                if (!taken.insert(cell_key(at_x + static_cast<i32>(c), at_y + static_cast<i32>(k)))
                         .second) {
                    ++twice;
                }
            }
        }
    }
    if (doc_shown_ && !taken.insert(cell_key(static_cast<i32>(std::lround(doc_x_)),
                                             static_cast<i32>(std::lround(doc_y_))))
                           .second) {
        ++twice;
    }
    return twice;
}

void Shell::draw_editor_tab(const Rect& rect) {
    const Metrics& metrics = ui_.metrics();
    const Rect body = rect.inset(metrics.px(8.0f));

    // The editor asks for a file first (D455). Not a dialog: it sends you to the library tab, with
    // *new* sitting where the cursor already is. There is no editing without something to edit, and
    // an editor that opens on an untitled nothing has to invent a place to put it.
    if (editing_.empty()) {
        ui_.markdown(body,
                     "### editor\n"
                     "\n"
                     "Choose something on the *library* tab and come back — a clip, a world, "
                     "anything on any shelf — and it opens here.\n"
                     "\n"
                     "**script** is the document as words. **visual** is the same document as boxes "
                     "and wires. Change either and the other changes with it.\n");
        const Rect go{body.x0, body.y1 - metrics.row(), body.x0 + metrics.px(120.0f), body.y1};
        if (ui_.button(id_of("editor.go"), go, Icon::Library, "to the library",
                       "Choose something to edit")) {
            tab_ = 0;
        }
        return;
    }

    refresh_graph();

    Column column;
    column.box = body;
    column.y = body.y0;
    column.row = metrics.row();
    column.gap = metrics.px(4.0f);

    // --- what is being edited ------------------------------------------------------------------
    {
        const Rect bar = column.next();
        const f32 cell = metrics.icon();
        f32 left = bar.x0;
        // The way back out, and only when there is one. A control that is always there and does
        // nothing most of the time is furniture (D486) — and this one says, by being there at all,
        // that you are inside something.
        if (!came_from_.empty() || !inside_.empty()) {
            const Rect back{left, bar.y0, left + metrics.row(), bar.y1};
            const std::string whence =
                inside_.empty() ? ("Back to " + shown_name(came_from_.back())) : "Back out";
            if (ui_.icon_button(id_of("editor.back"), back, Icon::Up, whence)) {
                leave_document();
                return;
            }
            left = back.x1 + metrics.px(2.0f);
        }
        // The drawing is the FILE's kind and not the shelf's. They are usually the same and the one
        // time they are not is the one that matters: a clip opened while the worlds shelf is
        // showing had a globe beside its name, which says the wrong thing about the wrong file.
        ui_.draw().icon(Rect{left, bar.mid_y() - cell * 0.5f, left + cell,
                             bar.mid_y() + cell * 0.5f},
                        icon_for_file(editing_));
        // **There is no save button.** It was a tick at the right of this row, and what a button
        // for saving buys over the key everybody already presses is one more thing on a panel whose
        // first constraint is *as little of everything as possible*. Asked for directly: **ctrl-S**,
        // which is what a player's hands already do, and the star beside the name is what says
        // there is anything to press it for.
        const Rect name{left + cell + metrics.px(5.0f), bar.y0, bar.x1, bar.y1};
        ui_.draw().push_clip(name);
        // A built-in is BOLD, everywhere it is named — the same weight the library's listing gives
        // it (§4), because it is a property of the name rather than something that has happened to
        // the row, and a player who wonders why they cannot type into this one has the answer in it.
        ui_.label(name, shown_name(editing_) + (editing_shipped_ ? "  (built in)" : "") +
                            (dirty_ ? " *" : ""),
                  Align::Left, kBold);
        ui_.draw().pop_clip();
    }

    // The one line the unsaved case needs. Not a dialog: what it says is which file is waiting, and
    // ctrl-S is what answers it.
    if (!waiting_.empty() && same_file(waiting_) != editing_) {
        const Rect row = column.next(metrics.px(16.0f));
        ui_.label(row, shown_name(waiting_) + " opens when this is saved", Align::Left, kPlain,
                  0.75f);
    }

    // --- the two views -------------------------------------------------------------------------
    {
        static constexpr Icon kViews[2]{Icon::Editor, Icon::Graph};
        static constexpr std::string_view kViewLabels[2]{"script", "visual"};
        static constexpr std::string_view kViewHints[2]{
            "The document as words, coloured by what each one is",
            "The same document as boxes and wires - drag, join, and add"};
        const Rect views = column.next();
        u32 want = view_;
        if (ui_.choice(id_of("editor.view"), views, {}, kViews, kViewLabels, kViewHints, 2, want)) {
            // Arriving in the visual view selects whatever the caret was sitting in, so the two
            // views agree about where you are rather than each keeping its own place.
            if (want == 1 && view_ == 0) {
                const u32 under = node_at_line(caret_line_ + 1);
                if (under != ClipGraph::kNone) choose(under);
            }
            view_ = want;
        }
    }

    // --- the parse, which happens as you type and whose failure is not an error -----------------
    const Rect status = column.next(metrics.px(16.0f));
    if (report_.ok) {
        ui_.label(status, "reads", Align::Left, kPlain, 0.55f);
    } else if (report_.line > 0) {
        ui_.label(status, "line " + std::to_string(report_.line) + ": " + report_.message,
                  Align::Left, kPlain, 0.85f);
    } else {
        // The fault is in something this document includes. Naming the file is the whole of what
        // the player can act on; a line number against a file they are not looking at is worse
        // than none.
        ui_.label(status,
                  report_.where.empty() ? report_.message : (report_.where + ": " + report_.message),
                  Align::Left, kPlain, 0.85f);
    }

    // Ctrl-S, from either view, because it is the key everybody presses and neither view should be
    // the one that does not take it. Ctrl-Z and ctrl-Y are here for exactly the same reason — and
    // ctrl-shift-Z as well, because half the world's editors spell redo that way and a player who
    // presses the one their hands know should not have to find out which half this is.
    if (ui_.input().is_down(Key::Ctrl) && !ui_.wants_keys()) {
        if (ui_.input().was_pressed(Key::S)) save_document();
        if (ui_.input().was_pressed(Key::Y) ||
            (ui_.input().was_pressed(Key::Z) && ui_.input().is_down(Key::Shift))) {
            step_forward();
            return;
        }
        if (ui_.input().was_pressed(Key::Z)) {
            step_back();
            return;
        }
        // And the three a node editor owes, on the same keys as the script view's — but only when
        // the graph is the view showing, or one ctrl-C would mean two things.
        if (view_ == 1) {
            if (ui_.input().was_pressed(Key::C) || ui_.input().was_pressed(Key::X)) {
                copy_chosen_nodes(ui_.input().was_pressed(Key::X));
                return;
            }
            if (ui_.input().was_pressed(Key::V)) {
                paste_nodes(menu_x_, menu_y_);
                return;
            }
            if (ui_.input().was_pressed(Key::A)) {
                std::vector<u32> all;
                for (usize i = 0; i < graph_.nodes.size(); ++i) {
                    if (node_shown_[i] && graph_.nodes[i].statement) {
                        all.push_back(static_cast<u32>(i));
                    }
                }
                choose_many(all);
                ui_.sound().say(Cue::Step);
                return;
            }
        }
    }

    const Rect page{column.box.x0, column.y, column.box.x1, column.box.y1};
    if (view_ == 0) {
        draw_script_view(page);
    } else {
        draw_visual_view(page);
    }
}

// --- the script -----------------------------------------------------------------------------
//
// The document as words, each coloured by what it IS: a shape, a value, a material, or none of the
// three. `14-ui-style.md`'s fourth permitted colour, and `game/clip_graph.hpp` is what reads a line
// into runs.
void Shell::draw_script_view(const Rect& page) {
    const Metrics& metrics = ui_.metrics();
    const f32 size = metrics.text();
    const f32 line_height = DrawList::line_height(size) + metrics.px(2.0f);
    const f32 content = static_cast<f32>(lines_.size() + 2) * line_height;
    const u64 scroll_id = id_of("editor.scroll");

    // The caret is kept on screen, because a key that moves it below the fold is a key that appears
    // to do nothing. Only when it has actually MOVED, so a hand on the scroll bar is not fought.
    if (caret_moved_) {
        caret_moved_ = false;
        const f32 at = static_cast<f32>(caret_line_) * line_height;
        const f32 have = ui_.scroll_of(scroll_id);
        if (at < have) ui_.set_scroll(scroll_id, at);
        if (at + line_height > have + page.height()) {
            ui_.set_scroll(scroll_id, at + line_height - page.height());
        }
        // And sideways, for the same reason: a caret past the right edge is a caret that appears
        // not to have moved.
        if (caret_line_ < lines_.size()) {
            const std::string& line = lines_[caret_line_];
            const f32 x = DrawList::measure(
                std::string_view(line).substr(0, std::min<usize>(caret_column_, line.size())), size);
            const f32 room_now =
                std::max(metrics.px(40.0f),
                         page.width() - DrawList::measure("0000", size, kMono) - metrics.px(18.0f));
            if (x < script_pan_x_) script_pan_x_ = x;
            if (x > script_pan_x_ + room_now) script_pan_x_ = x - room_now;
        }
    }

    // The gutter is the one thing that HAS to line up, so it keeps the monospaced step.
    const f32 gutter = DrawList::measure("0000", size, kMono) + metrics.px(8.0f);
    const f32 room = std::max(metrics.px(40.0f), page.width() - gutter - metrics.px(10.0f));
    const f32 across = std::max(0.0f, script_wide_ - room);
    // Sideways with SHIFT and the wheel, which is what every other list in this interface uses the
    // wheel for and what a player already does in a browser. Not a second bar: one bar and one
    // modifier is less of everything than two bars, and the first constraint in `14-ui-style.md` is
    // as little of everything as possible.
    if (page.holds(ui_.pointer_x(), ui_.pointer_y()) && ui_.input().wheel != 0.0f &&
        ui_.input().is_down(Key::Shift)) {
        script_pan_x_ -= ui_.input().wheel * metrics.px(60.0f);
    }
    script_pan_x_ = std::clamp(script_pan_x_, 0.0f, across);

    const Rect inner = ui_.begin_scroll(scroll_id, page, content);

    // Which node the caret is in, drawn as a wash down the lines it covers. That is the whole of
    // the link from this view back to the other one: the statement you are typing in is the box
    // that is lit over there.
    const u32 under = node_at_line(caret_line_ + 1);

    for (usize i = 0; i < lines_.size(); ++i) {
        const f32 y = inner.y0 + line_height * static_cast<f32>(i);
        if (y + line_height < page.y0 || y > page.y1) continue;
        // The line number, quieter than the code, because it is not part of the document.
        ui_.draw().text(inner.x0 + gutter - metrics.px(8.0f), y, std::to_string(i + 1), size, kMono,
                        Align::Right, 0.32f);
        if (under != ClipGraph::kNone && graph_.nodes[under].line <= i + 1 &&
            graph_.nodes[under].last_line >= i + 1) {
            ui_.draw().ink(Rect{inner.x0, y, inner.x1, y + line_height}, 0.06f);
        }
        if (!report_.ok && report_.line == static_cast<u32>(i + 1)) {
            ui_.draw().ink(Rect{inner.x0, y, inner.x1, y + line_height}, 0.14f);
        }

        // What is chosen, as a wash behind the words. Ink rather than a colour, because the words
        // over it invert against whatever is behind them and a coloured band would make the ink
        // rule decide twice.
        if (has_selection()) {
            u32 from_line = 0;
            u32 from_column = 0;
            u32 to_line = 0;
            u32 to_column = 0;
            selection_span(from_line, from_column, to_line, to_column);
            if (i >= from_line && i <= to_line) {
                const std::string& text = lines_[i];
                const usize begin =
                    (i == from_line) ? std::min<usize>(from_column, text.size()) : 0;
                const usize end =
                    (i == to_line) ? std::min<usize>(to_column, text.size()) : text.size();
                const f32 x0 = inner.x0 + gutter - script_pan_x_ +
                               DrawList::measure(std::string_view(text).substr(0, begin), size);
                // A line whose selection ends past its last character shows a little beyond it, so
                // a selection that spans lines reads as one block rather than as a ragged stack.
                const f32 x1 =
                    (end >= text.size() && i < to_line)
                        ? (inner.x0 + gutter - script_pan_x_ +
                           DrawList::measure(text, size) + metrics.px(4.0f))
                        : (inner.x0 + gutter - script_pan_x_ +
                           DrawList::measure(std::string_view(text).substr(0, end), size));
                if (x1 > x0) ui_.draw().ink(Rect{x0, y, x1, y + line_height}, 0.26f);
            }
        }

        // The line, run by run. A word the language knows takes the colour of what it makes, a
        // number takes the value colour, and everything the author chose is the ordinary ink —
        // which is the strongest thing here, because it is the part they wrote.
        //
        // Set PROPORTIONALLY and not monospaced, which is the opposite of what a code editor
        // usually does and is right for this face: a code span steps six cells for every glyph
        // (`kGlyphMonoStep`), and at three columns a letter that is what this typeface is, six is
        // half a line of gaps. Twenty characters of a sixty-character line fitted in the window.
        // Nothing here needs columns to line up — the gutter is the only thing that has to, and it
        // is set on its own.
        const std::string& text = lines_[i];
        ui_.draw().push_clip(
            Rect{inner.x0 + gutter, page.y0, page.x1 - metrics.px(9.0f), page.y1});
        for (const ClipSpan& span : colour_clip_line(text)) {
            if (span.column + span.length > text.size()) continue;
            const u32 which = clip_part_tint(span.part);
            const u32 style = (which == kClipNoTint) ? kPlain : tinted(kPlain, which + 1);
            const f32 coverage = (span.part == ClipPart::Comment)   ? 0.45f
                                 : (span.part == ClipPart::Grammar) ? 0.50f
                                                                    : 1.0f;
            const f32 at = DrawList::measure(std::string_view(text).substr(0, span.column), size);
            ui_.draw().text(inner.x0 + gutter - script_pan_x_ + at, y,
                            std::string_view(text).substr(span.column, span.length), size, style,
                            Align::Left, coverage);
        }

        // Drawn when THIS view has the keyboard, which is when nothing else does.
        //
        // It read `ui_.wants_keys()`, which is the opposite: that is true when a field or a slider
        // has taken the characters, and `edit_keys` below runs on `!wants_keys()`. So the caret was
        // shown exactly when the script view was NOT the thing being typed into, and hidden every
        // time it was — which is a text view with no caret in it at all, and is what the user
        // reported as the typing line not being there.
        if (caret_line_ == i && !ui_.wants_keys()) {
            const usize where = std::min<usize>(caret_column_, text.size());
            ui_.caret(inner.x0 + gutter - script_pan_x_ +
                          DrawList::measure(std::string_view(text).substr(0, where), size),
                      y, y + line_height, caret_since_);
        }
        ui_.draw().pop_clip();
    }

    // --- the pointer: put the caret, drag one out, take a word, take a line ------------------
    //
    // The sideways bar lies inside the page, so its rectangle is worked out first and the caret
    // ignores presses there — otherwise steering the bar drags a selection across the file behind
    // it.
    const f32 bar_tall = metrics.px(5.0f);
    const Rect bar_track =
        (across > 0.0f)
            ? Rect{page.x0 + gutter, page.y1 - bar_tall, page.x1 - metrics.px(9.0f), page.y1}
            : Rect{0.0f, 0.0f, -1.0f, -1.0f};
    const bool on_the_bar = bar_track.holds(ui_.pointer_x(), ui_.pointer_y());

    // Where in the document a point is, which all four of them need.
    const auto at_pointer = [&](u32& line_out, u32& column_out) {
        const f32 local_y = ui_.pointer_y() - inner.y0;
        line_out = static_cast<u32>(
            std::clamp(std::floor(local_y / line_height), 0.0f,
                       static_cast<f32>(lines_.empty() ? 0 : lines_.size() - 1)));
        const std::string& at = lines_.empty() ? rename_buffer_ : lines_[line_out];
        const f32 local_x = ui_.pointer_x() - inner.x0 - gutter + script_pan_x_;
        usize column_at = 0;
        while (column_at < at.size() &&
               DrawList::measure(std::string_view(at).substr(0, column_at + 1), size) < local_x) {
            ++column_at;
        }
        column_out = static_cast<u32>(column_at);
    };

    if (ui_.pressed_in(page) && !on_the_bar) {
        at_pointer(caret_line_, caret_column_);
        const u32 clicks = ui_.input().click_count;
        if (clicks >= 3 && caret_line_ < lines_.size()) {
            // The whole line, which is what a third click means in every editor there is.
            mark_line_ = caret_line_;
            mark_column_ = 0;
            caret_column_ = static_cast<u32>(lines_[caret_line_].size());
        } else if (clicks == 2 && caret_line_ < lines_.size()) {
            u32 from = 0;
            u32 to = 0;
            word_at(lines_[caret_line_], caret_column_, from, to);
            mark_line_ = caret_line_;
            mark_column_ = from;
            caret_column_ = to;
        } else {
            // Shift keeps the anchor, so shift-clicking extends what is already chosen.
            if (!ui_.input().is_down(Key::Shift)) drop_mark();
            selecting_ = true;
        }
        caret_since_ = seconds_;
        caret_moved_ = true;
        ui_.stop_typing();
    }
    if (selecting_ && ui_.input().mouse_left) {
        // Past the edge of the page the view comes along, so a selection can be longer than the
        // window is. Sideways moves the text under the caret; up and down moves the scroll, which
        // this view keeps in `Ui` and reads back on the next frame.
        // A scroll offset counts DOWN the document while a pan counts the content up the screen,
        // so both are held negated here and put back the right way round.
        f32 sideways = -script_pan_x_;
        f32 down = -ui_.scroll_of(scroll_id);
        pan_at_edge(page, sideways, down, 520.0f);
        script_pan_x_ = std::clamp(-sideways, 0.0f, std::max(0.0f, across));
        ui_.set_scroll(scroll_id, std::max(0.0f, -down));
        at_pointer(caret_line_, caret_column_);
        caret_since_ = seconds_;
    }
    if (!ui_.input().mouse_left) selecting_ = false;
    // The bar for the sideways travel. Drawn only when there IS any, because a control that is
    // always there and does nothing most of the time is furniture — the same argument the reset
    // button in a settings row is made of (D486).
    if (across > 0.0f) {
        const Rect& track = bar_track;
        const f32 wide = std::max(metrics.px(28.0f),
                                  track.width() * room / std::max(room, script_wide_));
        // A DRAG, not a jump. It jumped to wherever it was pressed and let go, which is a bar you
        // can aim at once rather than a bar you can steer — reported as the bars not being properly
        // draggable. `Ui::drag_handle` is what a dock's divider already uses: it follows the hand
        // and reports on every frame it moved.
        const f32 travel = std::max(1.0f, track.width() - wide);
        const f32 at = track.x0 + travel * (script_pan_x_ / std::max(across, 1.0f));
        const Rect handle{at, track.y0, at + wide, track.y1};
        f64 along = static_cast<f64>(script_pan_x_);
        // A press in the empty part of the track jumps there first, because a bar you have to drag
        // from one end is a bar that makes you travel the distance you were trying to skip.
        if (ui_.pressed_in(track) && !handle.holds(ui_.pointer_x(), ui_.pointer_y())) {
            along = static_cast<f64>(
                std::clamp((ui_.pointer_x() - track.x0 - wide * 0.5f) / travel, 0.0f, 1.0f) *
                across);
        }
        if (ui_.drag_handle(id_of("editor.script.across"), handle, true, false, travel / across,
                            along) ||
            ui_.pressed_in(track)) {
            script_pan_x_ = std::clamp(static_cast<f32>(along), 0.0f, across);
        }
        ui_.draw().ink(track, 0.07f);
        const f32 drawn = track.x0 + travel * (script_pan_x_ / std::max(across, 1.0f));
        ui_.draw().ink(Rect{drawn, track.y0, drawn + wide, track.y1},
                       ui_.busy() ? 0.55f : 0.30f);
    }

    ui_.end_scroll();

    // The script view eats characters, so the platform has to be composing them — and it takes
    // them only when nothing else has the keyboard, because a rename in the library tab and a
    // caret in the editor would otherwise both receive every letter typed.
    ui_.request_text_input();
    if (!ui_.wants_keys()) edit_keys(ui_.input());
}

// Which node covers a line, preferring the innermost — a `box` inside a `displace` is the answer,
// because that is the one whose numbers a player is looking at.
u32 Shell::node_at_line(u32 line) const {
    u32 best = ClipGraph::kNone;
    u32 span = 0xFFFFFFFFu;
    for (usize i = 0; i < graph_.nodes.size(); ++i) {
        const ClipNode& node = graph_.nodes[i];
        if (node.line > line || node.last_line < line) continue;
        const u32 wide = node.last_line - node.line;
        if (wide < span) {
            span = wide;
            best = static_cast<u32>(i);
        }
    }
    return best;
}

// After anything that changes the document from the visual view. The graph has to be re-read before
// anything reads it again, and the text has to be told it is dirty — three lines, in one place,
// because an edit that forgets one of them is an edit that draws correctly and saves nothing.
// --- undo -------------------------------------------------------------------------------------
//
// Asked for directly. Every other editor a player has used has ctrl-Z, and an editor that changes a
// file and cannot put it back is one people are afraid to press anything in.

void Shell::remember(const char* reason) {
    if (editing_.empty()) return;
    // The same kind of change, close together, is ONE undo. Typing `portico` is a word to whoever
    // typed it and seven keystrokes only to the machine.
    const bool same = undo_reason_ == reason && seconds_ - undo_stamped_ < 0.8;
    undo_reason_ = reason;
    undo_stamped_ = seconds_;
    if (same && !undo_.empty()) return;
    undo_.push_back(DocumentState{lines_, caret_line_, caret_column_});
    // Anything undone and then typed over is gone, which is what every editor does: a redo stack
    // that survives a new edit is a branch, and a branch nobody asked for is a branch nobody finds.
    redo_.clear();
    if (undo_.size() > 64) undo_.erase(undo_.begin());
}

void Shell::step_back() {
    if (undo_.empty()) {
        say("nothing to undo", 1.5);
        ui_.sound().say(Cue::Refuse);
        return;
    }
    redo_.push_back(DocumentState{lines_, caret_line_, caret_column_});
    lines_ = std::move(undo_.back().lines);
    caret_line_ = undo_.back().caret_line;
    caret_column_ = undo_.back().caret_column;
    undo_.pop_back();
    undo_reason_.clear();
    drop_mark();
    dirty_ = true;
    graph_stale_ = true;
    reparse_soon();
    refresh_graph();
    caret_moved_ = true;
    caret_since_ = seconds_;
    ui_.sound().say(Cue::Close);
}

void Shell::step_forward() {
    if (redo_.empty()) {
        say("nothing to redo", 1.5);
        ui_.sound().say(Cue::Refuse);
        return;
    }
    undo_.push_back(DocumentState{lines_, caret_line_, caret_column_});
    lines_ = std::move(redo_.back().lines);
    caret_line_ = redo_.back().caret_line;
    caret_column_ = redo_.back().caret_column;
    redo_.pop_back();
    undo_reason_.clear();
    drop_mark();
    dirty_ = true;
    graph_stale_ = true;
    reparse_soon();
    refresh_graph();
    caret_moved_ = true;
    caret_since_ = seconds_;
    ui_.sound().say(Cue::Open);
}

// --- the keys a node editor owes ---------------------------------------------------------------
//
// The same three the script view has, on the same keys, through the same system clipboard. What
// goes on it is the statements as TEXT — see `game/clip_graph.hpp` for why that and not a private
// format — so a node copied here pastes into a text editor, into another clip, and back.
void Shell::copy_chosen_nodes(bool cut) {
    if (chosen_set_.empty()) {
        say("nothing chosen", 1.5);
        ui_.sound().say(Cue::Refuse);
        return;
    }
    const std::string text = copy_clip_nodes(lines_, graph_, chosen_set_);
    if (text.empty()) {
        say("nothing here can be copied on its own", 2.5);
        ui_.sound().say(Cue::Refuse);
        return;
    }
    set_clipboard_text(text);
    if (!cut) {
        say(chosen_set_.size() == 1 ? "copied" : ("copied " + std::to_string(chosen_set_.size())),
            1.5);
        ui_.sound().say(Cue::Step);
        return;
    }
    remember("cut");
    const std::vector<u32> going = chosen_set_;
    choose(ClipGraph::kNone);
    document_changed(delete_clip_nodes(lines_, graph_, going));
}

void Shell::paste_nodes(f32 x, f32 y) {
    const std::string text = clipboard_text();
    std::vector<std::string> made;
    remember("paste");
    const std::string why = paste_clip_nodes(lines_, graph_, text, x, y, made);
    document_changed(why);
    if (!why.empty()) return;
    // What arrived is what is chosen, so the next thing a hand does is about the thing that just
    // appeared — the same rule duplicate follows.
    std::vector<u32> fresh;
    for (const std::string& name : made) {
        for (usize i = 0; i < graph_.nodes.size(); ++i) {
            if (graph_.nodes[i].name == name) fresh.push_back(static_cast<u32>(i));
        }
    }
    choose_many(fresh);
    say(made.size() == 1 ? "pasted" : ("pasted " + std::to_string(made.size())), 2.0);
}

// A selection pulled past the edge of its window takes the window with it.
//
// Asked for directly: *make it so that the text select of script and the box select of node visual
// editor drag and pan around the window across its edges.* Without it a selection can only ever be
// as long as what is on the screen, which on a sixteen-hundred-line clip is one screen in forty.
// The speed grows with how far past the edge the hand is, because a fixed rate is either too slow
// to cross a document or too fast to stop on a line.
void Shell::pan_at_edge(const Rect& area, f32& pan_x, f32& pan_y, f32 rate) {
    const Metrics& metrics = ui_.metrics();
    const f32 margin = metrics.px(18.0f);
    const f32 x = ui_.pointer_x();
    const f32 y = ui_.pointer_y();
    const auto past = [&](f32 over) {
        // In margins, capped, so a pointer a mile off the window is not a hundred times faster than
        // one just outside it.
        return std::clamp(over / margin, 0.0f, 3.0f);
    };
    f32 dx = 0.0f;
    f32 dy = 0.0f;
    if (x < area.x0 + margin) dx = past(area.x0 + margin - x);
    if (x > area.x1 - margin) dx = -past(x - (area.x1 - margin));
    if (y < area.y0 + margin) dy = past(area.y0 + margin - y);
    if (y > area.y1 - margin) dy = -past(y - (area.y1 - margin));
    if (dx == 0.0f && dy == 0.0f) return;
    const f32 step = rate * metrics.scale * ui_.dt();
    pan_x += dx * step;
    pan_y += dy * step;
}

void Shell::document_changed(const std::string& why) {
    if (!why.empty()) {
        say(why, 3.0);
        ui_.sound().say(Cue::Refuse);
        return;
    }
    dirty_ = true;
    graph_stale_ = true;
    reparse_soon();
    refresh_graph();
    ui_.sound().say(Cue::Commit);
}

// --- the visual view -----------------------------------------------------------------------------
//
// The document's statements as boxes and the names between them as wires, laid out left to right by
// what is made of what. It is drawn from the same `lines_` the script view is, re-read whenever
// those change, so the two cannot disagree about what the document is (D452) — and what it cannot
// draw is a box carrying the source rather than nothing at all (D454).
//
// **It CHANGES the document** (D757): drag a box to move it, drag out of its right-hand tab and
// drop on another to join them, press a left-hand tab to cut that wire, right-click for the palette
// and for *take out*. Every one of those is a line of the file being rewritten — see
// `game/clip_graph.hpp` for the surgery and why each refusal says what it says.
void Shell::draw_visual_view(const Rect& page) {
    const Metrics& metrics = ui_.metrics();

    // **There is no legend.** There was one — three swatches and three words along the top — and it
    // was right that a colour nobody can look up is a colour that means nothing. It was also a row
    // of the panel spent on a sentence a player reads once, which is the first constraint in
    // `14-ui-style.md` broken in the ordinary way: *as little of everything as possible while
    // staying legible*. Asked for directly, and what it costs is stated rather than waved away —
    // the three colours are now learned from the script view, where the word and its colour are the
    // same thing, and from `23-shell-and-libraries.md` §5c, which is the one place they are written
    // down.
    const Rect canvas{page.x0, page.y0, page.x1, page.y1};

    if (graph_.nodes.empty()) {
        ui_.label(Rect{canvas.x0, canvas.y0, canvas.x1, canvas.y0 + metrics.row()},
                  "nothing here yet - right-click to add something", Align::Left, kPlain, 0.55f);
        // **And the right-click has to WORK here**, which it did not: this branch drew the menu and
        // never opened one, so a document emptied of its last node — delete the one `include` line
        // a world is made of, which is exactly what a player does first — could not be added to at
        // all. Reported directly. The way back into a document is the one gesture that must work
        // when there is nothing in it.
        if (canvas.holds(ui_.pointer_x(), ui_.pointer_y()) && !ui_.any_menu_open() &&
            ui_.right_pressed_in(canvas)) {
            menu_about_ = ClipGraph::kNone;
            palette_group_ = -1;
            menu_x_ = 0.0f;
            menu_y_ = 0.0f;
            ui_.open_menu(id_of("editor.graph.menu"), ui_.pointer_x(), ui_.pointer_y());
        }
        draw_part_naming(canvas);
        draw_graph_menu(canvas);
        return;
    }

    // --- how big everything is at this zoom ---------------------------------------------------
    const f32 cell_w = metrics.px(kCellWide) * graph_zoom_;
    const f32 cell_h = metrics.px(kCellTall) * graph_zoom_;
    const f32 node_w = cell_w * kNodeWide;
    const f32 node_h = cell_h * kNodeTall;

    // Fitted to the document the first time it is looked at, so a clip of eighty boxes opens as a
    // picture of a clip rather than as three boxes and a lot of grey.
    if (!graph_fitted_ && graph_wide_ > 0.0f && graph_tall_ > 0.0f) {
        graph_fitted_ = true;
        const f32 fit_x = canvas.width() / std::max(1.0f, metrics.px(kCellWide) * graph_wide_);
        const f32 fit_y = canvas.height() / std::max(1.0f, metrics.px(kCellTall) * graph_tall_);
        // **It opens READABLE, not complete**, and that is the second answer to this question. The
        // first fitted the whole document into the panel, and on a fifty-box clip that is 38% — a
        // size at which every name is a smudge and the picture says only "there are a lot of them".
        // A document too big for the window is what the wheel and the drag are for; a document that
        // fits opens whole, which is most of them.
        graph_zoom_ = std::clamp(std::min(fit_x, fit_y), kZoomOpen, 1.0f);
        const f32 used_x = metrics.px(kCellWide) * graph_wide_ * graph_zoom_;
        const f32 used_y = metrics.px(kCellTall) * graph_tall_ * graph_zoom_;
        // Centred when it fits and at the top left when it does not, because the left of a graph is
        // what everything else is made of and is where a reader starts.
        graph_pan_x_ = (used_x < canvas.width()) ? (canvas.width() - used_x) * 0.5f
                                                 : metrics.px(4.0f);
        graph_pan_y_ = (used_y < canvas.height()) ? (canvas.height() - used_y) * 0.5f
                                                  : metrics.px(4.0f);
        return;   // one frame at the old size rather than a frame drawn at two of them
    }

    const bool over_canvas = canvas.holds(ui_.pointer_x(), ui_.pointer_y());
    const auto layout_x = [&](f32 sx) { return (sx - canvas.x0 - graph_pan_x_) / cell_w; };
    const auto layout_y = [&](f32 sy) { return (sy - canvas.y0 - graph_pan_y_) / cell_h; };

    // --- zoom, about the pointer --------------------------------------------------------------
    if (over_canvas && ui_.input().wheel != 0.0f && !ui_.any_menu_open()) {
        const f32 was = graph_zoom_;
        const f32 lx = layout_x(ui_.pointer_x());
        const f32 ly = layout_y(ui_.pointer_y());
        graph_zoom_ = std::clamp(graph_zoom_ * std::exp(ui_.input().wheel * 0.16f), kZoomLeast,
                                 kZoomMost);
        if (graph_zoom_ != was) {
            // The point under the pointer stays under the pointer, which is the whole of what makes
            // a zoom feel like a zoom rather than like the picture jumping.
            graph_pan_x_ = ui_.pointer_x() - canvas.x0 - lx * metrics.px(kCellWide) * graph_zoom_;
            graph_pan_y_ = ui_.pointer_y() - canvas.y0 - ly * metrics.px(kCellTall) * graph_zoom_;
        }
    }

    // --- panning, on the MIDDLE button --------------------------------------------------------
    //
    // It was the left one, which is the button a box-select needs, and a canvas cannot answer one
    // gesture with two things. Asked for directly. The middle button does nothing else anywhere in
    // this interface, so nothing was given up for it.
    if (over_canvas && ui_.input().mouse_middle) dragging_graph_ = true;
    if (dragging_graph_ && ui_.input().mouse_middle) {
        graph_pan_x_ += ui_.input().mouse_dx;
        graph_pan_y_ += ui_.input().mouse_dy;
    }
    if (!ui_.input().mouse_middle) dragging_graph_ = false;

    // Which CELL a box is on, which the wires need as much as the drawing does: a wire's whole
    // guarantee is that it turns in the strips between cells, and those are named by cell.
    const auto place_of = [&](usize i) {
        f32 lx = graph_x_[i];
        f32 ly = graph_y_[i];
        if (dragging_node_ == i) {
            lx = drag_at_x_;
            ly = drag_at_y_;
        } else if (dragging_node_ < graph_.nodes.size()) {
            for (const auto& along : drag_with_) {
                if (along.first != i) continue;
                lx = drag_at_x_ + along.second.first;
                ly = drag_at_y_ + along.second.second;
            }
        }
        return std::pair<f32, f32>{lx, ly};
    };
    const auto box_of = [&](usize i) {
        const std::pair<f32, f32> at = place_of(i);
        const f32 x = canvas.x0 + graph_pan_x_ + at.first * cell_w;
        const f32 y = canvas.y0 + graph_pan_y_ + at.second * cell_h;
        // An open box covers several cells and stops at the same place in the last of them, so the
        // lane along the bottom of that row is clear exactly as it is under a box of one cell.
        const u32 deep = (i < node_tall_.size()) ? std::max(1u, node_tall_[i]) : 1u;
        const u32 across = (i < node_wide_.size()) ? std::max(1u, node_wide_[i]) : 1u;
        return Rect{x, y, x + static_cast<f32>(across - 1) * cell_w + node_w,
                    y + static_cast<f32>(deep - 1) * cell_h + node_h};
    };
    // Where a wire leaves a node, and where its n-th wire arrives.
    const auto out_port = [&](const Rect& box) {
        return Rect{box.x1 - metrics.px(2.0f), box.mid_y() - metrics.px(4.0f),
                    box.x1 + metrics.px(4.0f), box.mid_y() + metrics.px(4.0f)};
    };
    const auto in_port = [&](const Rect& box, usize which, usize of) {
        const f32 span = box.height() - metrics.px(6.0f);
        const f32 at = box.y0 + metrics.px(3.0f) +
                       span * (of <= 1 ? 0.5f
                                       : static_cast<f32>(which) / static_cast<f32>(of - 1));
        return Rect{box.x0 - metrics.px(4.0f), at - metrics.px(4.0f), box.x0 + metrics.px(2.0f),
                    at + metrics.px(4.0f)};
    };

    // The document's box, worked out before the wires because they arrive at it.
    const Rect doc_box = [&] {
        const f32 at_x = dragging_doc_ ? doc_drag_x_ : doc_x_;
        const f32 at_y = dragging_doc_ ? doc_drag_y_ : doc_y_;
        const f32 x = canvas.x0 + graph_pan_x_ + at_x * cell_w;
        const f32 y = canvas.y0 + graph_pan_y_ + at_y * cell_h;
        return Rect{x, y, x + node_w,
                    y + static_cast<f32>(std::max(1u, doc_tall_) - 1) * cell_h + node_h};
    }();

    ui_.draw().push_clip(canvas);

    // --- the wires, first, so the boxes sit over them -----------------------------------------
    //
    // **A wire never crosses a box, and no two wires lie on top of each other.** Asked for
    // directly. It was three straight segments with the turn just right of the source, so a wire
    // from column one to column six ran flat through everything in between — which on any real
    // document is most of the picture, and is why the graph read as a mess of lines rather than as
    // a diagram.
    //
    // What makes it possible is the cell rule above: a box fills the top-left of its cell, so the
    // **channel** down the right of every column and the **lane** along the bottom of every row are
    // clear of boxes for ever. A wire is then five segments — out into the source's channel, down
    // or up it to a lane, along the lane, into the target's channel, and in — and every turn is in
    // clear ground by construction rather than by luck.
    //
    // Two shortcuts keep it from looking over-engineered. A wire whose straight run crosses nothing
    // takes the straight run: most wires go one column, and a wire that detours for no reason is a
    // wire a reader follows twice. And every wire sharing a channel or a lane is given its own slot
    // in it, ordered by where it starts, so parallel wires read as a bundle rather than as one
    // thick line.
    const f32 thin = std::max(1.0f, metrics.scale * graph_zoom_);
    {
        // Which cells have a box on them, so a straight run can be checked against them.
        std::unordered_set<u64> filled;
        // And the lanes a box runs THROUGH. A box of one cell stops before the lane at the bottom
        // of its row, so that lane is clear; a box that carries on into the next row covers it.
        std::unordered_set<u64> crossed;
        for (usize i = 0; i < graph_.nodes.size(); ++i) {
            if (!node_shown_[i]) continue;
            const std::pair<f32, f32> at = place_of(i);
            const i32 cx = static_cast<i32>(std::lround(at.first));
            const i32 cy = static_cast<i32>(std::lround(at.second));
            const u32 deep = (i < node_tall_.size()) ? std::max(1u, node_tall_[i]) : 1u;
            const u32 across = (i < node_wide_.size()) ? std::max(1u, node_wide_[i]) : 1u;
            for (u32 c = 0; c < across; ++c) {
                for (u32 k = 0; k < deep; ++k) {
                    filled.insert(cell_key(cx + static_cast<i32>(c), cy + static_cast<i32>(k)));
                    if (k + 1 < deep) {
                        crossed.insert(cell_key(cx + static_cast<i32>(c), cy + static_cast<i32>(k)));
                    }
                }
            }
        }

        struct Run {
            Rect from{};            // the box it leaves
            Rect into{};            // the box it arrives at
            f32 y0 = 0.0f;          // where it leaves, in screen y
            f32 y1 = 0.0f;          // where it arrives
            i32 channel_out = 0;    // the column whose right-hand strip it turns in, when it does
            i32 channel_in = 0;     // the column left of the target
            i32 lane = 0;           // the row whose bottom strip it runs along, when it does
            bool straight = true;   // nothing in the way: no lane needed
            u32 rgb = 0;
            bool written = true;    // the document says so, rather than the language meaning it
        };
        // One place that works out which strips a wire turns in, so the document's own wires are
        // routed by exactly the same rule as everything else rather than by a second copy of it.
        const auto route = [&](Run& run, i32 fx, i32 fy, i32 tx) {
            run.channel_out = fx;
            run.channel_in = tx - 1;
            run.straight = tx > fx;
            for (i32 c = fx + 1; run.straight && c < tx; ++c) {
                if (filled.count(cell_key(c, fy)) != 0) run.straight = false;
            }
        };
        // And the lane it runs along, if it needs one: the nearest to the middle of its two ends
        // that no open box runs through. Outwards a row at a time, because a lane one row further
        // is a wire that bends a little more and still never crosses anything.
        const auto clear_lane = [&](i32 want, i32 from_x, i32 to_x) {
            const i32 low = std::min(from_x, to_x);
            const i32 high = std::max(from_x, to_x);
            for (i32 step = 0; step < 24; ++step) {
                for (i32 side = 0; side < 2; ++side) {
                    const i32 lane = want + (side == 0 ? step : -step);
                    bool clear = true;
                    for (i32 c = low; c <= high && clear; ++c) {
                        if (crossed.count(cell_key(c, lane)) != 0) clear = false;
                    }
                    if (clear) return lane;
                    if (step == 0) break;
                }
            }
            return want;
        };
        std::vector<Run> runs;
        for (usize i = 0; i < graph_.nodes.size(); ++i) {
            if (!node_shown_[i]) continue;
            const ClipNode& node = graph_.nodes[i];
            const Rect into = box_of(i);
            const std::pair<f32, f32> into_at = place_of(i);
            // What the document says first, then what the language means. Both arrive at the same
            // edge, so they are counted together.
            std::vector<std::pair<u32, bool>> arriving;
            for (u32 other : node.inputs) {
                if (other < graph_.nodes.size() && node_shown_[other]) {
                    arriving.emplace_back(other, true);
                }
            }
            for (u32 other : implied_[i]) {
                if (other < graph_.nodes.size() && node_shown_[other]) {
                    arriving.emplace_back(other, false);
                }
            }
            const usize shown_all = arriving.size();
            usize shown_before = 0;
            for (usize k = 0; k < arriving.size(); ++k) {
                const u32 input = arriving[k].first;
                const usize mine = shown_before++;
                const Rect from = box_of(input);
                if (std::max(from.x1, into.x1) < canvas.x0) continue;
                if (std::min(from.x0, into.x0) > canvas.x1) continue;
                const std::pair<f32, f32> from_at = place_of(input);
                Run run;
                run.from = from;
                run.into = into;
                run.y0 = from.mid_y();
                run.y1 = in_port(into, mine, shown_all).mid_y();
                run.rgb =
                    tint_rgb(ui_.accent(), clip_carries_tint(graph_.nodes[input].carries));
                run.written = arriving[k].second;
                const i32 fx = static_cast<i32>(std::lround(from_at.first));
                const i32 fy = static_cast<i32>(std::lround(from_at.second));
                const i32 tx = static_cast<i32>(std::lround(into_at.first));
                const i32 ty = static_cast<i32>(std::lround(into_at.second));
                route(run, fx + static_cast<i32>(node_wide_[input]) - 1, fy, tx);
                // The lane nearest the middle of the two ends, so a wire that has to detour does
                // it by as little as the grid allows.
                run.lane = clear_lane(
                    static_cast<i32>(std::lround((static_cast<f32>(fy) + static_cast<f32>(ty)) *
                                                 0.5f)),
                    fx, tx);
                runs.push_back(run);
            }
        }

        // Everything that is an answer wires into the document. This is the whole of what the
        // picture was missing: seven boxes in a column, none of them joined to anything, and a
        // reader with no way to tell a graph from a list.
        if (doc_shown_) {
            // **Only what nothing else uses.** Reported directly: *not every node should be
            // connected to the final node as this doesnt make much sense.* It was every box on the
            // canvas, which was the same set until a box began showing what it is MADE of — after
            // that a material wired both into the coat that names it and into the file, which says
            // the file is made of the material and of the coat separately, and it is not.
            //
            // A box reaches the document when nothing at all reads it: the solid, the settings that
            // frame the world, and each part a manifest is assembled from. Everything else reaches
            // it through whatever uses it, which is what a graph is for.
            std::vector<u32> answers;
            for (usize i = 0; i < graph_.nodes.size(); ++i) {
                if (node_shown_[i] && used_by_[i].empty()) answers.push_back(static_cast<u32>(i));
            }
            std::stable_sort(answers.begin(), answers.end(),
                             [&](u32 a, u32 b) { return graph_y_[a] < graph_y_[b]; });
            for (usize k = 0; k < answers.size(); ++k) {
                const u32 node = answers[k];
                const Rect from = box_of(node);
                const std::pair<f32, f32> from_at = place_of(node);
                Run run;
                run.from = from;
                run.into = doc_box;
                run.y0 = from.mid_y();
                run.y1 = in_port(doc_box, k, answers.size()).mid_y();
                run.rgb = tint_rgb(ui_.accent(), clip_carries_tint(graph_.nodes[node].carries));
                const i32 fx = static_cast<i32>(std::lround(from_at.first));
                const i32 fy = static_cast<i32>(std::lround(from_at.second));
                const i32 tx = static_cast<i32>(std::lround(doc_x_));
                route(run, fx + static_cast<i32>(node_wide_[node]) - 1, fy, tx);
                run.lane = clear_lane(
                    static_cast<i32>(std::lround((static_cast<f32>(fy) + doc_y_) * 0.5f)), fx, tx);
                runs.push_back(run);
            }
        }

        // A slot each, in every strip anything shares. Ordered by where the wire starts, which is
        // the ordering that makes a bundle of them fan out rather than cross.
        std::map<i32, std::vector<usize>> down_channel;
        std::map<i32, std::vector<usize>> along_lane;
        for (usize r = 0; r < runs.size(); ++r) {
            if (!runs[r].straight) down_channel[runs[r].channel_out].push_back(r);
            down_channel[runs[r].channel_in].push_back(r);
            if (!runs[r].straight) along_lane[runs[r].lane].push_back(r);
        }
        std::vector<f32> slot_out(runs.size(), 0.5f);
        std::vector<f32> slot_in(runs.size(), 0.5f);
        std::vector<f32> slot_lane(runs.size(), 0.5f);
        for (auto& strip : down_channel) {
            std::stable_sort(strip.second.begin(), strip.second.end(),
                             [&](usize a, usize b) { return runs[a].y0 < runs[b].y0; });
            const f32 of = static_cast<f32>(strip.second.size() + 1);
            for (usize k = 0; k < strip.second.size(); ++k) {
                const usize r = strip.second[k];
                const f32 where = static_cast<f32>(k + 1) / of;
                if (!runs[r].straight && runs[r].channel_out == strip.first) slot_out[r] = where;
                if (runs[r].channel_in == strip.first) slot_in[r] = where;
            }
        }
        for (auto& strip : along_lane) {
            std::stable_sort(strip.second.begin(), strip.second.end(),
                             [&](usize a, usize b) { return runs[a].y0 < runs[b].y0; });
            const f32 of = static_cast<f32>(strip.second.size() + 1);
            for (usize k = 0; k < strip.second.size(); ++k) {
                slot_lane[strip.second[k]] = static_cast<f32>(k + 1) / of;
            }
        }

        // The strips themselves, in screen coordinates. A channel is what is left of a cell to the
        // right of its box; a lane is what is left below it.
        const f32 channel = cell_w - node_w;
        const f32 lane_deep = cell_h - node_h;
        const auto channel_x = [&](i32 column, f32 where) {
            return canvas.x0 + graph_pan_x_ + static_cast<f32>(column) * cell_w + node_w +
                   channel * where;
        };
        const auto lane_y = [&](i32 row, f32 where) {
            return canvas.y0 + graph_pan_y_ + static_cast<f32>(row) * cell_h + node_h +
                   lane_deep * where;
        };
        // A wire the document SAYS is solid; a wire the language MEANS is half as strong and half
        // as thick, so it reads as a relationship rather than as a line somebody drew. There is
        // nothing in the file to cut, and it has no tab for that reason.
        const auto flat = [&](f32 x0, f32 x1, f32 y, u32 rgb, bool written) {
            const f32 wide = written ? thin : std::max(1.0f, thin * 0.5f);
            ui_.draw().hue(Rect{std::min(x0, x1), y - wide * 0.5f, std::max(x0, x1),
                                y + wide * 0.5f},
                           rgb, written ? 1.0f : 0.62f);
        };
        const auto upright = [&](f32 x, f32 y0, f32 y1, u32 rgb, bool written) {
            const f32 wide = written ? thin : std::max(1.0f, thin * 0.5f);
            ui_.draw().hue(Rect{x - wide * 0.5f, std::min(y0, y1), x + wide * 0.5f,
                                std::max(y0, y1)},
                           rgb, written ? 1.0f : 0.62f);
        };

        for (usize r = 0; r < runs.size(); ++r) {
            const Run& run = runs[r];
            const Rect& from = run.from;
            const Rect& into = run.into;
            const f32 turn_in = channel_x(run.channel_in, slot_in[r]);
            if (run.straight) {
                flat(from.x1, turn_in, run.y0, run.rgb, run.written);
                upright(turn_in, run.y0, run.y1, run.rgb, run.written);
                flat(turn_in, into.x0, run.y1, run.rgb, run.written);
                continue;
            }
            const f32 turn_out = channel_x(run.channel_out, slot_out[r]);
            const f32 along = lane_y(run.lane, slot_lane[r]);
            flat(from.x1, turn_out, run.y0, run.rgb, run.written);
            upright(turn_out, run.y0, along, run.rgb, run.written);
            flat(turn_out, turn_in, along, run.rgb, run.written);
            upright(turn_in, along, run.y1, run.rgb, run.written);
            flat(turn_in, into.x0, run.y1, run.rgb, run.written);
        }
    }

    // The wire being drawn right now, from the tab it left to wherever the hand is.
    if (wiring_from_ < graph_.nodes.size()) {
        const Rect from = box_of(wiring_from_);
        const u32 rgb = tint_rgb(ui_.accent(), clip_carries_tint(graph_.nodes[wiring_from_].carries));
        const f32 y0 = from.mid_y();
        const f32 x1 = ui_.pointer_x();
        const f32 y1 = ui_.pointer_y();
        ui_.draw().hue(Rect{std::min(from.x1, x1), y0 - thin, std::max(from.x1, x1), y0 + thin},
                       rgb, 0.8f);
        ui_.draw().hue(Rect{x1 - thin, std::min(y0, y1), x1 + thin, std::max(y0, y1)}, rgb, 0.8f);
    }

    // --- the boxes ------------------------------------------------------------------------------
    const u32 caret_node = node_at_line(caret_line_ + 1);
    const bool detail = graph_zoom_ >= kZoomForDetail;
    const bool ports = graph_zoom_ >= kZoomForPorts;
    bool hit_a_node = false;
    u32 dropped_on = ClipGraph::kNone;

    for (usize i = 0; i < graph_.nodes.size(); ++i) {
        if (!node_shown_[i]) continue;
        const ClipNode& node = graph_.nodes[i];
        const Rect box = box_of(i);
        if (box.y1 < canvas.y0 || box.y0 > canvas.y1 || box.x1 < canvas.x0 ||
            box.x0 > canvas.x1) {
            continue;
        }
        const bool chosen = is_chosen(static_cast<u32>(i));
        const bool primary = (static_cast<u32>(i) == chosen_index_);
        const bool over = box.holds(ui_.pointer_x(), ui_.pointer_y()) && over_canvas;
        if (over && wiring_from_ < graph_.nodes.size() && wiring_from_ != i) {
            dropped_on = static_cast<u32>(i);
        }

        // The glass FIRST, so the wires running past behind it are hidden rather than showing
        // through the words. A box is a window and a window is in front of what is behind it —
        // and the first version without it drew blue names over green wires, which came out
        // magenta and read as a third colour that means nothing.
        ui_.draw().glass(box, 1.0f);
        ui_.draw().ink(box, chosen ? (primary ? 0.26f : 0.20f) : (over ? 0.15f : 0.10f));
        ui_.draw().edge(box, chosen ? (primary ? 0.95f : 0.75f) : (caret_node == i ? 0.55f : 0.25f),
                        chosen ? 2.0f : 1.0f);

        ui_.draw().push_clip(box);
        const f32 cell = std::min(metrics.icon() * 0.8f, node_h * 0.55f);

        // --- the one mark that says *there is something inside this* -------------------------
        //
        // A box made of other boxes, and a box that is an `include` and therefore made of a whole
        // FILE, are the same statement to a reader: this one has something in it and you can go
        // there. So they get one drawing and one press, and what happens next differs only in how
        // far it goes — a level down, or into another document.
        //
        // It was two marks and one of them was the play triangle, which says GO rather than *go
        // in*: reported as looking like a play button, which it was.
        f32 name_left = box.x0 + metrics.px(3.0f);
        bool has_parts = !node.target.empty();
        for (u32 input : node.inputs) {
            if (input < graph_.nodes.size()) has_parts = true;
        }
        if (has_parts) {
            const f32 in_wide = std::min(cell, node_h * 0.8f);
            const Rect in_mark{name_left, box.mid_y() - in_wide * 0.5f, name_left + in_wide,
                               box.mid_y() + in_wide * 0.5f};
            const bool over_mark = in_mark.holds(ui_.pointer_x(), ui_.pointer_y()) && over_canvas;
            ui_.draw().icon(in_mark, Icon::Inside, over_mark ? 1.0f : 0.7f);
            if (over_mark && ui_.pressed_in(in_mark)) {
                hit_a_node = true;
                ui_.draw().pop_clip();
                ui_.draw().pop_clip();
                if (!node.target.empty()) {
                    const std::filesystem::path went = follow_include(node.target);
                    if (went.empty()) {
                        say("there is no '" + node.target + "' beside this or in the game's clips",
                            3.0);
                        ui_.sound().say(Cue::Refuse);
                        return;
                    }
                    enter_document(went);
                } else {
                    go_inside(static_cast<u32>(i));
                }
                ui_.sound().say(Cue::Open);
                return;
            }
            name_left = in_mark.x1 + metrics.px(2.0f);
        }
        // The drawing goes when the box is small, and the name takes the room back. A fifth of a
        // narrow box spent saying *this is a shape* is a fifth not spent saying WHICH shape, and at
        // a whole document's worth of boxes the name is the only thing that tells one from another
        // — `port` and `porti` are the same word to a reader and `portico` is not.
        if (detail) {
            ui_.draw().icon(Rect{name_left, box.y0 + metrics.px(2.0f), name_left + cell,
                                 box.y0 + metrics.px(2.0f) + cell},
                            icon_of(node));
            name_left += cell + metrics.px(3.0f);
        }

        // --- what this one OFFERS, in the corner --------------------------------------------
        //
        // Two things a box can be, and neither was said anywhere: some of them can be gone INTO —
        // an `include` is a door onto the file it names — and some of them have numbers to change.
        // A player had to double-click everything to find out which. Asked for directly.
        //
        // The drawings are the ones that already mean those things elsewhere in this interface: the
        // play mark is *enter this world* on a shelf row, and the three sliders are *settings*. The
        // enter mark is also a PRESS, so the affordance and the way to use it are the same object
        // rather than a hint about a gesture.
        // And a mark on the right for a box that has numbers to change, at the detail size only: a
        // way in is rare and worth saying at every size, a number is on nearly every box and a mark
        // on nearly every box is a texture rather than a fact.
        //
        // Except on a box that OFFERS a list of named properties — a material, a variation — where
        // the mark is a control: it opens the box where it stands and the sliders are inside it.
        // Asked for directly: *these material nodes should show their settings inside their actual
        // node instead of in another settings window so you can directly tweak them from there.*
        const bool offers_properties = !clip_properties_of(node.head).empty();
        const bool has_numbers = !node.numbers.empty();
        f32 mark_right = box.x1 - metrics.px(3.0f);
        if (offers_properties || (detail && has_numbers)) {
            const Rect at{mark_right - cell * 0.85f, box.y0 + metrics.px(2.0f), mark_right,
                          box.y0 + metrics.px(2.0f) + cell * 0.85f};
            const bool over_mark =
                offers_properties && at.holds(ui_.pointer_x(), ui_.pointer_y()) && over_canvas;
            ui_.draw().icon(at, Icon::Settings,
                            over_mark ? 1.0f : (chosen || node_is_open(static_cast<u32>(i))
                                                    ? 0.85f
                                                    : 0.45f));
            if (over_mark && ui_.pressed_in(at)) {
                hit_a_node = true;
                const bool was = node_is_open(static_cast<u32>(i));
                ui_.draw().pop_clip();
                ui_.draw().pop_clip();
                open_node(static_cast<u32>(i), !was);
                ui_.sound().say(was ? Cue::Close : Cue::Open);
                return;
            }
            mark_right -= cell * 0.85f + metrics.px(2.0f);
        }

        const f32 title = std::max(metrics.small_text(), metrics.text() * graph_zoom_);
        const std::string first = node.name.empty() ? clip_head_shown(node.head) : node.name;
        ui_.draw().push_clip(Rect{box.x0, box.y0, mark_right, box.y1});
        ui_.draw().text(name_left,
                        box.y0 + (detail ? metrics.px(3.0f) : (node_h - title * kGlyphCap) * 0.5f),
                        first, title, node.name.empty() ? kPlain : kBold, Align::Left);
        ui_.draw().pop_clip();

        if (detail) {
            // What it IS on the left and what its numbers are on the right, with room reserved for
            // each: at a hundred interface pixels a material's name and its `rgb=` ran into one
            // another and came out as `mgterisl24,120,112`, which is neither of them.
            const f32 small = metrics.small_text();
            const f32 under_y = box.y0 + node_h * 0.56f;
            const f32 left = box.x0 + metrics.px(4.0f);
            const f32 right = box.x1 - metrics.px(4.0f);
            f32 spent = 0.0f;
            if (!node.name.empty()) {
                const u32 which = clip_carries_tint(node.carries);
                const std::string head_word = clip_head_shown(node.head);
                ui_.draw().text(left, under_y, head_word, small, tinted(kPlain, which + 1),
                                Align::Left, 0.85f);
                spent = DrawList::measure(head_word, small) + metrics.px(5.0f);
            }
            const std::string values = summary_of(node);
            if (!values.empty() && left + spent < right) {
                ui_.draw().push_clip(Rect{left + spent, box.y0, right, box.y1});
                ui_.draw().text(right, under_y, values, small, kPlain, Align::Right, 0.42f);
                ui_.draw().pop_clip();
            }
        }
        ui_.draw().pop_clip();

        // --- what it is made of, inside itself ------------------------------------------------
        //
        // A line under the name and then one slider a row, in the box rather than in a panel
        // somewhere else. It is the same code the panel on the left runs (`draw_property_rows`),
        // given a different rectangle — so a property changed here and a property changed there
        // write the same bytes into the same line.
        if (node_is_open(static_cast<u32>(i)) && box.height() > node_h + metrics.px(2.0f)) {
            usize rows = 0;
            for (const ClipProperty& offer : clip_properties_of(node.head)) rows += offer.parts;
            const Rect body{box.x0 + metrics.px(3.0f), box.y0 + node_h,
                            box.x1 - metrics.px(3.0f), box.y1 - metrics.px(2.0f)};
            ui_.draw().ink(Rect{box.x0, box.y0 + node_h - metrics.px(1.0f), box.x1,
                                box.y0 + node_h},
                           0.25f);
            if (rows > 0 && body.height() > metrics.px(6.0f)) {
                const f32 each = body.height() / static_cast<f32>(rows);
                ui_.draw().push_clip(body);
                draw_property_rows(node, body, body, each,
                                   hash_combine(id_of("node.inside"), i), false);
                ui_.draw().pop_clip();
            }
            // The body holds the pointer: a press on a slider is not a press on the box, or every
            // drag of a value would pick the whole node up and carry it away.
            if (body.holds(ui_.pointer_x(), ui_.pointer_y())) hit_a_node = true;
        }

        // --- the tabs a wire is made from -----------------------------------------------------
        if (ports) {
            const u32 rgb = tint_rgb(ui_.accent(), clip_carries_tint(node.carries));
            const Rect out = out_port(box);
            ui_.draw().hue(out, rgb, 0.9f);
            if (over_canvas && ui_.pressed_in(out)) {
                wiring_from_ = static_cast<u32>(i);
                hit_a_node = true;
                ui_.sound().say(Cue::Step);
            }
            // The tabs are on the wires the DOCUMENT says, and the count includes the ones the
            // language means — otherwise a tab would sit where no wire arrives.
            usize shown_inputs = 0;
            for (u32 input : node.inputs) {
                if (input < graph_.nodes.size() && node_shown_[input]) ++shown_inputs;
            }
            for (u32 input : implied_[i]) {
                if (input < graph_.nodes.size() && node_shown_[input]) ++shown_inputs;
            }
            usize drawn = 0;
            for (usize k = 0; k < node.inputs.size(); ++k) {
                if (node.inputs[k] >= graph_.nodes.size() || !node_shown_[node.inputs[k]]) continue;
                const Rect in = in_port(box, drawn, shown_inputs);
                ++drawn;
                const u32 from_rgb =
                    (node.inputs[k] < graph_.nodes.size())
                        ? tint_rgb(ui_.accent(),
                                   clip_carries_tint(graph_.nodes[node.inputs[k]].carries))
                        : rgb;
                const bool over_port = in.holds(ui_.pointer_x(), ui_.pointer_y()) && over_canvas;
                ui_.draw().hue(in, from_rgb, over_port ? 1.0f : 0.75f);
                if (over_port) {
                    ui_.draw().edge(in.inset(-metrics.px(1.0f)), 0.8f);
                    if (ui_.pressed_in(in)) {
                        hit_a_node = true;
                        remember("wire");
                        document_changed(disconnect_clip_node(lines_, graph_,
                                                              static_cast<u32>(i),
                                                              static_cast<u32>(k)));
                        ui_.draw().pop_clip();
                        return;   // the graph under this loop has just been re-read
                    }
                }
            }
        }

        // --- pressing, dragging, and the menu -------------------------------------------------
        if (over && !ui_.any_menu_open()) {
            if (ui_.right_pressed_in(box)) {
                hit_a_node = true;
                // On something already chosen it KEEPS the choice, so *take out 4* is one gesture;
                // on anything else it chooses that one first, because a menu about a thing you did
                // not point at acts on the wrong node. The library's own rule (D482), and it has to
                // be the same rule or the two menus mean different things.
                if (!is_chosen(static_cast<u32>(i))) choose(static_cast<u32>(i));
                menu_about_ = static_cast<u32>(i);
                palette_group_ = -1;
                ui_.open_menu(id_of("editor.graph.menu"), ui_.pointer_x(), ui_.pointer_y());
            } else if (ui_.pressed_in(box) && wiring_from_ >= graph_.nodes.size()) {
                hit_a_node = true;
                if (ui_.input().is_down(Key::Ctrl)) {
                    // Ctrl adds one to the choice or takes it out again, which is what it does in
                    // the library and in every file manager the player has already used.
                    std::vector<u32> want = chosen_set_;
                    const auto at = std::find(want.begin(), want.end(), static_cast<u32>(i));
                    if (at == want.end()) {
                        want.push_back(static_cast<u32>(i));
                    } else {
                        want.erase(at);
                    }
                    choose_many(want);
                    open_settings();
                    ui_.sound().say(Cue::Step);
                    continue;
                }
                if (!is_chosen(static_cast<u32>(i))) choose(static_cast<u32>(i));
                // A node's parameters are a parameters window (§5c), so choosing one opens the
                // left-hand side on it — which is the whole reason the two families exist.
                open_settings();
                ui_.sound().say(Cue::Step);
                if (ui_.input().click_count >= 2 && !node.target.empty()) {
                    // An `include` is a door. A world is a manifest — twenty lines naming the files
                    // it is assembled out of — so a visual view of one that could not be walked
                    // through would be twenty boxes and no way to reach anything they stand for.
                    const std::filesystem::path went = follow_include(node.target);
                    if (went.empty()) {
                        say("there is no '" + node.target + "' beside this or in the game's clips",
                            3.0);
                        ui_.sound().say(Cue::Refuse);
                    } else {
                        ui_.draw().pop_clip();
                        enter_document(went);
                        ui_.sound().say(Cue::Open);
                        return;
                    }
                } else if (ui_.input().click_count >= 2) {
                    // Straight to the line it is written on, in the other view. Two views of one
                    // document means being able to get from either to the other at the place you
                    // were looking at, rather than at the top.
                    caret_line_ = (node.line > 0) ? node.line - 1 : 0;
                    caret_column_ = 0;
                    caret_moved_ = true;
                    view_ = 0;
                    ui_.sound().say(Cue::Open);
                } else if (node.statement &&
                           ui_.pointer_y() <= box.y0 + node_h) {
                    // A statement can be picked up and put somewhere. A sub-expression cannot: it
                    // is drawn where whatever uses it puts it, and a position written on its line
                    // would be a second answer to a question its parent has already answered.
                    dragging_node_ = static_cast<u32>(i);
                    drag_at_x_ = graph_x_[i];
                    drag_at_y_ = graph_y_[i];
                    drag_grab_x_ = layout_x(ui_.pointer_x()) - drag_at_x_;
                    drag_grab_y_ = layout_y(ui_.pointer_y()) - drag_at_y_;
                    // Everything else chosen comes along, at the offset it already had. Choosing
                    // four things and moving one of them out from under the other three is not what
                    // a box round four things meant.
                    drag_with_.clear();
                    for (u32 other : chosen_set_) {
                        if (other == i || other >= graph_.nodes.size()) continue;
                        if (!graph_.nodes[other].statement) continue;
                        drag_with_.emplace_back(other, std::pair<f32, f32>{graph_x_[other] - drag_at_x_,
                                                                          graph_y_[other] - drag_at_y_});
                    }
                }
            }
        }
    }

    // --- the document itself ----------------------------------------------------------------
    //
    // Not a node: it is not in the file and there is nothing in it to change. It is the thing every
    // answer is an answer TO, and drawing it is what turns a column of unconnected statements into
    // a picture of a document. Its name is bold, like the file's name in the header and like a
    // built-in in the library, because it is the same kind of fact: a name that is a property of
    // the thing rather than of anything that has happened to it.
    if (doc_shown_ && doc_box.x0 < canvas.x1 && doc_box.x1 > canvas.x0) {
        // It is not a node — there is nothing in it to change and no wire leaves it — but it IS a
        // box on the canvas, and a box on a canvas that cannot be moved is the one thing in the
        // picture a player cannot arrange. Asked for directly. Where it goes is written into the
        // document on its own line, so it travels with the file like every other position (D756).
        const bool over_doc = doc_box.holds(ui_.pointer_x(), ui_.pointer_y()) && over_canvas;
        if (over_doc && ui_.pressed_in(doc_box) && !ui_.any_menu_open()) {
            hit_a_node = true;
            dragging_doc_ = true;
            doc_drag_x_ = doc_x_;
            doc_drag_y_ = doc_y_;
            doc_grab_x_ = layout_x(ui_.pointer_x()) - doc_x_;
            doc_grab_y_ = layout_y(ui_.pointer_y()) - doc_y_;
        }
        if (dragging_doc_) {
            doc_drag_x_ = std::round(layout_x(ui_.pointer_x()) - doc_grab_x_);
            doc_drag_y_ = std::round(layout_y(ui_.pointer_y()) - doc_grab_y_);
            if (ui_.input().mouse_left_released) {
                dragging_doc_ = false;
                remember("move");
                place_clip_document(lines_, doc_drag_x_, doc_drag_y_);
                document_changed({});
                ui_.draw().pop_clip();
                return;
            }
        }
        ui_.draw().glass(doc_box, 1.0f);
        ui_.draw().ink(doc_box, over_doc ? 0.22f : 0.16f);
        ui_.draw().edge(doc_box, over_doc ? 0.65f : 0.45f, 1.0f);
        ui_.draw().push_clip(doc_box);
        const f32 cell = std::min(metrics.icon() * 0.8f, node_h * 0.55f);
        const f32 left = doc_box.x0 + metrics.px(3.0f);
        ui_.draw().icon(Rect{left, doc_box.mid_y() - cell * 0.5f, left + cell,
                             doc_box.mid_y() + cell * 0.5f},
                        icon_for_file(editing_), 0.8f);
        ui_.label(Rect{left + cell + metrics.px(3.0f), doc_box.y0, doc_box.x1, doc_box.y1},
                  shown_name(editing_), Align::Left, kBold, 0.9f);
        ui_.draw().pop_clip();
    }

    ui_.draw().pop_clip();

    // --- the drag, finished ---------------------------------------------------------------------
    if (dragging_node_ < graph_.nodes.size()) {
        // **Snapped while it is being dragged, not when it is dropped.** A box that floats freely
        // and then jumps on release is a box you aimed at one place and got another; snapping as it
        // moves means where it looks is where it lands, which is the whole point of a grid.
        drag_at_x_ = std::round(layout_x(ui_.pointer_x()) - drag_grab_x_);
        drag_at_y_ = std::round(layout_y(ui_.pointer_y()) - drag_grab_y_);
        if (ui_.input().mouse_left_released) {
            const u32 moved = dragging_node_;
            dragging_node_ = ClipGraph::kNone;
            // And it lands on a cell nothing is standing on. The layout settles this anyway when
            // the document is re-read, but the document would then say one thing and the screen
            // another — and the file is what travels.
            {
                std::unordered_set<u64> taken;
                for (usize i = 0; i < graph_.nodes.size(); ++i) {
                    if (!node_shown_[i] || i == moved) continue;
                    bool coming_along = false;
                    for (const auto& along : drag_with_) {
                        if (along.first == i) coming_along = true;
                    }
                    if (coming_along) continue;
                    taken.insert(cell_key(static_cast<i32>(std::lround(graph_x_[i])),
                                          static_cast<i32>(std::lround(graph_y_[i]))));
                }
                take_free_cell(taken, drag_at_x_, drag_at_y_);
                for (auto& along : drag_with_) {
                    f32 x = drag_at_x_ + along.second.first;
                    f32 y = drag_at_y_ + along.second.second;
                    take_free_cell(taken, x, y);
                    along.second.first = x - drag_at_x_;
                    along.second.second = y - drag_at_y_;
                }
            }
            // Written ONCE, on the release. Writing it every frame of a drag would rewrite the line
            // sixty times a second and re-read the document with it, which is a millisecond a frame
            // spent on a number nobody has finished choosing.
            remember("move");
            bool wrote = place_clip_node(lines_, graph_.nodes[moved], drag_at_x_, drag_at_y_);
            for (const auto& along : drag_with_) {
                if (along.first >= graph_.nodes.size()) continue;
                wrote |= place_clip_node(lines_, graph_.nodes[along.first],
                                         drag_at_x_ + along.second.first,
                                         drag_at_y_ + along.second.second);
            }
            drag_with_.clear();
            if (wrote) document_changed({});
        }
    } else {
        dragging_node_ = ClipGraph::kNone;
    }

    // --- the wire, finished ---------------------------------------------------------------------
    if (wiring_from_ < graph_.nodes.size() && ui_.input().mouse_left_released) {
        const u32 from = wiring_from_;
        wiring_from_ = ClipGraph::kNone;
        if (dropped_on < graph_.nodes.size()) {
            remember("wire");
            document_changed(connect_clip_nodes(lines_, graph_, from, dropped_on));
            return;
        }
    }
    if (!ui_.input().mouse_left) wiring_from_ = ClipGraph::kNone;

    // --- a box drawn over the canvas, which is what chooses several ---------------------------
    //
    // Explorer's gesture, and the library's (D446), for the third time and for the same reason: an
    // interface that spends its novelty budget on *selecting things* has spent it in the worst
    // possible place. The band may only START where a press did not land on a node, on a tab or on
    // a wire being drawn — after that it holds the pointer itself.
    const bool may_band = !hit_a_node && dragging_node_ >= graph_.nodes.size() &&
                          wiring_from_ >= graph_.nodes.size() && !ui_.any_menu_open() &&
                          !dragging_graph_;
    Rect band{};
    bool band_done = false;
    if (may_band && ui_.band(id_of("editor.graph.band"), canvas, band, band_done)) {
        // And the canvas comes along when the band is pulled past its edge, for the same reason
        // the script view's selection does.
        if (!band_done) pan_at_edge(canvas, graph_pan_x_, graph_pan_y_, 620.0f);
        std::vector<u32> inside;
        for (usize i = 0; i < graph_.nodes.size(); ++i) {
            if (!node_shown_[i]) continue;
            const Rect box = box_of(i);
            if (box.x1 < band.x0 || box.x0 > band.x1 || box.y1 < band.y0 || box.y0 > band.y1) {
                continue;
            }
            inside.push_back(static_cast<u32>(i));
        }
        // Recomputed from the box every frame rather than accumulated, so dragging the band back
        // over a node takes it out again. An accumulating band can only ever choose more.
        choose_many(inside);
        if (band_done) {
            if (chosen_set_.empty()) {
                ui_.close_menu();
            } else {
                open_settings();
                ui_.sound().say(Cue::Step);
            }
        }
    }

    if (over_canvas && !hit_a_node && !ui_.any_menu_open() && ui_.right_pressed_in(canvas)) {
        menu_about_ = ClipGraph::kNone;
        palette_group_ = -1;
        menu_x_ = layout_x(ui_.pointer_x());
        menu_y_ = layout_y(ui_.pointer_y());
        ui_.open_menu(id_of("editor.graph.menu"), ui_.pointer_x(), ui_.pointer_y());
    }

    draw_part_naming(canvas);
    draw_graph_menu(canvas);
}

// Everything the graph can do, at the pointer. The same rule as the library's (D482, D488): a
// right-click is the way to reach an operation and there is no toolbar, because two ways to do one
// thing is two places for it to be wrong.
//
// The palette is TWO steps — the kinds of thing, then the things — because the language has ninety
// words in it and a menu of ninety is a menu nobody reads. Choosing a kind re-opens the menu where
// it already is, so it reads as one menu that went deeper rather than as two that happened.
// Every clip, or every material, a player could put in this document: what is already beside it,
// what is on their own shelf, and — for clips — what the game ships. Taken once, when the menu
// opens, because a listing re-read every frame is a menu whose third item is a different file by
// the time it is pressed (D488).
void Shell::gather_parts(u32 kind) {
    part_choices_.clear();
    const bool clips = kind == 1;
    std::vector<std::filesystem::path> where;
    // Beside the document first when there IS one — a part beside a world is still a part — and
    // then the shelves. With nothing open this is the new-world menu asking, and the shelves are
    // the whole of the answer.
    if (!editing_.empty()) where.push_back(editing_.parent_path());
    where.push_back(library_.root() / (clips ? "clips" : "materials"));
    if (clips) where.push_back(library_.shipped_root() / "clips");

    std::vector<std::string> seen;
    for (const std::filesystem::path& folder : where) {
        std::error_code error;
        if (!std::filesystem::exists(folder, error) || error) continue;
        std::vector<std::filesystem::path> here;
        for (const std::filesystem::directory_entry& entry :
             std::filesystem::directory_iterator(folder, error)) {
            if (error) break;
            if (!entry.is_regular_file()) continue;
            const std::string extension = entry.path().extension().string();
            const bool wanted = clips ? (extension == ".clip" || extension == ".wsclip")
                                      : (extension == ".wsmat" || extension == ".mat");
            if (!wanted) continue;
            // A file cannot include itself.
            if (!editing_.empty() && same_file(entry.path()) == editing_) continue;
            here.push_back(entry.path());
        }
        // By name, so the listing is the same on every machine rather than whatever order the
        // file system happens to hand back.
        std::sort(here.begin(), here.end(), [](const std::filesystem::path& a,
                                               const std::filesystem::path& b) {
            return a.filename().string() < b.filename().string();
        });
        for (const std::filesystem::path& one : here) {
            const std::string name = one.filename().string();
            if (std::find(seen.begin(), seen.end(), name) != seen.end()) continue;
            seen.push_back(name);
            part_choices_.push_back(one);
            if (part_choices_.size() >= 40) return;   // a menu, not a library
        }
    }
}

// A part off a shelf, named by the document.
//
// **D783 reversed.** It used to be COPIED beside the document, on the reasoning that an include
// resolves beside the file and then in the game's clips and nowhere else, so a world naming the
// player's own shelf would open on their machine and on nobody else's. That reasoning is still
// true about SENDING and was wrong about everything else: it meant a clip made in the editor was
// written into the worlds folder and appeared in no library at all — reported as *newly created
// materials on the node editor or clips dont show on your material or clips library*.
//
// So the shelves are where an include is looked for now (`ui::where_includes_live`), one copy of a
// thing exists, and it lives where that kind of thing lives. Gathering a world's parts beside it is
// what PACKAGING is for, and packaging is the thing to build when sending exists.
std::string Shell::take_part(const std::filesystem::path& from) {
    if (editing_.empty()) return "nothing is open";
    remember("part");
    const std::string why =
        add_clip_include(lines_, graph_, from.filename().string(), menu_x_, menu_y_);
    document_changed(why);
    return why;
}

// A new part, made beside the document, carrying the player's own author tag.
std::string Shell::make_part(u32 kind, const std::string& name) {
    if (editing_.empty()) return "nothing is open";
    std::string stem;
    for (char c : name) {
        // A file name, not a sentence: letters, digits, dashes and underscores, and a space becomes
        // an underscore. An include names this file in a quoted string that the whole language then
        // has to survive, and a name with a quotation mark in it is not one.
        if (c == ' ') {
            stem += '_';
        } else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                   c == '_' || c == '-') {
            stem += c;
        }
    }
    if (stem.empty()) return "give it a name first";
    const bool clips = kind == 1;
    // On its OWN shelf, which is where that kind of thing lives and where a player will look for it
    // afterwards. The document names it and `ui::where_includes_live` is what finds it.
    const std::string file = stem + (clips ? ".wsclip" : ".wsmat");
    const std::filesystem::path shelf = library_.root() / (clips ? "clips" : "materials");
    std::error_code error;
    std::filesystem::create_directories(shelf, error);
    error.clear();
    const std::filesystem::path at = shelf / file;
    if (std::filesystem::exists(at, error) && !error) {
        return "there is already a " + file + " on your " + (clips ? "clips" : "materials") +
               " shelf";
    }
    // A new part is not an empty file. An empty clip builds to nothing, and a document that
    // includes nothing is indistinguishable from a document that failed to load — the failure this
    // project has chased three times.
    const std::string body =
        clips ? ("# " + stem +
                 "\nlet " + stem + "_shape = box -1 0 -1   1 1 1  round=0.02\nsolid " + stem +
                 "_shape\n")
              // **The bare minimum that works**, asked for directly. A name and a colour is a
              // voxel type; everything else it can take is offered on the node with its usual
              // value beside it (D786), so writing them all out was a file full of defaults
              // pretending to be decisions.
              : ("# " + stem + "\nmaterial " + stem + " rgb=180,178,172\n");
    {
        std::ofstream out(at, std::ios::binary | std::ios::trunc);
        if (!out) return "could not make " + file + " beside this document";
        out.write(body.data(), static_cast<std::streamsize>(body.size()));
    }
    for (const Kind& of : shipped_kinds()) {
        if (of.folder == (clips ? "clips" : "materials")) {
            write_author(at, of, preferences_.username);
            break;
        }
    }
    remember("part");
    library_.refresh(true);
    const std::string why = add_clip_include(lines_, graph_, file, menu_x_, menu_y_);
    document_changed(why);
    return why;
}

// And the one row that asks what it is called, before it exists.
//
// D773 says a new thing asks for its name at once, on the row it was made on. There is no row here,
// so it asks in the place the menu was standing — and it asks BEFORE the file is made, because a
// file made first is a file called `untitled` if the player changes their mind.
void Shell::draw_part_naming(const Rect& canvas) {
    if (making_ == 0) return;
    const Metrics& metrics = ui_.metrics();
    const f32 wide = std::min(canvas.width() - metrics.px(16.0f), metrics.px(260.0f));
    const Rect row{canvas.x0 + metrics.px(8.0f), canvas.y0 + metrics.px(8.0f),
                   canvas.x0 + metrics.px(8.0f) + wide,
                   canvas.y0 + metrics.px(8.0f) + metrics.row()};
    ui_.panel(Rect{row.x0 - metrics.px(4.0f), row.y0 - metrics.px(4.0f), row.x1 + metrics.px(4.0f),
                   row.y1 + metrics.px(4.0f)});
    if (making_focus_) {
        making_focus_ = false;
        ui_.type_into(id_of("editor.graph.naming"), std::string());
    }
    if (ui_.field(id_of("editor.graph.naming"), row, making_buffer_,
                  making_ == 1 ? "what is the clip called?" : "what is the material called?")) {
        const u32 kind = making_;
        making_ = 0;
        const std::string why = make_part(kind, making_buffer_);
        making_buffer_.clear();
        if (why.empty()) {
            ui_.sound().say(Cue::Commit);
        } else {
            say(why, 3.5);
            ui_.sound().say(Cue::Refuse);
        }
        return;
    }
    if (ui_.input().was_pressed(Key::Escape)) {
        making_ = 0;
        making_buffer_.clear();
        ui_.sound().say(Cue::Close);
    }
}

void Shell::draw_graph_menu(const Rect& canvas) {
    const u64 id = id_of("editor.graph.menu");
    if (!ui_.menu_open(id)) return;
    (void)canvas;

    enum class Act : u32 { Script, CutWires, Duplicate, TakeOut, Group, Head, Part, NewPart,
                          TakePart };
    std::vector<Act> acts;
    std::vector<i32> args;   // which group, which head, which kind — so prepending an item is safe
    std::vector<Ui::MenuItem> items;
    std::vector<std::string> labels;

    // The two levels a part menu has. Negative, because the palette's own groups are indexed from
    // nought and these are not one of them.
    constexpr i32 kPartClips = -2;
    constexpr i32 kPartMaterials = -3;

    const bool about_a_node = menu_about_ < graph_.nodes.size();
    // What the menu is ABOUT, fixed the moment it opens: reading the choice every frame is how a
    // menu comes to act on something else by the time an item is pressed (D488, the same fault one
    // window along).
    const std::vector<u32> about = about_a_node ? chosen_set_ : std::vector<u32>{};
    const std::string many = (about.size() > 1) ? (" " + std::to_string(about.size())) : "";

    if (about_a_node) {
        bool any_wires = false;
        bool any_statement = false;
        for (u32 node : about) {
            if (node >= graph_.nodes.size()) continue;
            if (!graph_.nodes[node].inputs.empty()) any_wires = true;
            if (graph_.nodes[node].statement) any_statement = true;
        }
        labels.push_back("show in the script");
        items.push_back({Icon::Editor, labels.back(), about.size() == 1});
        acts.push_back(Act::Script);
        args.push_back(0);
        labels.push_back("duplicate" + many);
        items.push_back({Icon::Duplicate, labels.back(), any_statement});
        acts.push_back(Act::Duplicate);
        args.push_back(0);
        labels.push_back("cut every wire");
        items.push_back({Icon::Close, labels.back(), any_wires});
        acts.push_back(Act::CutWires);
        args.push_back(0);
        labels.push_back("take out" + many);
        items.push_back({Icon::Delete, labels.back(), any_statement});
        acts.push_back(Act::TakeOut);
        args.push_back(0);
    } else if (palette_group_ == kPartClips || palette_group_ == kPartMaterials) {
        const bool clips = palette_group_ == kPartClips;
        labels.push_back(clips ? "a new clip" : "a new material");
        items.push_back({Icon::New, labels.back(), true});
        acts.push_back(Act::NewPart);
        args.push_back(clips ? 1 : 2);
        for (const std::filesystem::path& one : part_choices_) {
            labels.push_back(shown_name(one));
            // Everything but a built-in can be taken off the shelf from here. What came with the
            // game is not the player's to lose, and a delete that came back on the next install
            // would look like the delete had failed (D494).
            items.push_back({clips ? Icon::Clip : Icon::Material, labels.back(), true,
                             !library_.is_shipped(one)});
            acts.push_back(Act::TakePart);
            args.push_back(static_cast<i32>(&one - part_choices_.data()));
        }
    } else if (palette_group_ < 0) {
        // A whole clip and a whole material come FIRST, above the words of the language: they are
        // the two biggest things a document can be made of, and the palette's own groups are the
        // small ones.
        labels.push_back("a clip");
        items.push_back({Icon::Clip, labels.back(), true});
        acts.push_back(Act::Part);
        args.push_back(1);
        labels.push_back("a material");
        items.push_back({Icon::Material, labels.back(), true});
        acts.push_back(Act::Part);
        args.push_back(2);
        i32 which = 0;
        for (const ClipPaletteGroup& group : clip_palette()) {
            labels.push_back(group.name);
            items.push_back({Icon::New, labels.back(), true});
            acts.push_back(Act::Group);
            args.push_back(which++);
        }
    } else {
        const std::vector<ClipPaletteGroup>& groups = clip_palette();
        const usize which = static_cast<usize>(palette_group_);
        if (which < groups.size()) {
            i32 head_at = 0;
            for (const std::string& head : groups[which].heads) {
                labels.push_back(clip_head_shown(head));
                items.push_back({Icon::New, labels.back(), true});
                acts.push_back(Act::Head);
                args.push_back(head_at++);
            }
        }
    }
    for (usize i = 0; i < items.size(); ++i) items[i].label = labels[i];

    i32 removed = -1;
    const i32 picked = ui_.menu(id, items.data(), static_cast<u32>(items.size()), removed);
    if (removed >= 0 && static_cast<usize>(removed) < args.size() &&
        acts[static_cast<usize>(removed)] == Act::TakePart) {
        const usize which = static_cast<usize>(args[static_cast<usize>(removed)]);
        palette_group_ = -1;
        if (which >= part_choices_.size()) return;
        const std::filesystem::path going = part_choices_[which];
        std::error_code error;
        if (library_.is_shipped(going)) {
            say("that one came with the game -- it is not yours to lose", 3.0);
            ui_.sound().say(Cue::Refuse);
            return;
        }
        // The one already open is not deleted out from under itself; everything about a document
        // that is gone comes down in one place (`close_document`), and doing it in the wrong order
        // leaves a tab editing a file nobody can open.
        if (!editing_.empty() && same_file(going) == editing_) {
            say("that one is open here -- close it first", 3.0);
            ui_.sound().say(Cue::Refuse);
            return;
        }
        if (!std::filesystem::remove(going, error) || error) {
            say("could not delete " + going.filename().string(), 3.0);
            ui_.sound().say(Cue::Refuse);
            return;
        }
        library_.refresh(true);
        say("deleted " + shown_name(going), 2.5);
        ui_.sound().say(Cue::Close);
        return;
    }
    if (picked < 0 || static_cast<usize>(picked) >= acts.size()) return;
    const usize at = static_cast<usize>(picked);

    switch (acts[at]) {
        case Act::Script: {
            menu_about_ = ClipGraph::kNone;
            if (about.empty() || about.front() >= graph_.nodes.size()) return;
            const ClipNode& node = graph_.nodes[about.front()];
            caret_line_ = (node.line > 0) ? node.line - 1 : 0;
            caret_column_ = 0;
            caret_moved_ = true;
            view_ = 0;
            ui_.sound().say(Cue::Open);
            return;
        }
        case Act::Duplicate: {
            menu_about_ = ClipGraph::kNone;
            std::vector<std::string> made;
            remember("duplicate");
            const std::string why = duplicate_clip_nodes(lines_, graph_, about, made);
            document_changed(why);
            if (!why.empty()) return;
            // The copies are what is chosen now, so the next thing a hand does — drag them
            // somewhere, change a number — is about the thing that was just made.
            std::vector<u32> fresh;
            for (const std::string& name : made) {
                for (usize i = 0; i < graph_.nodes.size(); ++i) {
                    if (graph_.nodes[i].name == name) fresh.push_back(static_cast<u32>(i));
                }
            }
            choose_many(fresh);
            say(made.size() == 1 ? ("copied to " + made.front())
                                 : ("copied " + std::to_string(made.size())),
                2.5);
            return;
        }
        case Act::CutWires: {
            menu_about_ = ClipGraph::kNone;
            // Backwards through each node's own wires, so taking one out cannot move the ones still
            // to go; and the nodes themselves in any order, because a wire is written inside the
            // thing that reads it and no two of them share a line.
            std::string why;
            bool cut = false;
            remember("wire");
            for (u32 node : about) {
                if (node >= graph_.nodes.size()) continue;
                for (usize k = graph_.nodes[node].links.size(); k-- > 0;) {
                    const std::string one =
                        disconnect_clip_node(lines_, graph_, node, static_cast<u32>(k));
                    if (one.empty()) {
                        cut = true;
                    } else if (why.empty()) {
                        why = one;
                    }
                }
            }
            if (cut) {
                dirty_ = true;
                graph_stale_ = true;
                refresh_graph();
                reparse_soon();
                ui_.sound().say(Cue::Commit);
            }
            if (!why.empty()) say(why, 3.0);
            return;
        }
        case Act::TakeOut:
            menu_about_ = ClipGraph::kNone;
            remember("take out");
            document_changed(delete_clip_nodes(lines_, graph_, about));
            return;
        case Act::Group:
            // A kind was chosen: the same menu, one level in, where it already is.
            palette_group_ = args[at];
            ui_.open_menu(id, ui_.pointer_x(), ui_.pointer_y());
            return;
        case Act::Part:
            // A clip or a material: the shelf's listing, taken once, in the same menu one level in.
            gather_parts(static_cast<u32>(args[at]));
            palette_group_ = (args[at] == 1) ? kPartClips : kPartMaterials;
            ui_.open_menu(id, ui_.pointer_x(), ui_.pointer_y());
            return;
        case Act::NewPart:
            palette_group_ = -1;
            making_ = static_cast<u32>(args[at]);
            making_buffer_.clear();
            making_focus_ = true;
            ui_.sound().say(Cue::Open);
            return;
        case Act::TakePart: {
            palette_group_ = -1;
            const usize which = static_cast<usize>(args[at]);
            if (which >= part_choices_.size()) return;
            const std::string why = take_part(part_choices_[which]);
            if (why.empty()) {
                ui_.sound().say(Cue::Commit);
            } else {
                say(why, 3.0);
                ui_.sound().say(Cue::Refuse);
            }
            return;
        }
        case Act::Head: {
            const std::vector<ClipPaletteGroup>& groups = clip_palette();
            const usize which = static_cast<usize>(palette_group_);
            const usize head = static_cast<usize>(args[at]);
            palette_group_ = -1;
            if (which >= groups.size() || head >= groups[which].heads.size()) return;
            // Through the same one path a scripted run takes, so the thing a photograph proves is
            // the thing a press does.
            if (add_node(groups[which].heads[head]).empty()) open_settings();
            return;
        }
    }
}


// What to call the file on screen.
//
// The stem alone, until the stem is not enough: a building's parts are `facility/doors.clip` and a
// player's own copy is `doors.wsclip`, and a header that says `doors` for both is a header that
// cannot answer *which one am I editing*. So the folder comes with it whenever the file is not
// directly on a shelf — which is the case the two are told apart in, and the only one that needs
// the extra word.
std::string Shell::shown_name(const std::filesystem::path& path) const {
    const std::string stem = path.stem().string();
    const std::filesystem::path folder = path.parent_path();
    if (folder.empty()) return stem;
    const std::string in = folder.filename().string();
    // A shelf's own name is not worth saying: `clips/sampler` is `sampler`.
    for (const Kind& kind : shipped_kinds()) {
        if (in == kind.folder) return stem;
    }
    if (in.empty()) return stem;
    return in + "/" + stem;
}

// A node's parameters, on the left, while its node is selected.
//
// `23-shell-and-libraries.md` §5c: *every node parameter is a slider by §3, with the same
// double-click-to-type and the same lack of a cap. A node's parameters are a parameters window:
// they open on the left while its node is selected, which is why the two families exist.*
// One property of one node as a slider, wherever it is drawn.
//
// The panel on the left and a box opened on the canvas are the same rows in two places, so they
// are the same code in one place. `area` is where the rows start and how wide they are; anything
// falling outside `clip_to` is skipped rather than drawn, which is what makes a scrolled panel and
// a box scrolled off the edge of the graph cost the same as each other: nothing.
// What it IS, and what it could be instead.
//
// The word was drawn and nothing else — `all` is a union and `grain` is an fbm, and that row is
// where that is said. Now it is also where it is CHANGED: a shape can become another shape, a
// pattern another pattern, a clip another clip. What a node can become is its own palette group
// (`clip_heads_like`), because that is already the answer to *which of these words are alike*.
//
// A node with nothing to become is still drawn, in the same place, as plain text. A control that
// appears and disappears as the selection moves is a row a player has to read before using.
void Shell::draw_head_choice(const Rect& row, const ClipNode& node) {
    const Metrics& metrics = ui_.metrics();
    const std::string shown = clip_head_shown(node.head);
    const bool is_door = node.head == "include" && !node.target.empty();
    const std::vector<std::string>& alike = clip_heads_like(node.head);
    const bool can_swap = is_door || alike.size() > 1;

    const f32 wide = DrawList::measure(shown, metrics.text()) + metrics.px(14.0f);
    const Rect cell{std::max(row.x0, row.x1 - wide), row.y0, row.x1, row.y1};
    const u64 id = id_of("node.head");
    const bool over = can_swap && cell.holds(ui_.pointer_x(), ui_.pointer_y());
    if (over) ui_.draw().ink(cell, 0.14f);
    ui_.draw().text(cell.x1 - (can_swap ? metrics.px(10.0f) : 0.0f),
                    cell.mid_y() - DrawList::cap_height(metrics.text()) * 0.5f, shown,
                    metrics.text(), tinted(kPlain, clip_carries_tint(node.carries) + 1),
                    Align::Right, over ? 1.0f : 0.9f);
    if (!can_swap) return;
    // A chevron, so a word that can be changed does not look like a word that cannot.
    const f32 mark = std::min(metrics.icon() * 0.6f, cell.height() * 0.6f);
    ui_.draw().icon(Rect{cell.x1 - mark, cell.mid_y() - mark * 0.5f, cell.x1,
                         cell.mid_y() + mark * 0.5f},
                    Icon::Expanded, over ? 0.9f : 0.45f);
    if (over && ui_.pressed_in(cell) && !ui_.any_menu_open()) {
        if (is_door) gather_parts(1);
        head_menu_door_ = is_door;
        ui_.open_menu(id, cell.x0, cell.y1);
    }
    if (!ui_.menu_open(id)) return;

    std::vector<Ui::MenuItem> items;
    std::vector<std::string> labels;
    if (head_menu_door_) {
        for (const std::filesystem::path& one : part_choices_) {
            labels.push_back(shown_name(one));
            items.push_back({Icon::Clip, labels.back(), true});
        }
    } else {
        for (const std::string& one : alike) {
            labels.push_back(clip_head_shown(one));
            items.push_back({Icon::New, labels.back(), one != node.head});
        }
    }
    for (usize i = 0; i < items.size(); ++i) items[i].label = labels[i];

    const i32 picked = ui_.menu(id, items.data(), static_cast<u32>(items.size()));
    if (picked < 0) return;
    const usize at = static_cast<usize>(picked);
    remember("kind");
    bool wrote = false;
    if (head_menu_door_) {
        if (at < part_choices_.size()) {
            wrote = write_clip_target(lines_, node, part_choices_[at].filename().string());
        }
    } else if (at < alike.size()) {
        wrote = write_clip_head(lines_, node, alike[at]);
    }
    if (wrote) {
        document_changed({});
    } else {
        say("that one cannot become this", 2.5);
        ui_.sound().say(Cue::Refuse);
    }
}

usize Shell::draw_shape_extras(const ClipNode& node, const Rect& area, const Rect& clip_to,
                               f32 row_height) {
    const Metrics& metrics = ui_.metrics();
    if (!node.statement || node.carries != ClipCarries::Shape) return 0;
    if (chosen_index_ >= graph_.nodes.size()) return 0;

    const u64 fold = id_of("node.fold", id_of("shape"));
    const u32 skin = clip_wrapper_of(graph_, chosen_index_, "shell");
    const u32 stretched = clip_wrapper_of(graph_, chosen_index_, "scale");
    const bool changed = skin != ClipGraph::kNone || stretched != ClipGraph::kNone;

    f32 y = area.y0;
    usize drawn = 0;
    const Rect head_row{area.x0, y, area.x1, y + std::min(row_height, metrics.row())};
    y += row_height;
    ++drawn;
    const Ui::Fold state = (head_row.y1 >= clip_to.y0 && head_row.y0 <= clip_to.y1)
                               ? ui_.section(fold, head_row, "shape", changed)
                               : Ui::Fold{ui_.section_open(fold), false};
    if (state.reset) {
        remember("shape");
        bool touched = false;
        if (skin != ClipGraph::kNone) touched |= unwrap_clip_node(lines_, graph_, skin);
        refresh_graph();
        const u32 again = clip_wrapper_of(graph_, chosen_index_, "scale");
        if (again != ClipGraph::kNone) touched |= unwrap_clip_node(lines_, graph_, again);
        if (touched) document_changed({});
        return drawn;
    }
    if (!state.open) return drawn;

    const f32 gutter = metrics.icon() + metrics.px(2.0f);
    // `hollow`, then `stretch` along each axis. Four rows, each holding one number of one wrapper.
    struct Row {
        const char* label;
        const char* key;      // the key on the wrapper that carries it
        const char* head;     // the word the language wraps with
        f64 unwrapped;        // what it means when there is no wrapper at all
        f64 low;
        f64 high;
        const char* about;
    };
    static const Row kRows[4] = {
        {"hollow", "thickness", "shell", 0.0, 0.0, 1.0,
         "How thick a skin to leave. Nought is solid all the way through"},
        {"stretch x", "x", "scale", 1.0, 0.05, 8.0, "How far it is stretched east to west"},
        {"stretch y", "y", "scale", 1.0, 0.05, 8.0, "And up and down"},
        {"stretch z", "z", "scale", 1.0, 0.05, 8.0, "And north to south"},
    };

    for (u32 i = 0; i < 4; ++i) {
        const Row& want = kRows[i];
        const u32 on = (std::string(want.head) == "shell") ? skin : stretched;
        const ClipNumber* written =
            (on != ClipGraph::kNone) ? graph_.nodes[on].number(want.key) : nullptr;
        const f64 now = (written != nullptr) ? written->value : want.unwrapped;

        const Rect whole{area.x0, y, area.x1, y + std::min(row_height, metrics.row())};
        const Rect row{whole.x0 + gutter, whole.y0, whole.x1, whole.y1};
        y += row_height;
        ++drawn;
        if (whole.y1 < clip_to.y0 || whole.y0 > clip_to.y1) continue;

        {
            const Rect back{whole.x0, whole.y0, whole.x0 + gutter, whole.y1};
            const bool off_default = std::abs(now - want.unwrapped) > 1e-9;
            if (ui_.reset_button(id_of("node.shape.back", i), back, off_default,
                                 "Put this back")) {
                remember("shape");
                bool touched = false;
                if (on != ClipGraph::kNone) {
                    // Back to its default is back to NOT BEING THERE, when nothing else on the
                    // wrapper is doing anything either — otherwise the document keeps a `scale`
                    // that scales by one, which is a word that does nothing.
                    touched = write_clip_key(lines_, graph_.nodes[on], want.key,
                                             spell_clip_number(want.unwrapped, "0.00"));
                }
                if (touched) document_changed({});
                return drawn;
            }
        }

        const std::string tooltip = want.about;
        Number about;
        about.label = want.label;
        about.tooltip = tooltip;
        about.low = want.low;
        about.high = want.high;
        about.step = 0.01;
        about.decimals = 2;
        f64 value = now;
        if (!ui_.number(id_of("node.shape", i), row, about, value) || value == now) continue;

        remember("shape");
        if (on != ClipGraph::kNone) {
            if (written != nullptr) {
                if (write_clip_number(lines_, *written, spell_clip_number(value, written->text))) {
                    document_changed({});
                }
            } else if (write_clip_key(lines_, graph_.nodes[on], want.key,
                                      spell_clip_number(value, "0.00"))) {
                document_changed({});
            }
            return drawn;
        }
        // Nothing wrapping it yet, so the wrapper goes on — written with the key this row carries
        // and nothing else, because a `scale` that also says `y=1 z=1` is two words that do nothing
        // beside one that does.
        const std::string args =
            std::string(want.key) + "=" + spell_clip_number(value, "0.00");
        if (wrap_clip_node(lines_, graph_, chosen_index_, want.head, args)) document_changed({});
        return drawn;
    }
    return drawn;
}

usize Shell::property_rows_of(const ClipNode& node, bool folding) const {
    const std::vector<ClipProperty>& offers = clip_properties_of(node.head);
    usize rows = 0;
    std::string group;
    for (const ClipProperty& offer : offers) {
        if (folding && offer.group != group) {
            group = offer.group;
            ++rows;   // the heading
            if (!ui_.section_open(id_of("node.fold", id_of(group)))) continue;
        }
        if (folding && !ui_.section_open(id_of("node.fold", id_of(group)))) {
            continue;
        }
        rows += offer.parts;
    }
    return rows;
}

usize Shell::draw_property_rows(const ClipNode& node, const Rect& area, const Rect& clip_to,
                                f32 row_height, u64 salt, bool folding) {
    const Metrics& metrics = ui_.metrics();
    const std::vector<ClipProperty>& offers = clip_properties_of(node.head);
    const Rect inner = area;
    const Rect& list = clip_to;
    f32 y = area.y0;
    usize row_at = 0;
    usize drawn = 0;
    std::string group;
    bool showing = true;

    // The gutter every row's *put this back* lives in, down the left, reserved whether or not any
    // row has one — a word that shifts sideways depending on whether the row above it is at its
    // default is a word that never sits in the same place twice.
    const f32 gutter = folding ? metrics.icon() + metrics.px(2.0f) : 0.0f;

    for (const ClipProperty& offer : offers) {
        if (folding && offer.group != group) {
            group = offer.group;
            const u64 fold = id_of("node.fold", id_of(group));
            // A heading says whether anything under it has been changed, so a player can find a
            // changed value without opening every fold to look for it.
            bool any_written = false;
            for (const ClipProperty& under : offers) {
                if (under.group != group) continue;
                for (const ClipNumber& number : node.numbers) {
                    if (number.key == under.key) any_written = true;
                }
            }
            const Rect head_row{inner.x0, y, inner.x1, y + std::min(row_height, metrics.row())};
            y += row_height;
            ++drawn;
            const Ui::Fold state = (head_row.y1 >= list.y0 && head_row.y0 <= list.y1)
                                       ? ui_.section(fold, head_row, group, any_written)
                                       : Ui::Fold{ui_.section_open(fold), false};
            showing = state.open;
            if (state.reset) {
                // Every key under this heading goes back to being unwritten at once.
                remember("value");
                bool touched = false;
                for (const ClipProperty& under : offers) {
                    if (under.group != group) continue;
                    touched |= erase_clip_key(lines_, node, under.key);
                }
                if (touched) document_changed({});
                return drawn;
            }
        }
        if (folding && !showing) continue;

        for (u32 part = 0; part < offer.parts; ++part) {
            const Rect whole{inner.x0, y, inner.x1, y + std::min(row_height, metrics.row())};
            const Rect row{whole.x0 + gutter, whole.y0, whole.x1, whole.y1};
            y += row_height;
            ++drawn;
            const usize mine = row_at++;
            if (whole.y1 < list.y0 || whole.y0 > list.y1) continue;

            // What the document says about this one, if it says anything.
            const ClipNumber* written = nullptr;
            for (const ClipNumber& number : node.numbers) {
                if (number.key == offer.key && number.index == part) written = &number;
            }

            // **Put it back**, which for a property whose default is a SILENCE means taking the key
            // out of the line. Drawn only when there is something to put back, because a control
            // that is always there and does nothing most of the time is furniture (D486) — and its
            // absence is also the answer to *is this one changed*.
            if (folding) {
                const Rect back{whole.x0, whole.y0, whole.x0 + gutter, whole.y1};
                if (ui_.reset_button(hash_combine(salt, mine * 2 + 1), back, written != nullptr,
                                     "Put this back to its usual value")) {
                    remember("value");
                    if (erase_clip_key(lines_, node, offer.key)) document_changed({});
                    return drawn;
                }
            }

            std::string label = offer.key;
            if (offer.parts == 3) {
                static const char* kChannel[3]{" red", " green", " blue"};
                label += kChannel[part];
            } else if (offer.parts > 1) {
                label += " " + std::to_string(part + 1);
            }
            // Both of these outlive the call because they are named locals: a `string_view`
            // onto a temporary is the bug this panel has already had once.
            const std::string tooltip =
                written != nullptr
                    ? (offer.about + ".  Written as " + written->text + " on line " +
                       std::to_string(written->line))
                    : (offer.about + ".  Not written: it takes " +
                       spell_clip_number(offer.fallback,
                                         offer.decimals > 0 ? "0.00" : "0") +
                       " until it is");
            Number about;
            about.label = label;
            about.tooltip = tooltip;
            about.low = offer.low;
            about.high = offer.high;
            about.step = std::pow(10.0, -static_cast<f64>(offer.decimals));
            about.decimals = static_cast<i32>(offer.decimals);

            f64 value = (written != nullptr) ? written->value : offer.fallback;
            const f64 was = value;
            if (!ui_.number(hash_combine(salt, mine * 2), row, about, value) || value == was) {
                continue;
            }
            if (written != nullptr) {
                // The bytes of one number, exactly as every other slider in this panel does.
                remember("value");
                if (write_clip_number(lines_, *written, spell_clip_number(value, written->text))) {
                    dirty_ = true;
                    graph_stale_ = true;
                    reparse_soon();
                }
                continue;
            }
            // Not written, so the key goes in — with every part of it, because `rgb=124` is
            // not a colour and a key half written is a key the reader cannot use.
            const std::string spelling = offer.decimals > 0 ? "0.00" : "0";
            std::string text;
            for (u32 other = 0; other < offer.parts; ++other) {
                const ClipNumber* had = nullptr;
                for (const ClipNumber& number : node.numbers) {
                    if (number.key == offer.key && number.index == other) had = &number;
                }
                const f64 each = (other == part) ? value
                                                 : (had != nullptr ? had->value
                                                                   : offer.fallback);
                if (other > 0) text += ",";
                text += spell_clip_number(each, had != nullptr ? had->text : spelling);
            }
            remember("value");
            if (write_clip_key(lines_, node, offer.key, text)) {
                dirty_ = true;
                graph_stale_ = true;
                reparse_soon();
            }
        }
    }
    return drawn;
}

void Shell::draw_node_parameters(const Rect& rect) {
    const Metrics& metrics = ui_.metrics();
    if (chosen_index_ >= graph_.nodes.size()) return;
    const ClipNode node = graph_.nodes[chosen_index_];   // by value: the graph is re-read under it

    ui_.panel(rect);
    draw_header(rect, window_settings_, icon_of(node),
                node.name.empty() ? clip_head_shown(node.head) : node.name);

    Column column;
    column.box = Rect{rect.x0 + metrics.px(8.0f), rect.y0 + metrics.row() + metrics.px(6.0f),
                      rect.x1 - metrics.px(8.0f), rect.y1 - metrics.px(6.0f)};
    column.y = column.box.y0;
    column.row = metrics.row();
    column.gap = metrics.px(4.0f);

    {
        const Rect row = column.next();
        const Rect back{row.x0, row.y0, row.x0 + metrics.px(96.0f), row.y1};
        if (ui_.button(id_of("node.back"), back, Icon::Up, "settings",
                       "Stop looking at this node")) {
            choose(ClipGraph::kNone);
            return;
        }
        // What it is, in the language's own word and in the colour that word has everywhere else:
        // `all` is a union and `grain` is an fbm, and this row is where that is said.
        draw_head_choice(row, node);
    }

    if (node.opaque) {
        // D454: this is source nothing could read, carried whole rather than dropped. The panel
        // says so and shows it; the script view is where it is edited.
        const Rect where{column.box.x0, column.y, column.box.x1, column.box.y1};
        ui_.markdown(where,
                     "This is text the graph could not read, kept exactly as it is written. "
                     "The **script** view is where it is edited.\n");
        return;
    }

    // The ranges, worked out once when the node is chosen. A range recomputed from the value every
    // frame puts the handle back in the middle of its travel on every frame of a drag, which reads
    // as a slider that cannot be moved.
    if (range_node_ != chosen_node_ || node_range_.size() != node.numbers.size()) {
        range_node_ = chosen_node_;
        node_range_.clear();
        for (const ClipNumber& number : node.numbers) {
            const f64 span = std::max(1.0, std::abs(number.value));
            const f64 step = std::pow(10.0, -static_cast<f64>(clip_number_decimals(number.text)));
            const f64 low = std::floor((number.value - span) / step) * step;
            node_range_.emplace_back(low, low + span * 2.0);
        }
    }

    // What it is made of, for a node that has no numbers of its own. A union IS its children, and a
    // panel that answered *nothing to change here* to the most common node in a clip would be a
    // panel a player stops opening. Listed by name, each a press that goes there — which turns the
    // one node with nothing to say into the table of contents for everything under it.
    std::vector<u32> made_of;
    for (u32 input : node.inputs) {
        if (input < graph_.nodes.size()) made_of.push_back(input);
    }

    // --- everything this KIND of statement can be given ---------------------------------------
    //
    // A shape has the numbers it has and no others, so for a shape this is empty and the panel
    // lists what is written, as it always did. A material is the other case: the document writes
    // three of a dozen and the rest take their usual value silently, so a panel that lists what is
    // written is a panel that says a material has three properties. Reported directly.
    const std::vector<ClipProperty>& offers = clip_properties_of(node.head);
    // What is FOLDED AWAY takes no room, so the scroll's content is what will actually be drawn.
    const usize offered_rows = property_rows_of(node, true);

    // Two things the LANGUAGE has as operations and this panel now offers as properties: how hollow
    // it is and how far it is stretched. Only on something that makes matter — a pattern has no
    // skin and a number cannot be stretched.
    const bool has_shape_rows = node.statement && node.carries == ClipCarries::Shape;
    const usize shape_rows =
        has_shape_rows ? (1 + (ui_.section_open(id_of("node.fold", id_of("shape"))) ? 4 : 0)) : 0;

    const Rect list{column.box.x0, column.y, column.box.x1, column.box.y1};
    const f32 row_height = metrics.row() + metrics.px(4.0f);
    const f32 rows = static_cast<f32>((offers.empty() ? node.numbers.size() : offered_rows) +
                                      shape_rows + node.words.size() + made_of.size() + 2);
    const Rect inner = ui_.begin_scroll(id_of("node.scroll"), list, rows * row_height);
    f32 y = inner.y0;

    usize positional = 0;
    usize positional_count = 0;
    for (const ClipNumber& number : node.numbers) {
        if (number.key.empty()) ++positional_count;
    }

    if (!offers.empty()) {
        const usize laid = draw_property_rows(node, Rect{inner.x0, y, inner.x1, inner.y1}, list,
                                             row_height, id_of("node.panel"), true);
        y += static_cast<f32>(laid) * row_height;
    }
    if (has_shape_rows) {
        y += static_cast<f32>(draw_shape_extras(node, Rect{inner.x0, y, inner.x1, inner.y1}, list,
                                                row_height)) *
             row_height;
    }
    // The same gutter the offered rows use, down the left, so a panel of positions and a panel of
    // properties are the same panel.
    const f32 gutter = metrics.icon() + metrics.px(2.0f);
    for (usize i = 0; offers.empty() && i < node.numbers.size(); ++i) {
        const ClipNumber& number = node.numbers[i];
        const Rect whole{inner.x0, y, inner.x1, y + metrics.row()};
        const Rect row{whole.x0 + gutter, whole.y0, whole.x1, whole.y1};
        y += row_height;
        if (whole.y1 < list.y0 || whole.y0 > list.y1) {
            if (number.key.empty()) ++positional;
            continue;
        }

        // **Put it back**, and what *back* means for a number with no name is the number the
        // palette makes this word WITH. Nothing in the language says what a box's third number
        // ought to be — but the palette has to make a box somebody can see, and the value it makes
        // it with is exactly that answer. Drawn only when the two differ (D486).
        f64 made_with = 0.0;
        const bool has_default =
            clip_default_number(node.head, number.key, number.index, made_with);
        {
            const Rect back{whole.x0, whole.y0, whole.x0 + gutter, whole.y1};
            const bool changed = has_default && std::abs(made_with - number.value) > 1e-9;
            if (ui_.reset_button(id_of("node.number.back", i), back, changed,
                                 "Put this back to what a new one is made with")) {
                remember("value");
                if (write_clip_number(lines_, number,
                                      spell_clip_number(made_with, number.text))) {
                    document_changed({});
                }
                return;
            }
        }

        std::string label;
        if (number.key.empty()) {
            label = positional_name(node, positional, positional_count);
            ++positional;
        } else {
            label = number.key;
            // `rgb=124,120,112` is three rows, and each of them has to say which one it is.
            usize of_this_key = 0;
            for (const ClipNumber& other : node.numbers) {
                if (other.key == number.key) ++of_this_key;
            }
            if (of_this_key > 1) label += " " + std::to_string(number.index + 1);
        }

        // Both of these are string_views onto strings that have to outlive the call, which is what
        // these two locals are for.
        const std::string tooltip =
            "Written as " + number.text + " on line " + std::to_string(number.line);
        Number about;
        about.label = label;
        about.tooltip = tooltip;
        about.low = (i < node_range_.size()) ? node_range_[i].first : number.value - 1.0;
        about.high = (i < node_range_.size()) ? node_range_[i].second : number.value + 1.0;
        const u32 decimals = clip_number_decimals(number.text);
        // The document's own precision is the slider's step, so `sides=6` steps by one and
        // `round=0.04` by a hundredth. Nothing had to be told which: the file already said.
        about.step = std::pow(10.0, -static_cast<f64>(decimals));
        about.decimals = static_cast<i32>(decimals);

        f64 value = number.value;
        if (ui_.number(id_of("node.number", i), row, about, value) && value != number.value) {
            remember("value");
            // The one way a slider changes a document: the bytes of this number, and no others.
            // Everything else in the file — the comments, the blank lines, the author's own spacing
            // — is untouched, which is what makes the text view worth trusting after a visual edit.
            if (write_clip_number(lines_, number, spell_clip_number(value, number.text))) {
                dirty_ = true;
                graph_stale_ = true;
                reparse_soon();
            }
        }
    }

    for (usize w = 0; w < node.words.size(); ++w) {
        const ClipWord& word = node.words[w];
        const Rect row{inner.x0 + gutter, y, inner.x1, y + metrics.row()};
        y += row_height;
        if (row.y1 < list.y0 || row.y0 > list.y1) continue;
        // A value that is not a number is not a slider (D444): a choice is a choice and a name is a
        // name. What it gets instead is a row saying what it is — and, where it names something the
        // document bound, a press that goes there.
        ui_.label(Rect{row.x0 + metrics.px(4.0f), row.y0, row.x1, row.y1},
                  word.key.empty() ? word.text : (word.key + "  " + word.text), Align::Left, kPlain,
                  0.8f);
        if (word.names_a_part) {
            const Rect go{row.x1 - metrics.row(), row.y0, row.x1, row.y1};
            if (ui_.icon_button(id_of("node.follow", w), go, Icon::Up, "Look at what this names")) {
                for (usize other = 0; other < graph_.nodes.size(); ++other) {
                    if (graph_.nodes[other].name == word.text) {
                        reveal(static_cast<u32>(other));
                        choose(static_cast<u32>(other));
                        break;
                    }
                }
            }
        }
    }

    if (!made_of.empty()) {
        const Rect heading{inner.x0, y, inner.x1, y + metrics.row()};
        y += row_height;
        ui_.label(Rect{heading.x0 + metrics.px(4.0f), heading.y0, heading.x1, heading.y1},
                  "made of", Align::Left, kPlain, 0.5f);
        for (usize m = 0; m < made_of.size(); ++m) {
            const ClipNode& part = graph_.nodes[made_of[m]];
            const Rect row{inner.x0, y, inner.x1, y + metrics.row()};
            y += row_height;
            if (row.y1 < list.y0 || row.y0 > list.y1) continue;
            const f32 cell = metrics.icon() * 0.8f;
            ui_.draw().icon(Rect{row.x0 + metrics.px(4.0f), row.mid_y() - cell * 0.5f,
                                 row.x0 + metrics.px(4.0f) + cell, row.mid_y() + cell * 0.5f},
                            icon_of(part));
            ui_.label(Rect{row.x0 + cell + metrics.px(8.0f), row.y0, row.x1 - metrics.row() * 2.0f,
                           row.y1},
                      part.name.empty() ? part.head : part.name, Align::Left, kPlain, 0.85f);
            const Rect cut{row.x1 - metrics.row() * 2.0f, row.y0, row.x1 - metrics.row(), row.y1};
            if (ui_.icon_button(id_of("node.cut", m), cut, Icon::Close, "Cut this wire")) {
                document_changed(
                    disconnect_clip_node(lines_, graph_, chosen_index_, static_cast<u32>(m)));
                ui_.end_scroll();
                return;
            }
            const Rect go{row.x1 - metrics.row(), row.y0, row.x1, row.y1};
            if (ui_.icon_button(id_of("node.part", m), go, Icon::Up, "Look at this one")) {
                reveal(made_of[m]);
                choose(made_of[m]);
                break;
            }
        }
    } else if (node.numbers.empty() && node.words.empty()) {
        // The honest empty case, said rather than left blank: a blank panel and a broken one look
        // the same, which is the failure `14-ui-style.md` names for a refusal that does not explain
        // itself.
        ui_.label(Rect{inner.x0 + metrics.px(4.0f), y, inner.x1, y + metrics.row()},
                  "nothing to change here", Align::Left, kPlain, 0.5f);
    }

    ui_.end_scroll();
}

std::string Shell::document() const {
    std::string text;
    if (began_with_mark_) text += "\xEF\xBB\xBF";
    for (usize i = 0; i < lines_.size(); ++i) {
        text += lines_[i];
        if (i + 1 < lines_.size()) text += "\n";
    }
    if (ended_with_newline_) text += "\n";
    return text;
}

// The same file, spelled the one way.
//
// `clips/sampler.clip`, `clips\\sampler.clip` and `C:/.../clips/sampler.clip` are one file and
// three different `path` objects, and everything in this editor that asks *is this the one already
// open* was comparing them for equality. So opening the file already open threw away the caret and
// the layout as though it were a different document, `is_shipped` answered from a relative path and
// got it wrong, and *this one is waiting to be saved* could name the file that was already there.
// One spelling, at the one door every document comes through.
std::filesystem::path Shell::same_file(const std::filesystem::path& path) const {
    std::error_code error;
    const std::filesystem::path settled = std::filesystem::weakly_canonical(path, error);
    return (error || settled.empty()) ? path.lexically_normal() : settled;
}

bool Shell::document_is_current() const {
    if (editing_.empty()) return false;
    std::error_code error;
    const std::filesystem::file_time_type when = std::filesystem::last_write_time(editing_, error);
    if (error) return false;   // it has gone: whatever is under that name now is not this
    const u64 bytes = static_cast<u64>(std::filesystem::file_size(editing_, error));
    if (error) return false;
    return when == editing_stamp_ && bytes == editing_bytes_;
}

// Everything about the open document, put down at once.
//
// Not a patch on the tab that draws it: the state a document consists of is `editing_` and about a
// dozen things that are ABOUT `editing_` — the lines, the graph, the caret, what is chosen in it,
// where you came from, what is open, the undo stack — and any one of them left behind is a bug
// waiting for the next document to walk into. So there is one place that puts all of it down and
// every route out of a document goes through it.
void Shell::close_document(const char* why) {
    if (editing_.empty()) return;
    editing_.clear();
    editing_shipped_ = false;
    editing_stamp_ = std::filesystem::file_time_type{};
    editing_bytes_ = 0;
    lines_.clear();
    graph_ = ClipGraph{};
    graph_stale_ = true;
    node_shown_.clear();
    node_tall_.clear();
    node_wide_.clear();
    implied_.clear();
    used_by_.clear();
    graph_x_.clear();
    graph_y_.clear();
    doc_shown_ = false;
    dragging_doc_ = false;
    dragging_node_ = ClipGraph::kNone;
    wiring_from_ = ClipGraph::kNone;
    drag_with_.clear();
    chosen_node_.clear();
    chosen_nodes_.clear();
    chosen_set_.clear();
    chosen_index_ = ClipGraph::kNone;
    inside_.clear();
    came_from_.clear();
    opened_.clear();
    undo_.clear();
    redo_.clear();
    undo_reason_.clear();
    making_ = 0;
    making_buffer_.clear();
    part_choices_.clear();
    dirty_ = false;
    report_ = ParseReport{};
    if (why != nullptr && *why != '\0') say(why, 3.0);
}

void Shell::open_document(const std::filesystem::path& raw) {
    const std::filesystem::path path = same_file(raw);
    editing_ = path;
    editing_shipped_ = library_.is_shipped(path);
    {
        std::error_code error;
        editing_stamp_ = std::filesystem::last_write_time(path, error);
        if (error) editing_stamp_ = std::filesystem::file_time_type{};
        error.clear();
        editing_bytes_ = static_cast<u64>(std::filesystem::file_size(path, error));
        if (error) editing_bytes_ = 0;
    }
    lines_.clear();
    std::ifstream file(path, std::ios::binary);
    std::string line;
    began_with_mark_ = false;
    // A different document is a different history. Undoing into the last file you had open is the
    // one thing an undo stack must never do.
    undo_.clear();
    redo_.clear();
    undo_reason_.clear();
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (lines_.empty() && line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF &&
            static_cast<unsigned char>(line[1]) == 0xBB &&
            static_cast<unsigned char>(line[2]) == 0xBF) {
            began_with_mark_ = true;
            line.erase(0, 3);
        }
        lines_.push_back(line);
    }
    // Whether the last line had a newline after it, which `getline` swallows either way. Asked of
    // the file directly, because there is nothing left in `lines_` that remembers.
    {
        char last = '\n';
        std::ifstream tail(path, std::ios::binary | std::ios::ate);
        const std::streamoff bytes =
            tail ? static_cast<std::streamoff>(tail.tellg()) : std::streamoff{0};
        if (bytes > 0) {
            tail.seekg(bytes - 1);
            tail.get(last);
        }
        ended_with_newline_ = (bytes == 0) || last == '\n';
    }
    if (lines_.empty()) lines_.push_back({});
    caret_line_ = 0;
    caret_column_ = 0;
    dirty_ = false;
    // Everything about the OTHER view goes with it: a graph of the last document, a node chosen in
    // it, and the travel its sliders were given are all about a file that is no longer open.
    caret_since_ = seconds_;
    caret_moved_ = true;
    mark_line_ = 0;
    mark_column_ = 0;
    selecting_ = false;
    script_pan_x_ = 0.0f;
    // Where you came from is about a JOURNEY, and opening a file off the shelf is not one. Anything
    // that means "go deeper" restores this after the call — see `enter_document`.
    came_from_.clear();
    inside_.clear();
    graph_stale_ = true;
    chosen_node_.clear();
    chosen_index_ = ClipGraph::kNone;
    range_node_.clear();
    node_range_.clear();
    graph_pan_x_ = 0.0f;
    graph_pan_y_ = 0.0f;
    graph_zoom_ = 1.0f;
    graph_fitted_ = false;
    dragging_node_ = ClipGraph::kNone;
    wiring_from_ = ClipGraph::kNone;
    menu_about_ = ClipGraph::kNone;
    palette_group_ = -1;
    ui_.set_scroll(id_of("editor.scroll"), 0.0f);
    reparse();
}

// Into a document, and back out of it.
//
// `open_document` is the way in from the shelf and forgets where you were, which is right: choosing
// a different file is not going deeper into anything. `enter_document` is the way in through an
// `include`, and it remembers — so a world's twenty pieces are twenty doors with a way back through
// each of them rather than twenty one-way trips.
void Shell::enter_document(const std::filesystem::path& path) {
    const std::filesystem::path from = editing_;
    // Where you were INSIDE the file you are leaving is not somewhere to come back to: the way back
    // is to that file, at its top, which is where it was opened.

    std::vector<std::filesystem::path> trail = came_from_;
    open_document(path);
    if (!from.empty() && from != editing_) trail.push_back(from);
    came_from_ = trail;
}

void Shell::leave_document() {
    // Out of a box first, then out of a file. One control for both journeys, because from a
    // player's side they are one journey: they went in, and this comes back.
    if (!inside_.empty()) {
        inside_.pop_back();
        graph_fitted_ = false;
        lay_out_graph();
        ui_.sound().say(Cue::Close);
        return;
    }
    if (came_from_.empty()) return;
    if (dirty_) {
        // Nothing is thrown away without being asked about, here as everywhere else in this tab.
        say("save this first -- ctrl-S", 3.0);
        ui_.sound().say(Cue::Refuse);
        return;
    }
    std::vector<std::filesystem::path> trail = came_from_;
    const std::filesystem::path back = trail.back();
    trail.pop_back();
    open_document(back);
    came_from_ = trail;
    ui_.sound().say(Cue::Close);
}

// The parse, and the ONE place it happens, so the two views cannot be showing different verdicts.
// `editing_` goes over with the text because a document's includes resolve relative to where it
// lives, and a world is very often one `include` line and nothing else.
void Shell::reparse() {
    parse_wanted_ = false;
    if (!parse_) return;
    const u64 began = now_ns();
    report_ = parse_(document(), editing_);
    parse_cost_ = static_cast<f64>(now_ns() - began) * 1.0e-9;

    // **A document with no shapes in it is not missing its solid.**
    //
    // The parser is right to say so about a WORLD: a world that never says which shape is the solid
    // builds to nothing, and that has to be shouted about. It is nonsense about a voxel type, which
    // is a colour and a roughness and has no shapes at all, and about a fragment that declares the
    // materials a building shares. Reported directly: *im getting warnings that make sense for a
    // material editor like "the file never says which shape is the solid"*.
    //
    // Decided from the document rather than from the file's extension, because that is the actual
    // question: something that makes no matter cannot be asked which of its matter is the world.
    // The game's own build path is untouched — it keeps saying it, because there it is true.
    if (!report_.ok && report_.line == 0 && report_.where.empty() &&
        report_.message.find("which shape is the solid") != std::string::npos) {
        refresh_graph();
        bool any_shape = false;
        for (const ClipNode& node : graph_.nodes) {
            if (node.carries == ClipCarries::Shape) any_shape = true;
        }
        if (!any_shape) report_ = ParseReport{};
    }
}

// **D453 said the script is parsed on every keystroke, and this is what that had to become.**
//
// The rule it exists for is unchanged and is the important half: a script that does not parse is
// not an error, the view says so in one line, and nothing pops up — because a parser that
// interrupts you halfway through typing a word is a parser you fight. What was literal in it was
// *on every keystroke*, and that was affordable while the only thing the editor could open was a
// clip. A **world** costs the whole building: 22 ms to splice its twenty-two pieces and 54 to read
// them, measured, which is five frames a letter on exactly the file kind D744 made editable.
//
// So the verdict is asked for when the text has been STILL for a moment, and the moment is a
// function of what the last one cost — three times it, capped at half a second. A document that
// parses in a millisecond re-parses three milliseconds later, which is every keystroke in
// everything but name; a world re-parses a fifth of a second after you stop, which is a verdict
// that arrives while you are still looking at the line you typed. The cap is what stops a document
// that has become expensive from quietly stopping being checked at all.
void Shell::reparse_soon() {
    parse_wanted_ = true;
    parse_at_ = seconds_ + std::clamp(parse_cost_ * 3.0, 0.0, 0.5);
}

void Shell::save_document() {
    if (editing_.empty()) return;
    // What came with the game is not the player's to change (D494). It can be opened and it can be
    // duplicated — and the duplicate lands on their own shelf, which is what makes *duplicate* the
    // way to edit something the game shipped. Refused rather than written, and the refusal says
    // what to do instead: a refusal that does not explain itself is indistinguishable from a bug.
    if (editing_shipped_) {
        say("this one came with the game -- duplicate it and edit the copy", 4.0);
        ui_.sound().say(Cue::Refuse);
        return;
    }
    if (!dirty_) return;
    std::ofstream file(editing_, std::ios::binary | std::ios::trunc);
    if (!file) {
        message_ = "could not write that file";
        message_until_ = seconds_ + 2.5;
        return;
    }
    const std::string text = document();
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    file.close();
    dirty_ = false;
    {
        std::error_code error;
        editing_stamp_ = std::filesystem::last_write_time(editing_, error);
        if (error) editing_stamp_ = std::filesystem::file_time_type{};
        error.clear();
        editing_bytes_ = static_cast<u64>(std::filesystem::file_size(editing_, error));
        if (error) editing_bytes_ = 0;
    }
    ui_.sound().say(Cue::Commit);
    // Whatever was chosen while this one had unsaved changes opens now. Nothing was thrown away
    // and nothing had to be answered: the file that was waiting is the one the player picked.
    if (!waiting_.empty() && same_file(waiting_) != editing_) {
        const std::filesystem::path next = waiting_;
        waiting_.clear();
        open_document(next);
    }
}

void Shell::edit_keys(const InputState& input) {
    if (editing_.empty() || lines_.empty()) return;
    // What came with the game is READ ONLY, not merely unsaveable (D494, and asked for directly).
    //
    // It refused the save and took every keystroke up to it, so a player could spend ten minutes on
    // a built-in and be told at the end. A refusal that arrives after the work is worse than one
    // that arrives instead of it. The caret still moves — reading a file means moving about in it —
    // and nothing that CHANGES a byte is accepted.
    const bool wants_to_change = !input.typed.empty() || input.fired(Key::Enter) ||
                                 input.fired(Key::Backspace) || input.fired(Key::Delete);
    if (editing_shipped_) {
        if (wants_to_change) {
            say("this one came with the game -- duplicate it and edit the copy", 3.0);
            ui_.sound().say(Cue::Refuse);
        }
        move_caret(input);
        return;
    }
    // Who made a file is not the player's to type over (D447). The line is on the screen because
    // the file is on the screen; it is not editable because a name the interface lets you overwrite
    // is a name that means nothing. A backspace at the start of the line under it would join into
    // it, so that is refused too.
    {
        const usize at = std::min<usize>(caret_line_, lines_.size() - 1);
        const bool on_it = is_author_line(lines_[at]);
        const bool joining_it =
            at > 0 && caret_column_ == 0 && input.fired(Key::Backspace) &&
            is_author_line(lines_[at - 1]);
        const bool eating_it = at + 1 < lines_.size() && input.fired(Key::Delete) &&
                               caret_column_ >= lines_[at].size() && is_author_line(lines_[at + 1]);
        // And a selection that runs ACROSS it takes it with everything else, which is the same
        // edit by a longer route. Cut and paste go through here too, so both are covered.
        bool chosen_over_it = false;
        if (has_selection() && (wants_to_change || (input.is_down(Key::Ctrl) &&
                                                    (input.was_pressed(Key::X) ||
                                                     input.was_pressed(Key::V))))) {
            u32 from_line = 0;
            u32 from_column = 0;
            u32 to_line = 0;
            u32 to_column = 0;
            selection_span(from_line, from_column, to_line, to_column);
            for (u32 line = from_line; line <= to_line && line < lines_.size(); ++line) {
                if (is_author_line(lines_[line])) chosen_over_it = true;
            }
        }
        if ((on_it && wants_to_change) || joining_it || eating_it || chosen_over_it) {
            say("who made a file travels with it and is not edited from in here", 3.0);
            ui_.sound().say(Cue::Refuse);
            move_caret(input);
            return;
        }
    }
    // The editor takes the keyboard only when its own view is the one showing, which is the check
    // the caller has already made by drawing it.
    // --- the four a text view owes: choose everything, copy, cut, paste ---------------------
    //
    // Every one of them on the key a player's hands already know. Before this there was nothing to
    // copy FROM — no selection at all — so the editor was a place you could type into and not a
    // place you could work in.
    if (input.is_down(Key::Ctrl)) {
        if (input.was_pressed(Key::A)) {
            mark_line_ = 0;
            mark_column_ = 0;
            caret_line_ = static_cast<u32>(lines_.size() - 1);
            caret_column_ = static_cast<u32>(lines_.back().size());
            caret_moved_ = true;
            caret_since_ = seconds_;
            return;
        }
        if ((input.was_pressed(Key::C) || input.was_pressed(Key::X)) && has_selection()) {
            set_clipboard_text(selected_text());
            if (input.was_pressed(Key::X)) {
                remember("cut");
                erase_selection();
                dirty_ = true;
                graph_stale_ = true;
                reparse_soon();
            }
            caret_moved_ = true;
            caret_since_ = seconds_;
            return;
        }
        if (input.was_pressed(Key::V)) {
            const std::string pasted = clipboard_text();
            if (!pasted.empty()) {
                remember("paste");
                erase_selection();
                // Split on newlines, and drop a carriage return that came with them: text copied
                // out of a Windows editor carries CRLF, and a stray \r in a clip is a character the
                // tokenizer treats as space and the file carries for ever.
                std::vector<std::string> parts{std::string()};
                for (char c : pasted) {
                    if (c == '\n') {
                        parts.emplace_back();
                    } else if (c != '\r') {
                        parts.back() += c;
                    }
                }
                std::string& into = lines_[std::min<usize>(caret_line_, lines_.size() - 1)];
                const u32 at = static_cast<u32>(std::min<usize>(caret_column_, into.size()));
                const std::string tail = into.substr(at);
                into = into.substr(0, at) + parts.front();
                if (parts.size() == 1) {
                    caret_column_ = static_cast<u32>(into.size());
                    into += tail;
                } else {
                    for (usize k = 1; k < parts.size(); ++k) {
                        lines_.insert(lines_.begin() + static_cast<isize>(caret_line_) + k,
                                      parts[k]);
                    }
                    caret_line_ += static_cast<u32>(parts.size() - 1);
                    caret_column_ = static_cast<u32>(parts.back().size());
                    lines_[caret_line_] += tail;
                }
                drop_mark();
                dirty_ = true;
                graph_stale_ = true;
                reparse_soon();
                caret_moved_ = true;
                caret_since_ = seconds_;
            }
            return;
        }
    }

    // Anything that writes replaces what is chosen, which is what every text field everywhere does.
    if (has_selection() && (!input.typed.empty() || input.fired(Key::Enter) ||
                            input.fired(Key::Backspace) || input.fired(Key::Delete))) {
        remember("type");
        erase_selection();
        dirty_ = true;
        graph_stale_ = true;
        reparse_soon();
        caret_moved_ = true;
        caret_since_ = seconds_;
        // A backspace or a delete on a selection has done its whole job by removing it.
        if (input.typed.empty() && !input.fired(Key::Enter)) return;
    }

    if (!input.typed.empty() || input.fired(Key::Enter) || input.fired(Key::Backspace) ||
        input.fired(Key::Delete)) {
        remember("type");
    }
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

    // **The anchor comes with the caret.** Without this the mark stays where the caret was before
    // the first letter, so after typing one character there is a one-character SELECTION — and the
    // next character replaces it. Reported exactly that way: *i can just type one letter before
    // replacing that letter with the next.* A selection is something a hand asked for; an edit is
    // never one.
    if (touched) drop_mark();

    move_caret(input);

    if (touched) {
        dirty_ = true;
        // Re-parsed on every keystroke, and a script that does not parse is NOT an error (D453):
        // the status line says where it stopped and nothing pops up, because a parser that
        // interrupts you halfway through typing a word is a parser you fight.
        reparse_soon();
        // The graph, though, is re-read at once, so the other view is this document rather than the
        // document as it was when it was opened (D452). It costs 1.3 ms on the largest fragment in
        // the repository and it reads the document alone — where the parse reads everything the
        // document includes, which is why one of the two is deferred and the other is not.
        graph_stale_ = true;
    }
}

// --- what is chosen ---------------------------------------------------------------------------
//
// One anchor and one caret, and the selection is whatever lies between them. Everything else — the
// double-click, the drag, ctrl-A, and what ctrl-C takes — is a way of putting those two somewhere.

void Shell::drop_mark() {
    mark_line_ = caret_line_;
    mark_column_ = caret_column_;
}

bool Shell::has_selection() const {
    return mark_line_ != caret_line_ || mark_column_ != caret_column_;
}

void Shell::selection_span(u32& from_line, u32& from_column, u32& to_line, u32& to_column) const {
    const bool forwards =
        mark_line_ < caret_line_ || (mark_line_ == caret_line_ && mark_column_ <= caret_column_);
    from_line = forwards ? mark_line_ : caret_line_;
    from_column = forwards ? mark_column_ : caret_column_;
    to_line = forwards ? caret_line_ : mark_line_;
    to_column = forwards ? caret_column_ : mark_column_;
}

std::string Shell::selected_text() const {
    if (!has_selection()) return {};
    u32 from_line = 0;
    u32 from_column = 0;
    u32 to_line = 0;
    u32 to_column = 0;
    selection_span(from_line, from_column, to_line, to_column);
    std::string out;
    for (u32 line = from_line; line <= to_line && line < lines_.size(); ++line) {
        const std::string& text = lines_[line];
        const usize begin = (line == from_line) ? std::min<usize>(from_column, text.size()) : 0;
        const usize end = (line == to_line) ? std::min<usize>(to_column, text.size()) : text.size();
        if (end > begin) out += text.substr(begin, end - begin);
        if (line != to_line) out += "\n";
    }
    return out;
}

void Shell::erase_selection() {
    if (!has_selection()) return;
    u32 from_line = 0;
    u32 from_column = 0;
    u32 to_line = 0;
    u32 to_column = 0;
    selection_span(from_line, from_column, to_line, to_column);
    if (from_line >= lines_.size()) return;
    to_line = std::min<u32>(to_line, static_cast<u32>(lines_.size() - 1));
    const std::string head = lines_[from_line].substr(
        0, std::min<usize>(from_column, lines_[from_line].size()));
    const std::string tail =
        lines_[to_line].substr(std::min<usize>(to_column, lines_[to_line].size()));
    lines_[from_line] = head + tail;
    if (to_line > from_line) {
        lines_.erase(lines_.begin() + static_cast<isize>(from_line) + 1,
                     lines_.begin() + static_cast<isize>(to_line) + 1);
    }
    caret_line_ = from_line;
    caret_column_ = static_cast<u32>(head.size());
    drop_mark();
}

// A word is letters, digits and the underscore, because `snake_case_names` are one word to whoever
// wrote them — the same rule the markdown reader uses to leave them alone.
void Shell::word_at(const std::string& line, u32 column, u32& from, u32& to) {
    const auto part_of_a_word = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '_' || c == '.' || c == '-';
    };
    usize at = std::min<usize>(column, line.size());
    if (at >= line.size() || !part_of_a_word(line[at])) {
        // Not on a word: take the run of whatever is under the pointer instead, so a double-click
        // on a space or a brace still chooses something rather than nothing.
        if (at >= line.size()) {
            from = static_cast<u32>(line.size());
            to = from;
            return;
        }
        usize begin = at;
        while (begin > 0 && !part_of_a_word(line[begin - 1]) && line[begin - 1] == line[at]) --begin;
        usize end = at;
        while (end < line.size() && !part_of_a_word(line[end]) && line[end] == line[at]) ++end;
        from = static_cast<u32>(begin);
        to = static_cast<u32>(end);
        return;
    }
    usize begin = at;
    while (begin > 0 && part_of_a_word(line[begin - 1])) --begin;
    usize end = at;
    while (end < line.size() && part_of_a_word(line[end])) ++end;
    from = static_cast<u32>(begin);
    to = static_cast<u32>(end);
}

// Where the caret goes, which a document being READ needs as much as one being written.
void Shell::move_caret(const InputState& input) {
    if (lines_.empty()) return;
    const std::string& line = lines_[std::min<usize>(caret_line_, lines_.size() - 1)];
    caret_column_ = static_cast<u32>(std::min<usize>(caret_column_, line.size()));
    const bool extending = input.is_down(Key::Shift);
    bool moved = false;
    if (input.fired(Key::Left) && caret_column_ > 0) {
        --caret_column_;
        moved = true;
    }
    if (input.fired(Key::Right) && caret_column_ < line.size()) {
        ++caret_column_;
        moved = true;
    }
    if (input.fired(Key::Up) && caret_line_ > 0) {
        --caret_line_;
        moved = true;
    }
    if (input.fired(Key::Down) && caret_line_ + 1 < lines_.size()) {
        ++caret_line_;
        moved = true;
    }
    // A page at a time, because a clip fragment is sixteen hundred lines and an arrow key is not a
    // way to cross one.
    if (input.fired(Key::PageUp)) {
        caret_line_ = (caret_line_ > 20u) ? caret_line_ - 20u : 0u;
        moved = true;
    }
    if (input.fired(Key::PageDown)) {
        caret_line_ = static_cast<u32>(
            std::min<usize>(caret_line_ + 20u, lines_.empty() ? 0 : lines_.size() - 1));
        moved = true;
    }
    if (input.was_pressed(Key::Home)) {
        caret_column_ = 0;
        moved = true;
    }
    if (input.was_pressed(Key::End)) {
        caret_column_ =
            static_cast<u32>(lines_[std::min<usize>(caret_line_, lines_.size() - 1)].size());
        moved = true;
    }

    if (moved) {
        // Shift keeps the anchor where it was, so the movement drags a selection behind it;
        // anything else brings the anchor along, which is the same as having none.
        if (!extending) drop_mark();
        caret_moved_ = true;
        caret_since_ = seconds_;
    }
}

}  // namespace ws::ui
