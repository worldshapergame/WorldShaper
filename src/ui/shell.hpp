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
#include <utility>
#include <vector>

#include "core/types.hpp"
#include "game/clip_graph.hpp"
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
    // A voxel type off the materials shelf, chosen to build with.
    //
    // *Make it so that the player will place the material he has selected on his library instead of
    // the Q and E system which is now obsolete.* Asked for directly, and it is the right shape: Q
    // and E stepped through whatever the OPEN WORLD happened to declare, in the order it declared
    // them, which is a palette a player cannot see, cannot name and cannot add to. A library is all
    // three of those things already.
    std::filesystem::path paint_with;
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
    // The line IN THIS DOCUMENT, or nought when the trouble is inside something it includes — a
    // world is very often one `include` line and the fault is three files away, and marking line
    // three of the wrong file is worse than marking nothing.
    u32 line = 0;
    std::string message;
    std::string where;   // the file it is in, when that is not this one
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
    //
    // It is given the PATH as well as the text, because a document's includes resolve relative to
    // where it lives — and a world is very often nothing but `include "facility.clip"`, which
    // without the path reads as an error on the one file the game makes when a player presses
    // *new*.
    void set_parser(std::function<ParseReport(const std::string&, const std::filesystem::path&)>
                        parser) {
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

    // Open the editor on a file, with the library window up and the editor tab showing. This is
    // what *edit* in the library's menu does, what following a selection into the editor does, and
    // what `--edit FILE` does — one path, because "open this in the editor" meaning three slightly
    // different things is exactly how two of them end up wrong.
    void open_editor(const std::filesystem::path& path);
    // `--editor-view script|visual`: which of the two views is showing. The visual one is the one
    // nothing could photograph, which is the same argument `--title-shot` is made of.
    bool open_editor_view(std::string_view which);
    // `--editor-node NAME[,NAME...]`: choose one node, or several, by the names the document bound
    // them to. The only way a run with no hand on the mouse can put the parameters panel on screen —
    // and, with a list, the only way it can photograph a choice of several, which is the one state
    // a dragged box exists to produce.
    bool choose_node(std::string_view names);

    // `--editor-part FILE`: put another document inside this one, exactly as the graph's menu
    // does — copied beside it and included by name. And `--editor-new-part clip|material NAME`,
    // which is the other half of the same menu. Empty on success, one line otherwise.
    //
    // A whole document made of other documents is what every world the game ships IS, and until
    // these there was no way to build one without a hand on the mouse (D460).
    std::string add_part(const std::filesystem::path& from);
    std::string new_part(std::string_view kind, std::string_view name);

    // How many sockets the box named NAME has. **A box IS its sockets**, so this is how a test
    // asks what a reader would see on it — there is nothing to open any more, because everything a
    // node takes is already written down its left side.
    u32 sockets_on(std::string_view name) const;

    // `--new-world FILE`, and empty for a world of nothing: what the new-world menu does, by the
    // one path a press takes. A world made of a clip is what *new* on the worlds shelf offers, and
    // a flow nothing automated can walk is a flow nothing automated ever checks (D460).
    std::string new_world(const std::filesystem::path& from);

    // How many boxes on the graph stand where another one already is. **Always nought**, and it is
    // a promise rather than a tidy-up: every box is on a whole cell, and a cell holds one. Asked
    // directly rather than by looking at a picture, because a picture is what a person has to check
    // and this is the class of thing that goes wrong when nobody is looking (D460).
    u32 boxes_overlapping() const;
    // `--editor-enter NAME`: go into the `include` of that name, exactly as pressing its door mark
    // does. The way in and the way back out are a whole state of this tab — the header grows a
    // control, and the document under it is a different one — and a state nothing automated can
    // reach is a state nothing automated ever checks (D460).
    std::string enter_node(std::string_view name);
    // Back out of whatever was entered, one step. What the header's control does.
    void leave_document();
    // How deep in this document is, which is what the back control is drawn from.
    usize came_from() const { return came_from_.size(); }

    // `--editor-add box`: put a new node in the open document, by the same path the palette takes.
    // One path rather than two, so that what a scripted run photographs is what a press does — and
    // so the whole of "the visual view CHANGES the document" is reachable without a hand on the
    // mouse, which is the only way anything automated ever looks at it (D460).
    std::string add_node(std::string_view head);
    const std::filesystem::path& editing() const { return editing_; }

    // What is being played, so the worlds library can say so and offer the way out.
    void set_playing(std::string name) { playing_ = std::move(name); }
    // Which file the tool is building with, so the shelf can put a mark on that row. Set by
    // whoever answered the verdict, because only they know whether it worked.
    void set_painting(std::filesystem::path with) { painting_ = std::move(with); }
    const std::filesystem::path& painting() const { return painting_; }

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
    // --- what a new world is made FROM ------------------------------------------------------------
    //
    // *When you create a world on your library you first get an option between terrain (we will make
    // it work later) or clip, and from clip you can select any of your library clips.* Asked for
    // directly, and it replaces the fallback the facility used to be: **a new world is not the
    // building the game ships with.** That was a stopgap for having nothing to generate a world
    // from, and it made every new world identical — which is why editing one and making another
    // read as the edit having been lost.
    //
    // 0 nothing, 1 choosing what it is made from, 2 choosing which clip.
    u32 making_world_ = 0;
    void draw_new_world_menu();
    std::string make_world_from(const std::filesystem::path& clip);
    // The file that is open is GONE. Everything about it goes with it, because a tab that edits a
    // file nobody can open is a tab that saves over whatever takes its name next.
    void close_document(const char* why);
    // Every setting back to how it shipped and every file the player made to the recycle bin.
    std::string wipe_everything();
    void draw_community_tab(const Rect& rect);
    void draw_editor_tab(const Rect& rect);
    // The lessons, listed where the editor sits when nothing is open.
    //
    // *I can barely understand how things connect and work.* The rules of this language are three
    // sentences long and every one of them was learnable only by breaking it. So the four documents
    // that teach them ship with the game and open in the editor like anything else — each one is a
    // real clip, buildable, with its lesson written in its own comments and something to change on
    // every line. There is nothing to maintain beside the game: a lesson that stopped parsing would
    // fail `ws_clipcheck` with every other clip.
    void draw_lessons(const Rect& rect);
    // The two views of one document (D452). The script is text; the visual one is the same
    // statements drawn as nodes and the names between them drawn as wires.
    void draw_script_view(const Rect& rect);
    void draw_visual_view(const Rect& rect);
    // A node's parameters are a parameters window, and that is why the two families exist
    // (`23-shell-and-libraries.md` §5c). It takes the left-hand window while a node is selected.
    void draw_node_parameters(const Rect& rect);
    bool a_node_is_selected() const;
    // Re-read the document into a graph. Only when the text has actually changed, because this is
    // otherwise a parse a frame for a window nobody is typing into.
    void refresh_graph();
    // Choose a node by index, keeping the key and the index in step. `ClipGraph::kNone` is
    // "nothing", and everything that selects goes through here so the two cannot disagree.
    void choose(u32 index);
    // Choose several at once — what a dragged box produces. The first is the primary, so the
    // left-hand window still has one node to be about.
    void choose_many(const std::vector<u32>& indices);
    bool is_chosen(u32 index) const;
    // Which node covers a line of the document, innermost first. It is the whole of the link
    // between the two views: the statement the caret is in is the box that is lit over there, and
    // the box you double-click is the line the caret goes to.
    u32 node_at_line(u32 line) const;
    // Where every box sits, worked out once when the document is re-read. See the cpp for why the
    // row is the part worth choosing well.
    void lay_out_graph();
    // --- a box that is opened where it stands --------------------------------------------------
    //
    // *These material nodes should show their settings inside their actual node instead of in
    // another settings window so you can directly tweak them from there.* Reported directly, and it
    // is right for exactly the nodes it was asked for: a shape's numbers are six positions that
    // mean nothing without their labels, and a material's are a list of named properties, which is
    // a thing you read down rather than a thing you look up.
    // Every node's sockets, worked out with the layout rather than per frame: the height of a box
    // is the number of them, so the layout needs them and every draw wants the same answer.
    std::vector<std::vector<ClipSocket>> sockets_;
    // The socket a wire is being dragged toward, and which one it left. A wire leaves an OUTPUT and
    // lands on a named INPUT — that is the whole of the rewrite, and it is why `wiring_from_` alone
    // was never enough to say what a drop meant.
    u32 wiring_socket_ = 0;
    // How many cells tall each shown box is: a title and one row per socket. The layout reserves
    // every cell it covers, so a tall box cannot land on anything and nothing can land on it.
    std::vector<u32> node_tall_;
    // --- the wires the LANGUAGE has and the document does not spell out ---------------------------
    //
    // A `paint` coats the solid. A `weather` weathers it. A `variation` varies it. None of those
    // three NAMES the solid — the language applies them to whatever the document ends up being — so
    // none of them had a wire, and the picture came out as a row of statements each joined to the
    // file and to nothing else. Reported twice: *i only see everything separately connected to
    // weather demo instead of one node connected to another*, and then *not every node should be
    // connected to the final node as this doesnt make much sense*.
    //
    // So the relationship the language has is drawn even though no name carries it. These wires are
    // dimmer and have no tab to cut, because there is nothing in the file to cut: they are what the
    // words MEAN rather than what the document says.
    std::vector<std::vector<u32>> implied_;
    // And how many across. Two, always: a socket row is a name, a value and a dot, and at one
    // cell `translucent` and its value came out on top of one another.
    std::vector<u32> node_wide_;
    u32 cells_tall(u32 index) const;
    u32 cells_wide(u32 index) const;
    // One property of one node as a slider, wherever it is drawn. The panel on the left and the box
    // on the canvas are the same rows in two places, so they are the same code in one place.
    // `folding` puts the rows under headings that fold, which is what the settings window has done
    // since D485 and for the same reason: a panel that shows every control it has at once is a
    // panel a player reads rather than uses. The BOX on the canvas draws them all — it is already
    // exactly as tall as its rows and folding one would leave a hole in the picture.
    //
    // Returns how many rows it drew, so a caller laying out around it knows where it ended.
    usize draw_property_rows(const ClipNode& node, const Rect& area, const Rect& clip_to,
                             f32 row_height, u64 salt, bool folding);
    // How many rows `draw_property_rows` will draw, given the same `folding`. Asked before the
    // scroll region is opened, because a scroll needs its content height up front.
    usize property_rows_of(const ClipNode& node, bool folding) const;
    // The head word of the chosen node, as a control: pressing it lists what that node could be
    // instead and choosing one rewrites the word. See `clip_heads_like` for what "could be" means.
    void draw_head_choice(const Rect& row, const ClipNode& node);
    // Hollow, and how far it is stretched along each axis: two things the LANGUAGE has as
    // operations (`shell { }` and `scale { }`) and a node's settings did not offer. A row that goes
    // off nought wraps the statement in the word the language uses; a row that comes back to nought
    // takes the wrapper off again, so the document says what a person would have typed.
    usize draw_shape_extras(const ClipNode& node, const Rect& area, const Rect& clip_to,
                            f32 row_height);
    bool head_menu_door_ = false;   // the open head menu is listing FILES rather than words

    // Where the box that stands for the FILE sits, and whether there is one.
    //
    // Outside anything, every box on the canvas is a statement nothing else uses (D769) — and a
    // reader looking at seven of them side by side asked the obvious question: *the weatherings
    // aren't seemingly connected to anything.* They are: they are connected to the document. So the
    // document gets a box at the right and everything that is an answer wires into it, which is
    // both true and the difference between a diagram and a list.
    f32 doc_x_ = 0.0f;
    f32 doc_y_ = 0.0f;
    bool doc_shown_ = false;
    // And it can be picked up like anything else. Where it was put is written into the document as
    // its own `#@` comment, because a layout that does not travel with the file is state the game
    // keeps about a file, and §4 forbids that (D445).
    u32 doc_tall_ = 1;
    bool dragging_doc_ = false;
    f32 doc_grab_x_ = 0.0f;
    f32 doc_grab_y_ = 0.0f;
    f32 doc_drag_x_ = 0.0f;
    f32 doc_drag_y_ = 0.0f;

    // What the keys a node editor owes do. The clipboard is the SYSTEM's, and what goes on it is
    // the statements as text — see `game/clip_graph.hpp`.
    void copy_chosen_nodes(bool cut);
    void paste_nodes(f32 x, f32 y);
    // Drags the canvas when a band or a text selection is pulled past the edge of its window, so a
    // selection can be longer than the window is.
    void pan_at_edge(const Rect& area, f32& pan_x, f32& pan_y, f32 rate);
    // Go into a box: from here on the canvas shows that box and what it is made of, and nothing
    // else. Coming back out is `leave_document`, which is one control for both journeys.
    void go_inside(u32 index);
    // Everything that has to be walked into for this one to be on screen. What choosing a node by
    // name has to do before the node can be looked at.
    void reveal(u32 index);
    // The editor is given room the first time it is opened. See the cpp for why once and why wider.
    void give_editor_room();
    // Everything the graph can do, at the pointer: the palette, and what one node can be told.
    void draw_graph_menu(const Rect& canvas);
    // What is typed into the add menu's search. Kept between frames because the menu is redrawn
    // from nothing every one of them, and cleared when the menu closes.
    //
    // *Add a search bar for the right click list for nodes, i cant find the coat node.* Eighty
    // words in ten folds is a palette you have to already know the shape of; a search is how you
    // find a word you know the NAME of, and `clip_head_matches` is how you find one you only know
    // the job of.
    std::string palette_search_;
    // --- a part: another clip, or a material ---------------------------------------------------
    //
    // *There is no node for adding a clip into the editor either an empty one or one from your clip
    // library (it should work the same for materials).* Reported directly, and it was the one thing
    // the graph could not do that the language can: `include` is how a document is made of other
    // documents, and every world the game ships is one.
    //
    // A part from the library is **copied beside the document** rather than pointed at where it
    // lives. That is not a shortcut: an include resolves beside the file doing the including and
    // then in the game's own clips, and nowhere else — so a world that pointed at the player's own
    // shelf would open on their machine and on nobody else's. Copying is what makes a document
    // something you can send.
    //
    // 0 nothing, 1 a clip, 2 a material. What it is called is asked for at once (D773).
    u32 making_ = 0;
    std::string making_buffer_;
    bool making_focus_ = false;   // put the caret in it on the frame it appears
    // What the part menu is offering, captured when it opens: a listing re-read every frame is a
    // menu whose third item is a different file by the time it is pressed (D488).
    std::vector<std::filesystem::path> part_choices_;
    void gather_parts(u32 kind);
    // Makes the file, beside the document, and writes the `include` that reads it. Empty on
    // success, one line saying what happened otherwise.
    std::string make_part(u32 kind, const std::string& name);
    std::string take_part(const std::filesystem::path& from);
    void draw_part_naming(const Rect& canvas);
    // After anything that changed the document from the visual view. `why` empty means it worked;
    // anything else is the one line the refusal says.
    void document_changed(const std::string& why);
    // Where an `include` points, by the rule the game resolves one with: beside the file that says
    // it, then the clips the game ships with. Empty when it is nowhere, which is a world whose
    // parts have been deleted and is a thing that has happened three times.
    std::filesystem::path follow_include(const std::string& named) const;
    // One spelling of a path, so "is this the file already open" has an answer. See the cpp.
    std::filesystem::path same_file(const std::filesystem::path& path) const;
    // What to call a file on screen: its own name, and the folder too when that is what tells it
    // apart from another of the same name.
    std::string shown_name(const std::filesystem::path& path) const;
    // The selection follows the library into the editor: choosing something and opening the editor
    // is how a player asks to edit it, and an editor that then says "open something first" has
    // asked the question they just answered.
    void follow_selection();
    // Whether what is on disk is still what was read into `lines_`. False when the file has been
    // written, replaced, or deleted and made again under the same name.
    bool document_is_current() const;
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
    std::filesystem::path painting_;
    // Set when the file the tool is building with has just been written, so the verdict can ask for
    // it to be read again. *Changing the properties of a material while you have it equipped should
    // change its properties live so that you dont need to switch to another material then back.*
    bool repaint_ = false;

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
    // The editor has been given room once this session. See draw_library for why once.
    bool widened_for_editor_ = false;
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

    // The editor tab. It asks for a file before it opens (D455) — but a file already CHOSEN in
    // the library is an answer to that question, so this is empty only when nothing is selected
    // and nothing has been opened.
    std::filesystem::path editing_;
    // It came with the game rather than off the player's own shelf, so it opens and does not save
    // (D494). Worked out from where the file IS rather than from the row it was opened through,
    // because it is opened four ways and a flag set in three of them is a file written to in the
    // fourth.
    bool editing_shipped_ = false;
    // When the file was last written, as the file system reported it at the moment it was read.
    //
    // "Is this the document already open" was answered by the PATH alone, and a path is not a file:
    // delete `untitled.wsworld` while it is open, make a new one, choose it — same path, so nothing
    // reloaded, and what was on the screen was the deleted world. Saving then wrote the dead world
    // back over the new one. A name is not an identity and neither is a path; what makes this
    // answerable is asking the file whether it is still the one that was read.
    std::filesystem::file_time_type editing_stamp_{};
    u64 editing_bytes_ = 0;
    std::vector<std::string> lines_;
    u32 caret_line_ = 0;
    u32 caret_column_ = 0;
    // --- undo, and the one thing it is not ------------------------------------------------------
    //
    // The whole document, kept before each change. Not a list of edits: an edit here is a slider
    // moving three bytes, a node being taken out of the middle of a file, a paste of twenty lines
    // and a keystroke, and a scheme that could undo all four would be four schemes. A clip is a few
    // tens of kilobytes and this keeps sixty-four of them, which is a megabyte in the worst case in
    // the repository and nothing in the usual one.
    //
    // Changes GROUP: typing a word is one undo rather than five, because the state is only filed
    // when the reason changes or when a moment has passed since the last one. That is the whole of
    // why `remember` takes a reason.
    struct DocumentState {
        std::vector<std::string> lines;
        u32 caret_line = 0;
        u32 caret_column = 0;
    };
    std::vector<DocumentState> undo_;
    std::vector<DocumentState> redo_;
    std::string undo_reason_;
    f64 undo_stamped_ = -1.0e9;
    void remember(const char* reason);
    void step_back();      // ctrl-Z
    void step_forward();   // ctrl-Y, and ctrl-shift-Z, which is the other half of the world

    // Whether the file on disk began with a UTF-8 byte-order mark, so that saving it puts one
    // back. Three invisible bytes that every Windows text editor writes by default; the parser
    // now skips them and the editor must not silently drop them, which is the same promise the
    // trailing newline is made of.
    bool began_with_mark_ = false;
    // Whether the file on disk ended with a newline, so that saving it puts one back.
    //
    // `std::getline` cannot tell "a\nb\n" from "a\nb" — both give two lines — so opening any of
    // this repository's clips and saving it took the last byte off. That is a document the editor
    // changed without being asked to, which is the one thing it must never do (D745).
    bool ended_with_newline_ = true;
    // Where a selection started, which is the caret's other end.
    //
    // A text view without one is a text view a player can only edit a character at a time — no
    // double-click on a word, no dragging across a line, no ctrl-A, and nothing for ctrl-C to take.
    // `mark_line_`/`mark_column_` is the anchor and the caret is the moving end, so a selection is
    // whatever lies between them and there is no third state to keep in step.
    u32 mark_line_ = 0;
    u32 mark_column_ = 0;
    bool selecting_ = false;   // the hand is down and dragging one out
    // When typing here began, which is what the caret's pulse is measured from — so it is bright at
    // the moment of the click rather than wherever the world clock happens to be.
    f64 caret_since_ = 0.0;
    // How far the script view is scrolled sideways, and how wide its widest line is. A clip's
    // comments are sentences and a docked panel is a quarter of a screen, so a document without a
    // way across it is a document read twenty characters at a time.
    f32 script_pan_x_ = 0.0f;
    f32 script_wide_ = 0.0f;
    // It moved, so the view has to bring it back into sight. Only on a MOVE, because a view that
    // scrolled to the caret every frame is one that cannot be scrolled away from with the bar.
    bool caret_moved_ = false;
    bool dirty_ = false;
    ParseReport report_{};
    std::function<ParseReport(const std::string&, const std::filesystem::path&)> parse_;
    // When the verdict is next worth asking for, and what the last one cost. A parse of an ordinary
    // clip is a millisecond and a parse of a WORLD is the whole building — 22 ms to splice its
    // twenty-two pieces and 54 to read them — so "on every keystroke" is a promise that costs five
    // frames a letter on the one file kind this change made editable.
    bool parse_wanted_ = false;
    f64 parse_at_ = 0.0;
    f64 parse_cost_ = 0.0;   // seconds

    // Something else was selected while this document had unsaved changes. Nothing is thrown away
    // and nothing pops up: the editor says which file is waiting, and opens it the moment this one
    // is saved.
    std::filesystem::path waiting_;
    // The documents this one was entered FROM, innermost last.
    //
    // Double-clicking an `include` goes into that file, which is what a world is for — twenty lines
    // naming the pieces it is assembled out of. Before this there was no way back out of one except
    // finding it again on the shelf, which is not a way out of anything: a door you can only walk
    // one way through is a trapdoor.
    std::vector<std::filesystem::path> came_from_;

    // The same document as a graph, which is what the visual view draws. Re-read when the text
    // changes and not otherwise.
    ClipGraph graph_;
    bool graph_stale_ = true;
    // Which node is selected, by the key that survives a re-read — where its head is written, and
    // the name beside it. An index alone would be a different node by the time a drag's second
    // frame arrived, because the graph is re-read on the frame the drag writes.
    std::string chosen_node_;
    // Everything chosen, by the same keys, and the primary is the first of them. A selection of
    // one is the ordinary case and is what the parameters window on the left is about; a selection
    // of several is what a dragged box makes, and is what *duplicate* and *take out* act on.
    std::vector<std::string> chosen_nodes_;
    std::vector<u32> chosen_set_;
    // And what that key resolved to in the graph as it stands, worked out once when the graph is
    // re-read. Three separate places wanted it every frame — the panel on the left, the box drawn
    // lit on the right, and the test that decides which of the two the left-hand window is — and
    // each of them was a walk over every node building a key string for it, which on a fragment of
    // seven hundred nodes is two thousand strings a frame to answer one question.
    u32 chosen_index_ = ClipGraph::kNone;
    // Where each of the selected node's sliders may travel, worked out once when it is selected. A
    // range recomputed from the value every frame is a handle that never leaves the middle.
    std::vector<std::pair<f64, f64>> node_range_;
    // --- what is on screen, which is most of what makes a graph readable ---------------------
    //
    // A clip is a hundred and thirty boxes and a panel is a quarter of a screen, and no amount of
    // laying out makes those two numbers agree. So a document does not show all of itself: it shows
    // its ANSWERS — the statements nothing else uses — and everything under one of them appears
    // when that one is opened. A fold, exactly as a settings panel is a fold (D485), and for the
    // same reason: *a panel that shows every control it has at once is a panel a player reads
    // rather than uses.*
    //
    // **It is a focus and not a fold** (D772). The fold opened a box in place and everything under
    // it appeared beside everything else, which on a document of any size is the wall it was
    // supposed to prevent — reported as *instead of cascading just show you only the nodes inside
    // of it*. So going into a box shows that box and WHAT IT IS MADE OF, one level, and nothing
    // else; going into one of those goes a level further; and the way back is the same control that
    // comes back out of a file.
    //
    // Where you are is view state and lives here rather than in the file. A `#@` position is
    // AUTHORED and travels with the document (D756); where somebody happens to be looking is the
    // same class of thing as a scroll offset.
    std::vector<std::string> inside_;
    std::vector<bool> node_shown_;
    std::vector<std::vector<u32>> used_by_;

    // Where every box sits, in cells, in step with `graph_.nodes`. Recomputed only when the
    // document is re-read, because it is a sort per column and this is asked sixty times a second.
    std::vector<f32> graph_x_;
    std::vector<f32> graph_y_;
    f32 graph_wide_ = 0.0f;
    f32 graph_tall_ = 0.0f;

    f32 graph_pan_x_ = 0.0f;
    f32 graph_pan_y_ = 0.0f;
    // How big the boxes are drawn. The wheel moves it about the pointer; a document opens fitted to
    // the window, because eighty boxes at full size is three boxes and a lot of grey.
    f32 graph_zoom_ = 1.0f;
    bool graph_fitted_ = false;
    bool dragging_graph_ = false;

    // The box being carried, and where the hand took hold of it — in cells, so the box does not
    // jump to the pointer on the first frame of the drag.
    u32 dragging_node_ = 0xFFFFFFFFu;
    f32 drag_grab_x_ = 0.0f;
    f32 drag_grab_y_ = 0.0f;
    f32 drag_at_x_ = 0.0f;
    f32 drag_at_y_ = 0.0f;
    // The rest of the choice, and where each of them sits relative to the one in hand. Choosing
    // four things and then moving one of them out from under the other three is not what a box
    // round four things meant.
    std::vector<std::pair<u32, std::pair<f32, f32>>> drag_with_;
    // The wire being drawn, out of this box and toward the pointer.
    u32 wiring_from_ = 0xFFFFFFFFu;

    // What the graph's menu is about — a box, or the canvas — and, when it is the canvas, which
    // group of the palette it is showing and where in the layout a new box would land.
    u32 menu_about_ = 0xFFFFFFFFu;
    i32 palette_group_ = -1;
    f32 menu_x_ = 0.0f;
    f32 menu_y_ = 0.0f;
    // Which node `node_range_` was worked out for. The ranges must NOT be recomputed when the
    // document is re-read, which happens on every frame of a drag — a handle whose travel is
    // recentred on the value it is showing never leaves the middle of its own slider.
    std::string range_node_;

    std::string document() const;
    void open_document(const std::filesystem::path& raw);
    // Go INTO a document, remembering the one being left so there is a way back.
    void enter_document(const std::filesystem::path& path);
    void save_document();
    void edit_keys(const InputState& input);
    // Where the caret goes. Split out because a document being READ moves its caret too, and a
    // built-in is read and not written (D494).
    void move_caret(const InputState& input);
    // Whether anything is chosen, and where it runs from and to in document order.
    bool has_selection() const;
    void selection_span(u32& from_line, u32& from_column, u32& to_line, u32& to_column) const;
    // What is chosen, as text, and taking it out again. Both work on the span above.
    std::string selected_text() const;
    void erase_selection();
    // Put the anchor where the caret is: what every movement that is not a selection does.
    void drop_mark();
    // Where a word begins and ends on a line, for the double-click.
    static void word_at(const std::string& line, u32 column, u32& from, u32& to);
    // Ask for the verdict again. Not at once: see `reparse` in the cpp for why a parse on every
    // keystroke is a promise this had to stop keeping literally in order to keep at all.
    void reparse_soon();
    void reparse();

    f64 seconds_ = 0.0;
};

}  // namespace ws::ui
