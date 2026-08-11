#pragma once
// The shell: what the game opens on, and everything that is on screen that is not the world.
//
// documentation/23-shell-and-libraries.md is the specification; decisions D441–D456. The shape of
// it, in the order it matters:
//
//   **The game opens on a title, not in a world** (D441). Nothing is loaded until somebody asks
//   for something to be loaded, which is what makes `09-performance-budgets.md`'s *cold start to
//   main menu ≤3 s* and *enter a world ≤5 s* two numbers about two different events. It is also
//   what first exercises `02-architecture-overview.md`'s rule that **a world is torn down on the
//   way out, never shared**.
//
//   **Two buttons on it: worlds, and settings. Nothing else** (D442). No news, no store, no social
//   feed, and no "continue" that guesses which world you meant. Leaving is the window's own close
//   button and Escape, because a third button spends a third of the interface on the one action
//   every player already knows how to do.
//
//   **Every scripted run walks straight past it.** `--screenshot`, `--settle`, `--cam`, `--fly`,
//   `--chisel`, `--ticks`, `--stream-frames` and the crash tests open the world they were told to
//   open and never wait for a click. Every measurement in this project is taken by one of those
//   flags, and a menu a harness has to click through would end measurement here.
//
// # The state machine
//
// Three states — title, library, world — and the loading screen covers the transition between the
// last two. "Library" is not a separate screen: it is the title with a library window open on it,
// which is what makes opening a world and organising your worlds the same two clicks.

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "core/types.hpp"
#include "ui/dock.hpp"
#include "ui/library.hpp"
#include "ui/logo.hpp"
#include "ui/widgets.hpp"

namespace ws::ui {

// What the shell is showing. The world is drawn by the renderer and the title draws its own room,
// so this is also what the GPU pass is told to put behind the panels.
enum class Stage : u32 {
    Title,   // a place, not a screen: one room, two things in it
    World,
};

// What a frame of the shell decided. Everything that changes the process's state leaves through
// here rather than being done inside a window, so the one place that can open or close a world is
// the one place that knows how to tear one down.
struct Verdict {
    bool quit = false;                  // the title was left
    bool open_world = false;            // and this is the file
    std::filesystem::path world;
    bool leave_world = false;           // back to the title, tearing this world down
};

// What the shell itself remembers, per player, beside the game's own settings.txt.
struct Preferences {
    // The name every file this player makes is stamped with (D447). It is the same chosen username
    // the invite codes will carry (answer J2), so a player has one identity and not two.
    std::string username = "shaper";

    // The player's own colour, used ONLY where inversion is blind — a sixth of the brightness
    // range, centred on mid grey. Pure green by default, which is the entire point of it being a
    // colour at all: it is a setting.
    f32 accent[3]{0.0f, 1.0f, 0.0f};

    // The logo's hue. The one SURFACE allowed a colour of its own, so it gets its own setting
    // rather than borrowing the accent — an interface where the mark and the blind-band ink are
    // the same colour has quietly turned one permitted colour into a theme.
    u32 logo_rgb = 0xE08A2Cu;

    bool sound = true;
    f32 volume = 0.6f;

    // There is no share switch (D495). Everything in a library is offered while you are online,
    // full stop. D450 kept a switch for the player who had thought about it — and a switch that
    // ships on, that nobody turns off, and that the community tab has to check on every peer is a
    // setting whose only real effect is that the browser is sometimes mysteriously empty. *Online*
    // and *browsable* being one state is what makes the community tab work at all without a server
    // (`23-shell-and-libraries.md` §5b): being reachable IS the publishing.

    // 0 means "work it out from the window", which is what almost everybody wants.
    f32 interface_scale = 0.0f;

