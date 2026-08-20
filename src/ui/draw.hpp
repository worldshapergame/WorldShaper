#pragma once
// What the shell hands the card: a list of marks, and which tiles of the screen each one touches.
//
// # Why a list and not a set of draw calls
//
// Every mark in this interface is the same operation — take what is behind this pixel and decide
// what colour a mark on it has to be (documentation/14-ui-style.md, the ink rule). That is one
// compute shader, and the only thing that varies between a panel, a label, an icon and a slider's
// handle is the SHAPE the coverage comes from. So the host says where the shapes are and the card
// says what colour they come out; nothing here holds a colour except the five the style permits.
//
// # Why the tiles
//
// The style document prices the ink at about forty-six nanoseconds a pixel and the budget for the
// whole interface at 0.6 ms (documentation/09-performance-budgets.md), so running it over a 1440p
// screen would spend six milliseconds deciding that nothing is there. The rule it states is *run
// the ink only over panel rectangles, never the full screen*, and this is how: the host bins every
// mark into sixteen-pixel tiles, the dispatch covers only the tiles that have something in them,
// and a pixel walks its own tile's short list rather than the whole document.
//
// # The order is the compositing order
//
// A pixel evaluates its tile's commands in the order they were added, so a label added after the
// glass it sits on reads the glass as its backdrop and inverts against THAT rather than against
// the world two layers down. That is not a convenience — it is the whole reason the percentage on
// the loading bar inverts as the bar passes under it, per pixel, with no branch anywhere.

#include <string>
#include <string_view>
#include <vector>

#include "core/types.hpp"
#include "ui/glyphs.hpp"
#include "ui/logo.hpp"

namespace ws::ui {

// A rectangle in device pixels, x to the right and y down, the way the swapchain is indexed.
struct Rect {
    f32 x0 = 0.0f;
    f32 y0 = 0.0f;
    f32 x1 = 0.0f;
    f32 y1 = 0.0f;

    f32 width() const { return x1 - x0; }
    f32 height() const { return y1 - y0; }
    f32 mid_x() const { return (x0 + x1) * 0.5f; }
    f32 mid_y() const { return (y0 + y1) * 0.5f; }
    bool empty() const { return x1 <= x0 || y1 <= y0; }
    bool holds(f32 x, f32 y) const { return x >= x0 && x < x1 && y >= y0 && y < y1; }

