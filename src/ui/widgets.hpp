#pragma once
// The controls, and the three rules that decide what every one of them does.
//
// documentation/14-ui-style.md and documentation/23-shell-and-libraries.md are the specification.
// The parts of it that live HERE, rather than in the shader or in the layout, are:
//
//   **The icon is the control and the label is the fallback.** When a button runs out of room the
//   drawing gives up room before the word does and never gives up all of it — it shrinks to about
//   0.55 of nominal and stops, and only then does the word start shrinking. A button with no
//   drawing at all is not a state this can reach.
//
//   **Every numeric value is a slider, a double-click types into it, and a typed value is not
//   capped** (D444). The slider's range is where the handle can go, not where the value can. A
//   value that would break the game is refused *and says in one line what it would have done*,
//   because a refusal that does not explain itself is indistinguishable from a bug.
//
//   **Motion answers a press, and nothing else.** Resting a pointer on a control must not run its
//   animation: crossing a settings panel to reach one row would otherwise make every row on the
//   way report as though it had been used. The exception, and it is not one, is the *tooltip* — a
//   pointer resting for about a third of a second gets one sentence, because that is the one place
//   in this interface with words in it and asking for it is what a hover is.
//
// # Why immediate mode
//
// There is no retained widget tree, no callbacks and no invalidation. A frame states what the
// interface is; the identity of a control is a number the caller derives, and the only thing that
// survives a frame is which control is being dragged, which is being typed into, and how far
// through its animation each is. A shell whose windows are a state machine over three states does
// not need a scene graph, and a scene graph is where an interface's second source of truth lives.

#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "core/hash.hpp"
#include "platform/window.hpp"
#include "ui/draw.hpp"
#include "ui/ink.hpp"
#include "ui/sound.hpp"

namespace ws::ui {

// An identity for a control. Derived from a name and, where a control is one of many alike, an
// index — a row in a list, an option in a group. Two controls with the same id are one control as
// far as dragging and typing are concerned, which is a bug that shows up as two sliders moving
// together and is worth being able to spell.
inline u64 id_of(std::string_view name, u64 index = 0) {
    u64 h = 0xCBF29CE484222325ull;
    for (char c : name) h = hash_combine(h, static_cast<u64>(static_cast<u8>(c)));
    return hash_combine(h, index + 1);
}

// How big everything is. One number: device pixels per interface pixel, worked out from the
// window's SHORT side so a layout keeps its proportions on a wide monitor rather than growing
// until the column runs off the sides.
struct Metrics {
    f32 scale = 2.0f;

    f32 px(f32 interface_pixels) const { return interface_pixels * scale; }
    // The text size a row is set at: two glyph pixels per interface pixel, which puts a capital at
    // ten interface pixels. The loading screen already settled this number.
    f32 text() const { return scale * 2.0f; }
    f32 small_text() const { return scale * 1.0f; }
    f32 row() const { return px(22.0f); }
    f32 gap() const { return px(6.0f); }
    f32 pad() const { return px(10.0f); }
    f32 icon() const { return px(16.0f); }
};

// What a control that refuses a value says about it. Empty means the value is fine.
using Guard = std::function<std::string(f64)>;

// What a slider is about, so that one call carries the whole row rather than nine arguments in an
// order nobody can remember.
struct Number {
    std::string_view label;
    std::string_view tooltip;
    f64 low = 0.0;         // where the handle starts
    f64 high = 1.0;        // and where it ends. NOT where the value can go: see D444
    f64 step = 0.0;        // 0 is continuous
    i32 decimals = 2;
    std::string_view unit;
    Guard guard;           // says in one line what a refused value would have done
};

class Ui {
public:
    Ui();

    // --- the frame --------------------------------------------------------------------------
    void begin(const InputState& input, u32 width, u32 height, f64 seconds, f32 scale);
    // Draws whatever the frame asked to be drawn last — the tooltip and any refusal — and bins
    // the list. Nothing may be added after this.
    void end();

    DrawList& draw() { return draw_; }
    const DrawList& draw() const { return draw_; }
    Sound& sound() { return sound_; }
    const Metrics& metrics() const { return metrics_; }
    Rect screen() const { return Rect{0.0f, 0.0f, static_cast<f32>(width_), static_cast<f32>(height_)}; }
    f64 seconds() const { return seconds_; }

    // Whether the shell has taken the pointer or the keyboard this frame, so the game underneath
    // knows not to also act on them. Asked AFTER `end`.
    bool wants_mouse() const { return wants_mouse_; }
    bool wants_keys() const { return typing_ != 0; }
    // Whether the platform should be composing text. A field with focus asks for it implicitly;
    // anything else that eats characters — the script view, which is not a widget — asks out loud.
    bool wants_text_input() const { return typing_ != 0 || asked_for_text_; }
    void request_text_input() { asked_for_text_ = true; }