    bool read(const std::filesystem::path& path);
    bool write(const std::filesystem::path& path) const;
};

// The knobs the settings window edits that belong to the renderer rather than to the shell. Held
// as plain numbers so that ws_ui does not have to know what an AutoQuality is; the application
// copies them in before the frame and reads them out after, and `changed` says when that is worth
// doing.
struct Knobs {
    bool auto_quality = true;
    f64 target_fps = 0.0;        // 0 is the monitor's own refresh rate
    f64 quality_level = 7.0;
    f64 render_scale = 1.0;
    // The camera's own shipped value (src/game/camera.hpp), not a number of this struct's own.
    // These defaults are what *reset* puts a row back to, so one that disagreed with the game
    // would be a button that changes a setting to something the game never had.
    f64 field_of_view = 90.0;
    bool vsync = true;
    bool motion_blur = true;
    bool overlay = false;        // the player-facing performance overlay
    bool changed = false;        // set by the shell, cleared by whoever applies it
};

// The player-facing performance readout, as numbers rather than as a window.
//
// It was an ImGui window, and ImGui is rendered onto the swapchain after the interface has been
// composited — so it was drawn OVER the docked windows, which is backwards: a readout of what the
// frame costs is about the world, and a window is in front of the world. Handing the numbers to
// the shell instead puts it in the interface's own compositing order, so a panel covers it the way
// a panel covers anything, and it is lettered in the game's own face and inverts against whatever
// is behind it like every other mark.
struct Overlay {
    bool on = false;
    f64 fps = 0.0;
    f64 frame_ms = 0.0;
    f64 worst_ms = 0.0;
    f64 gpu_ms = 0.0;
    u32 width = 0;
    u32 height = 0;
};

// What the editor tab learned from the last keystroke. A script that does not parse is NOT an
// error (D453): the view says so in one line and nothing pops up.
struct ParseReport {
    bool ok = true;
    u32 line = 0;
    std::string message;
};

class Shell {
public:
    Shell();

    void set_stage(Stage stage);
    Stage stage() const { return stage_; }

    Ui& ui() { return ui_; }
    const Ui& ui() const { return ui_; }
    Preferences& preferences() { return preferences_; }
    Knobs& knobs() { return knobs_; }
    Library& library() { return library_; }
    Overlay& overlay() { return overlay_; }

    // Where the layout and the preferences are kept. Under the same root the libraries are, so a
    // player who copies that folder has copied their whole setup.
    // `root` is where the player's own files are; `game` is the folder the executable sits in,
    // which is where the clips the game ships with are read from, in place, for ever (D494).
    void load(const std::filesystem::path& root, const std::filesystem::path& game);
    void save() const;
    // Written whenever it has changed, at most once a second, rather than only on the way out — a
    // settings file that a crash throws away is a settings file (D496).
    void save_if_changed();

    // Puts the worlds the game ships with on the shelf, once, if the shelf is empty.
    //
    // A library over a real folder starts as an EMPTY real folder, and a first run whose worlds
    // list is blank teaches a player that the button does nothing. So the scenes that travel with
    // the executable are copied in — with the author tag every file carries — and after that they
    // are the player's own files, to rename, duplicate or throw away like any others.
    void seed_worlds(const std::filesystem::path& shipped);

    // Somebody has to be able to say whether a document parses without ws_ui knowing what a clip
    // is. The application supplies this, using the type table it already has.
    void set_parser(std::function<ParseReport(const std::string&)> parser) {
        parse_ = std::move(parser);
    }

    // One frame. `input` is the raw window state; `seconds` is a clock that does not stop.
    // Everything the shell decided comes back in the verdict.
    Verdict frame(const InputState& input, u32 width, u32 height, f64 seconds);

    // In the world, this is what Escape reaches: **both** windows at once, in one press.
    //
    // It was three steps — give the mouse back, then open the library, then close it again — so the
    // key everybody presses to reach the settings had to be pressed twice before anything appeared,
    // and what appeared was the other half of the interface. The two families are one STATE (D443):
    // what a player wants when they press Escape is the menu, not one side of it. So one press puts
    // parameters on the left and the library on the right, and the next takes both away.
    void open_windows();
    // The settings window, with every section folded away again — which is what it is supposed to
    // open as, every time and not only the first.
    void open_settings();
    void close_windows();
    // The same without the sound, for the paths that have one of their own. It also puts down the
    // keyboard and any menu, because a control that stops being drawn never gets to let go itself.
    void shut_windows();
    void toggle_windows();
    bool windows_open() const;
    // What is left in the middle: the world, which a window may never cover. A press in here while
    // the windows are up means *back to what I was doing*, and the caller is the only thing that
    // can act on that because it owns the mouse.
    const Rect& centre() const { return dock_.centre(); }
    // What a scripted photograph of the shell needs: open a named window without a click. Nothing
    // else opens one, so this is the whole of the harness's reach into the interface.
    void open_window(std::string_view which, bool open);
    // Which shelf the library window is showing, by folder name. `--shelf clips` is the only thing
    // that calls it: the shelves other than *worlds* were reachable in the interface and by nothing
    // a run could photograph, so "the built-in clips are not in the library" was a report that
    // could only be answered by clicking.
    bool open_shelf(std::string_view folder);

    // What is being played, so the worlds library can say so and offer the way out.
    void set_playing(std::string name) { playing_ = std::move(name); }

    // One line, at the bottom of the screen, for a few seconds. The application uses it to say why
    // a world came up empty — which used to be a line in a log file and is therefore a thing no
    // player has ever seen.
    void say(std::string line, f64 seconds);