    Rect inset(f32 by) const { return Rect{x0 + by, y0 + by, x1 - by, y1 - by}; }
    Rect clip_to(const Rect& other) const {
        return Rect{(x0 > other.x0) ? x0 : other.x0, (y0 > other.y0) ? y0 : other.y0,
                    (x1 < other.x1) ? x1 : other.x1, (y1 < other.y1) ? y1 : other.y1};
    }
};

// What a mark is. The card has one branch on this and nothing else.
enum class Mark : u32 {
    Glass = 0,   // the world behind, blurred: a panel's whole background and its only substance
    Ink = 1,     // the inverse of what is behind, at a coverage: fills, dividers, handles, carets
    Text = 2,    // a run of characters, in one style
    Icon = 3,    // one of the drawings below, in its own cell
    Hue = 4,     // one of the five permitted colours, stated rather than inverted
    Edge = 5,    // a hairline border inside `rect`, so an edge is exact rather than a fade
    // The logo mark: the one SURFACE allowed a hue of its own, because it is a picture rather than
    // furniture (documentation/14-ui-style.md, the five permitted colours). It is the only mark
    // here that is animated by the clock rather than by a press, for the same reason.
    Logo = 6,
};

// The drawings. An icon is the control and the label is the fallback, so this list is the
// interface's actual vocabulary — each one is a picture of what the thing DOES, and each animates
// to say it did it rather than to say a pointer is over it.
//
// Drawn on the card, in `shaders/shell.comp`, on a thirteen by thirteen cell so they belong to the
// same alphabet as the letters beside them. Adding one is a function there and a name here.
enum class Icon : u32 {
    None = 0,
    World,       // a sphere of voxels: the worlds library, and a world file
    Settings,    // three sliders at three settings
    Library,     // a shelf of upright things
    Community,   // three figures, the ones you can reach
    Editor,      // lines of text with a caret
    Folder,      // and its lid opens when it is entered
    Up,          // out of this folder, into the one above
    New,         // a plus
    Rename,      // a pen over a line
    Duplicate,   // one square splitting into two — the animation is the split
    Delete,      // a lid opening over a bin — the animation is the opening
    Trash,       // where a delete goes, as a shelf of its own
    Sort,        // three bars, longest first
    Search,      // a lens
    Private,     // an eye, closed
    Shared,      // an eye, open
    Close,       // a cross
    Clip,        // a small solid made of voxels: a clip file
    Material,    // a swatch of pattern
    Mod,         // a block with a socket
    Character,   // a standing figure
    Script,      // a page with a chevron on it
    Tick,        // the mark a switch makes when it is on
    Play,        // enter this world
    Reset,       // an arrow going back round: put this value back where it started
    Collapsed,   // a section that is folded away, pointing at what would open
    Expanded,    // and one that is open, pointing at what is under it
    Pattern,     // a wave: a field read for its value rather than its sign, which is what every
                 // grain, checker and stripe in a clip is
    Graph,       // two nodes and the wire between them: the visual view of a document
    Count,
};

// One instruction, as the card reads it. Sixty-four bytes, and the layout is duplicated field for
// field in shaders/shell.comp — a mismatch here is a picture that is wrong in a way no compiler
// can see, so the struct there carries the same comment and a static assertion guards the size.
struct Command {
    f32 rect[4]{};   // what the mark covers
    f32 clip[4]{};   // and the window it is inside, so a list can scroll under its own header
    u32 kind = 0;
    u32 a = 0;       // Text: the first character.  Icon: which.  Hue/Logo: 0xRRGGBB
    u32 b = 0;       // Text: how many.  Logo: the seed
    u32 style = 0;   // Text: the style bits from ui/glyphs.hpp.  Logo: the seed before this one
    f32 f0 = 0.0f;   // coverage, everywhere it means anything
    f32 f1 = 0.0f;   // Text: the pen's x.  Icon: the animation's phase, 0 to 1.
                     // Logo: where its two frames start in the character buffer
    f32 f2 = 0.0f;   // Text: the capitals' top y.  Logo: seconds since the seed was chosen
    f32 f3 = 0.0f;   // Text: device pixels per glyph pixel.  Edge: its thickness.
                     // Logo: how far through the morph out of the seed before it, 1 when settled
};
static_assert(sizeof(Command) == 64, "shaders/shell.comp reads this layout word for word");

// Where a run of text sits against the point it was given.
enum class Align : u32 { Left, Centre, Right };

// The screen, cut into squares, so a pixel only ever walks the marks that reach it.
inline constexpr u32 kTilePixels = 16;

class DrawList {
public:
    // The size of the surface being drawn on, which is what the tiles are cut from. Clears
    // everything: a draw list is built from nothing every frame, because an interface that
    // remembers what it drew last frame is an interface with two truths in it.
    void begin(u32 width, u32 height);

    // --- the marks --------------------------------------------------------------------------
    //
    // Every one takes the current clip rectangle, which `push_clip`/`pop_clip` maintain, so a
    // caller never has to pass one and can never forget to.