    // --- surfaces ---------------------------------------------------------------------------
    //
    // A panel is the world behind it, blurred, and a hairline edge. It has no other substance,
    // because it has no colour of its own to be made out of.
    void panel(const Rect& rect, f32 strength = 1.0f);
    void divider(const Rect& rect, bool vertical = false);
    void label(const Rect& row, std::string_view text, Align align = Align::Left,
               u32 style = kPlain, f32 coverage = 1.0f);
    // A block of markdown, laid out and drawn. Returns the height it took, so a caller can
    // measure before it commits room — which is what a description in a library entry needs.
    f32 markdown(const Rect& box, std::string_view text, bool measure_only = false);

    // --- controls ---------------------------------------------------------------------------
    //
    // Every one of these answers on the RELEASE inside its own rectangle, because a press that
    // slides off a button and lets go is a press somebody changed their mind about.
    // `stacked` puts the drawing ABOVE the word instead of beside it, which is what a title
    // button is: the one place with room enough that the icon does not have to give any up.
    bool button(u64 id, const Rect& rect, Icon icon, std::string_view label,
                std::string_view tooltip = {}, bool stacked = false);
    // The same, with no word at all: for a row of tools where the rectangle is one icon wide.
    bool icon_button(u64 id, const Rect& rect, Icon icon, std::string_view tooltip = {});
    // A switch is a switch. Not a slider, and not two icons: it is one thing with two states and
    // there is nothing to choose between.
    bool toggle(u64 id, const Rect& rect, std::string_view label, bool& value,
                std::string_view tooltip = {});
    // One of several is icons, and each option may say its own sentence — three buttons on a row
    // are three questions and the row's description can only answer the first.
    bool choice(u64 id, const Rect& rect, std::string_view row_label, const Icon* icons,
                const std::string_view* labels, const std::string_view* tooltips, u32 count,
                u32& value, std::string_view tooltip = {});
    // The slider, with everything D444 asks of it.
    //
    // Returns true on every frame it WROTE to `value` — which is every frame of a drag, not only the
    // one the hand comes off on. A caller copies what came back into wherever the setting actually
    // lives, so a slider that only reported on release was a slider whose every drag was discarded
    // and whose handle went back where it started. That was the bug, and this is the contract that
    // makes it impossible: if the value moved, the caller is told, every frame.
    bool number(u64 id, const Rect& rect, const Number& about, f64& value);
    // A name is a name. Returns true on the frame it is committed with enter; `value` is only
    // written then, so escaping out of a field leaves what was there.
    bool field(u64 id, const Rect& rect, std::string& value, std::string_view placeholder = {},
               std::string_view tooltip = {});
    // A row of tabs, each with a drawing and a word.
    bool tabs(u64 id, const Rect& rect, const Icon* icons, const std::string_view* labels,
              u32 count, u32& active);

    // A press with nothing drawn for it: the hit box, the tooltip and the sound, and not one mark.
    // The logo is what this is for — it is a picture that answers a press, and a hover wash over it
    // would make the one picture on the screen into a control with a background.
    bool pressable(u64 id, const Rect& rect, std::string_view tooltip = {});

    // A bare drag: no label, no value spelled out, no notch sounded. This is what a docking band's
    // edge and a splitter between two windows are — they have nothing to say and nothing to
    // commit, they simply follow the hand. `extent` is what one whole unit of `value` is worth in
    // device pixels; `inverted` is for an edge that grows as the pointer moves the other way.
    // Returns true on any frame it moved.
    bool drag_handle(u64 id, const Rect& rect, bool along_x, bool inverted, f32 extent,
                     f64& value);

    // The raw pointer, for the few things that are about the whole screen rather than about a
    // rectangle — carrying a window to another edge is the only one so far.
    f32 pointer_x() const { return input_.mouse_x; }
    f32 pointer_y() const { return input_.mouse_y; }
    bool pressed_in(const Rect& rect) const {
        return input_.mouse_left_pressed && rect.holds(input_.mouse_x, input_.mouse_y);
    }
    bool released() const { return input_.mouse_left_released; }
    const InputState& input() const { return input_; }

    // Whether the player did ANYTHING this frame — moved the pointer, pressed a button or a key,
    // scrolled, typed. Not "did they use a control": the one thing that asks is the logo's idle
    // clock (src/ui/logo.hpp), and what it needs to know is whether somebody is there at all.
    bool touched() const;