    // `--icon-sheet`: draw the whole vocabulary instead of the title — every drawing at four sizes
    // and across five steps of its own animation. The smallest of the four is the size a row
    // actually sets an icon at on a small desktop, which is where a drawing stops being legible and
    // is the one size nothing ever looked at them at.
    void show_icons(bool on) { icons_ = on; }

    // `--logo-seed N`: fix which combination the mark draws, so a photograph of the title can be
    // compared with the last one. Nothing changes it after this, including a press on the mark.
    void pin_logo(u32 seed) { logo_.pin(seed); }
    // `--logo-change N`: ask for another combination on a named frame, pinned or not, so the morph
    // between two of them is a thing a photograph can be taken of.
    void change_logo() { logo_.force_change(seconds_); }
    const LogoSeed& logo() const { return logo_; }

private:
    void draw_title(Verdict& verdict);
    void draw_overlay();
    void draw_icon_sheet();
    void draw_settings(const Rect& rect);
    void draw_library(const Rect& rect, Verdict& verdict);
    void draw_library_tab(const Rect& rect, Verdict& verdict);
    // The menu a right-click opens, drawn after the listing so it sits over it.
    void draw_library_menu(Verdict& verdict);
    // What *use this* means for one entry, wherever it was asked for.
    void open_entry(const Entry& entry, Verdict& verdict);
    void make_new_file();
    // Every setting back to how it shipped and every file the player made to the recycle bin.
    std::string wipe_everything();
    void draw_community_tab(const Rect& rect);
    void draw_editor_tab(const Rect& rect);
    void draw_header(const Rect& rect, u32 window, Icon icon, std::string_view title);

    // The selection, which every operation in a library works on. Held by NAME rather than by
    // index, because a listing is re-read whenever the folder changes underneath and an index into
    // the previous listing is a file somebody else is about to be deleted instead of.
    bool selected(const Entry& entry) const;
    void select_only(const Entry& entry);
    void select_add(const Entry& entry);
    void select_range(const std::vector<Entry>& entries, const Entry& to);
    std::vector<Entry> selection() const;
    void clear_selection();

    Ui ui_;
    Dock dock_;
    Library library_;
    LogoSeed logo_;
    Preferences preferences_;
    Knobs knobs_{};
    Overlay overlay_{};
    Stage stage_ = Stage::Title;
    std::filesystem::path root_;
    std::string playing_;

    bool icons_ = false;
    // How tall the settings came out last frame. See draw_settings for why it is measured rather
    // than counted now that a section can be folded away.
    f32 settings_height_ = 0.0f;
    // When *reset everything* stops being armed. A destructive button that is one press is a
    // button somebody presses by accident; one that needs a second press within a few seconds is
    // one that cannot be pressed by habit either.
    f64 wipe_armed_until_ = 0.0;
    // What was last written, and when to next bother comparing.
    mutable std::string saved_;
    f64 save_check_at_ = 0.0;
    std::string snapshot() const;
    void adopt_old_settings_files();
    u32 window_settings_ = 0;
    u32 window_worlds_ = 0;
    u32 tab_ = 0;               // library, community, editor
    u32 view_ = 0;              // and, inside the editor, script or visual
    u32 kind_ = 0;              // which shelf the library window is showing

    std::vector<std::string> chosen_;   // the selection, by file name
    std::string last_clicked_;
    // What the open menu is ABOUT, captured when it opened. Not re-read from the selection every
    // frame: a menu whose items change while it is up is a menu whose indices mean something else
    // by the time one is chosen.
    std::vector<Entry> menu_of_;
    std::string search_;                // what the library's search box holds
    std::string message_;               // one line, from the last operation that had something to say
    f64 message_until_ = 0.0;

    // Renaming happens in place, on the row, because a dialog for a name is a dialog.
    std::string renaming_;
    std::string rename_buffer_;
    // And the new-folder row, which appears at the top of the listing when it is being made.
    bool naming_folder_ = false;
    std::string folder_buffer_;

    // The editor tab. It asks for a file before it opens (D455), so this is empty until one is
    // chosen and the tab sends you to the library until it is.
    std::filesystem::path editing_;
    std::vector<std::string> lines_;
    u32 caret_line_ = 0;
    u32 caret_column_ = 0;
    bool dirty_ = false;
    ParseReport report_{};
    std::function<ParseReport(const std::string&)> parse_;

    std::string document() const;
    void open_document(const std::filesystem::path& path);
    void save_document();
    void edit_keys(const InputState& input);

    f64 seconds_ = 0.0;
};

}  // namespace ws::ui