    void glass(const Rect& rect, f32 strength = 1.0f);
    void ink(const Rect& rect, f32 coverage = 1.0f);
    void edge(const Rect& rect, f32 coverage = 1.0f, f32 thickness = 1.0f);
    void hue(const Rect& rect, u32 rgb, f32 coverage = 1.0f);
    // `phase` is 1 at REST and 0 at the press, and it is 1 by default because most icons in this
    // interface are not on a control at all — a window's header, the kind of a file in a listing —
    // and those must be drawn standing still. It defaulted to 0, which is the fully pressed pose,
    // so every icon that was not on a button has been drawn mid-animation since the shell existed:
    // the header's bin had its lid open, the folder in a row was ajar, and the tick was a tick that
    // had not been drawn yet. None of it looked like a bug because nothing ever showed the two
    // poses side by side, which is exactly what `--icon-sheet` is for.
    void icon(const Rect& cell, Icon which, f32 coverage = 1.0f, f32 phase = 1.0f);
    // The mark, animated by the clock. There is exactly one of these on screen at a time and it is
    // on the title.
    //
    // `rgb` is the player's chosen logo hue and goes to the card as a BIAS on the palette rather
    // than as the answer — the seed still chooses, and their setting still shifts the family it
    // chooses from. `seed` is the combination, `previous` the one before it and `morph` how far
    // between them (1 when there is no change in progress); `age` is seconds since `seed` was
    // chosen, which is where an animation with a beginning begins. `src/ui/logo.hpp` keeps all four,
    // and `now` and `before` are what it decided for those two seeds this frame — the sorted order
    // of the mark's slices and the two weights, which travel in the character buffer because a
    // command is sixty-four bytes and a permutation is a hundred and sixty words.
    void logo(const Rect& cell, u32 rgb, u32 seed, u32 previous, f32 age, f32 morph,
              const LogoFrame& now, const LogoFrame& before);

    // A run of text, positioned by its CAPITALS rather than by its line box.
    //
    // `top` is where a capital's top row sits. A line box carries descender room whether the
    // string has descenders or not, which puts a word like `Fast` visibly high in its button with
    // an empty band beneath it; centring each string's own ink instead would fix that word and
    // break the column, because `Field of view` and `Haze distance (km)` would sit on different
    // baselines. A capital is the same box for every string at a given size, so that is what gets
    // centred. Returns the width it set, in device pixels.
    f32 text(f32 x, f32 top, std::string_view utf8, f32 pixels, u32 style = kPlain,
             Align align = Align::Left, f32 coverage = 1.0f);

    // --- clipping ---------------------------------------------------------------------------
    void push_clip(const Rect& rect);
    void pop_clip();
    const Rect& clip() const { return clips_.back(); }

    // --- what the card is given -------------------------------------------------------------
    //
    // `bin` has to be called once, after the last mark, and before any of the three below are
    // read. It is separate from the marks so that binning walks the finished list once rather
    // than growing a tile list per mark.
    void bin();

    const std::vector<Command>& commands() const { return commands_; }
    const std::vector<u32>& characters() const { return characters_; }
    // Two words a tile: where its list starts in `tile_indices`, and how long it is.
    const std::vector<u32>& tiles() const { return tiles_; }
    const std::vector<u32>& tile_indices() const { return tile_indices_; }

    u32 tiles_across() const { return across_; }
    u32 tiles_down() const { return down_; }

    // The union of everything drawn, snapped out to whole tiles: what the dispatch has to cover
    // and, deliberately, nothing more. Empty when nothing was drawn at all, which is the case the
    // caller skips the pass entirely for.
    const Rect& bounds() const { return bounds_; }
    bool empty() const { return commands_.empty(); }

    // --- measuring, which the layout needs before it can place anything ----------------------
    //
    // In device pixels, at `pixels` device pixels per glyph pixel. The same arithmetic the card
    // draws with, from the same table, which is what makes a hit box land on its own drawing.
    static f32 measure(std::string_view utf8, f32 pixels, u32 style = kPlain);
    // The height of a capital, and of the whole cell including the descender's row.
    static f32 cap_height(f32 pixels) { return static_cast<f32>(kGlyphCap) * pixels; }
    static f32 line_height(f32 pixels) { return static_cast<f32>(kGlyphRows) * pixels; }
    // How much of `utf8` fits in `room`, in characters, and where the ellipsis would go. A list of
    // file names is mostly names that do not fit.
    static usize fits(std::string_view utf8, f32 room, f32 pixels, u32 style = kPlain);

private:
    void add(Mark kind, const Rect& rect, u32 a, u32 b, u32 style, f32 f0, f32 f1, f32 f2, f32 f3);

    std::vector<Command> commands_;
    std::vector<u32> characters_;
    std::vector<u32> tiles_;
    std::vector<u32> tile_indices_;
    std::vector<Rect> clips_;
    Rect bounds_{};
    u32 width_ = 0;
    u32 height_ = 0;
    u32 across_ = 0;
    u32 down_ = 0;
};

}  // namespace ws::ui