    // --- scrolling --------------------------------------------------------------------------
    //
    // Begins a clipped region; everything drawn until `end_scroll` is offset by the scroll and cut
    // to the rectangle. Returns the rectangle to lay content out in, whose top is already moved.
    Rect begin_scroll(u64 id, const Rect& rect, f32 content_height);
    void end_scroll();
    f32 scroll_of(u64 id) const;

    // --- selection ---------------------------------------------------------------------------
    //
    // The rubber band, which is Explorer's and deliberately so: an interface that spends its
    // novelty budget on *selecting things* has spent it in the worst possible place. `begin_band`
    // is called with the empty space of a list; it reports the box while it is being dragged.
    bool band(u64 id, const Rect& rect, Rect& box, bool& finished);

    // --- the one place there are words --------------------------------------------------------
    //
    // Claimed by whichever control the pointer is resting on. Drawn once, at the end, over
    // everything, because a tooltip belongs to the screen rather than to the window it came from.
    void tooltip(const Rect& over, std::string_view sentence);

    // A one-line refusal, said where the thing that refused is, for a couple of seconds. The
    // second half of D444: a refusal that does not explain itself reads as a bug.
    void refuse(const Rect& at, std::string_view why);

    // Whether anything is being dragged or typed into, which is how a caller knows a click on the
    // world behind is not meant for the world.
    bool busy() const { return active_ != 0 || typing_ != 0; }
    void stop_typing();

    // The player's own colour, used where inversion is blind, and the logo's, which is the one
    // surface allowed a hue of its own.
    void set_accent(const Colour& accent) { accent_ = accent; }
    const Colour& accent() const { return accent_; }

private:
    struct Anim {
        f32 press = 0.0f;    // 1 on the frame of a press, decaying: motion answers a press
        f32 shown = 0.0f;    // where a handle is drawn, which chases where the value is
        bool shown_valid = false;
        u64 touched = 0;
    };
    Anim& anim(u64 id);
    // The pointer is over this and nothing above it. Also the place tooltip timing is decided.
    bool hovering(u64 id, const Rect& rect);
    void press_started(u64 id);
    // What a value reads as on a row: the number, its unit, and no more decimals than it earns.
    std::string spell(f64 value, const Number& about) const;
    // The label and drawing of a control, laid out by the rule at the top of this file.
    // `forced_size` overrides the row's own text size, which is what a row of tabs needs: three
    // words of three lengths each shrinking to fit their own cell come out at three different
    // sizes, and a row of mixed sizes reads as a mistake rather than as a fit.
    void icon_and_label(const Rect& rect, Icon icon, std::string_view text, f32 phase,
                        f32 coverage, bool centred, f32 forced_size = 0.0f);
    // The one size at which every one of `count` labels fits its share of `rect`.
    f32 shared_size(const Rect& rect, const std::string_view* labels, u32 count) const;

    DrawList draw_;
    Sound sound_;
    Metrics metrics_;
    Colour accent_{0.0f, 1.0f, 0.0f};

    InputState input_{};
    u32 width_ = 0;
    u32 height_ = 0;
    f64 seconds_ = 0.0;
    f32 dt_ = 1.0f / 60.0f;
    u64 frame_ = 0;

    u64 hot_ = 0;        // under the pointer this frame
    u64 was_hot_ = 0;    // and last frame, which is what a rest is measured against
    u64 active_ = 0;     // taking a drag
    u64 typing_ = 0;     // taking characters
    f64 hover_since_ = 0.0;
    bool wants_mouse_ = false;
    bool asked_for_text_ = false;

    // What is being typed, kept here rather than in the caller's string so that escaping out of a
    // field leaves the value it had. Committed on enter and on nothing else.
    std::string buffer_;
    usize caret_ = 0;
    f64 caret_since_ = 0.0;
    bool buffer_all_selected_ = false;

    // Where a drag started and what the value was then, so a slider tracks the pointer's movement
    // rather than jumping the handle to wherever the press landed.
    f32 grab_x_ = 0.0f;
    f64 grab_value_ = 0.0;

    struct Pending {
        Rect over{};
        std::string text;
        bool set = false;
    };
    Pending tooltip_{};
    struct Refusal {
        Rect at{};
        std::string text;
        f64 until = 0.0;
    };
    Refusal refusal_{};

    // Scroll offsets and rubber bands survive a frame; nothing else does.
    std::unordered_map<u64, f32> scrolls_;
    std::unordered_map<u64, Rect> bands_;
    std::unordered_map<u64, Anim> anims_;
    std::vector<f32> scroll_stack_;
};

}  // namespace ws::ui
