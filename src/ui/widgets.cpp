#include "ui/widgets.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "game/markup.hpp"

namespace ws::ui {
namespace {

// How long a pointer has to rest before it is asking a question. A third of a second, which is
// the number documentation/14-ui-style.md gives and is long enough that crossing a panel to reach
// one row does not put a sentence on every row on the way.
constexpr f64 kRestSeconds = 0.33;
// And how far it may drift while resting. A hand on a mouse is never still; two device pixels is
// still, and twenty is on the way somewhere.
constexpr f32 kRestDrift = 3.0f;

// How long a refusal stays up. Long enough to read one line, short enough that it is gone before
// the next attempt.
constexpr f64 kRefusalSeconds = 2.5;

// How fast a handle catches up with its value. The style asks for motion that shows the value
// MOVING rather than just its new position, so the handle chases rather than snapping — and it
// has to arrive well inside the time it takes to look, or dragging feels like syrup.
constexpr f32 kChaseRate = 22.0f;

// How long a press animation runs. It is the drawing saying what it did, so it outlasts the click
// and not much more.
constexpr f32 kPressSeconds = 0.34f;

f32 chase(f32 have, f32 want, f32 dt) {
    const f32 t = std::min(1.0f, dt * kChaseRate);
    return have + (want - have) * t;
}

// What the drawing is actually given, out of the linear countdown the press keeps.
//
// `press` falls from 1 to 0 at a constant rate, and handing that straight to an icon was the whole
// of the animation: a drawing snapped to its pressed pose and then slid back at one speed, which
// on the screen reads as a slide rather than as an answer. The eye reads *acceleration*, not
// position — a thing that stops dead did not move, it was moved.
//
// So the countdown is eased, and the ease overshoots: the drawing leaves fast, comes home slowing,
// goes a tenth of the way PAST home and settles back. The overshoot is the part that costs nothing
// and does all the work, because it is the only part of the motion that says something elastic
// happened rather than something linear.
//
// The value returned is the MOTION — 1 on the press, 0 at rest, and briefly about -0.10 on the way
// home. `phase` is one minus it, so `phase` runs a little past 1 and every drawing in
// shaders/shell.comp is written to take a small negative motion as a counter-swing.
f32 press_motion(f32 press) {
    if (press <= 0.0f) return 0.0f;
    const f32 u = 1.0f - press;            // 0 the instant it was pressed, 1 when it is over
    const f32 s = u - 1.0f;
    // The standard back-ease, whose constant is the one that overshoots by about a tenth.
    constexpr f32 kBack = 1.70158f;
    return -((kBack + 1.0f) * s * s * s + kBack * s * s);
}

// And what the shader is handed: 1 at rest, 0 at the press, a little over 1 on the way home.
f32 press_phase(f32 press) { return 1.0f - press_motion(press); }

std::string trim_zeros(std::string text) {
    if (text.find('.') == std::string::npos) return text;
    while (!text.empty() && text.back() == '0') text.pop_back();
    if (!text.empty() && text.back() == '.') text.pop_back();
    return text;
}

}  // namespace

Ui::Ui() = default;

void Ui::begin(const InputState& input, u32 width, u32 height, f64 seconds, f32 scale) {
    dt_ = static_cast<f32>(std::clamp(seconds - seconds_, 0.0, 0.1));
    if (frame_ == 0) dt_ = 1.0f / 60.0f;
    const f32 moved = std::abs(input.mouse_x - input_.mouse_x) +
                      std::abs(input.mouse_y - input_.mouse_y);
    input_ = input;
    width_ = width;
    height_ = height;
    seconds_ = seconds;
    metrics_.scale = std::max(1.0f, scale);
    ++frame_;

    // A drag with no button held is not a drag.
    //
    // Every control that takes `active_` gives it back when it sees the release — and a control
    // that stopped being DRAWN never sees one. That is not a hypothetical: opening a world from the
    // library closes the windows on the press, so the release of that very double-click arrived
    // with the listing gone, and the rubber band it had started stayed active for the rest of the
    // process. The next time the windows came up, in the world, a selection box was already being
    // dragged by a mouse with nothing held down — which is what it looked like, because it was.
    //
    // Not the release frame: on that frame the button is already up and the control it belongs to
    // has to be allowed to see it. One frame later there is nothing left to see.
    if (active_ != 0 && !input.mouse_left && !input.mouse_left_released) active_ = 0;

    was_hot_ = hot_;
    hot_ = 0;
    wants_mouse_ = false;
    asked_for_text_ = false;
    tooltip_.set = false;
    if (moved > kRestDrift) hover_since_ = seconds_;

    draw_.begin(width, height);
    scroll_stack_.clear();
    claims_.clear();

    // A control that has not been drawn for a while is a control that no longer exists; its
    // animation goes with it. Pruned lazily rather than per frame, because the map is tens of
    // entries and walking it every frame to find nothing is the more expensive habit.
    if ((frame_ & 0xFF) == 0) {
        for (auto it = anims_.begin(); it != anims_.end();) {
            it = (frame_ - it->second.touched > 256) ? anims_.erase(it) : std::next(it);
        }
    }
}

void Ui::end() {
    // The tooltip and the refusal are drawn last because they belong to the SCREEN rather than to
    // the window they came from: a sentence clipped by the panel that raised it is a sentence
    // whose last three words are the ones you needed.
    if (refusal_.until > seconds_ && !refusal_.text.empty()) {
        const f32 pad = metrics_.px(6.0f);
        const f32 size = metrics_.text();
        const f32 width = DrawList::measure(refusal_.text, size) + pad * 2.0f;
        const f32 height = DrawList::line_height(size) + pad * 2.0f;
        f32 x = std::clamp(refusal_.at.x0, 0.0f, static_cast<f32>(width_) - width);
        f32 y = refusal_.at.y1 + metrics_.px(4.0f);
        if (y + height > static_cast<f32>(height_)) y = refusal_.at.y0 - height - metrics_.px(4.0f);
        const Rect box{x, y, x + width, y + height};
        draw_.glass(box);
        // A destructive decision is red, and so is the sentence that says a value was refused:
        // it is the one of the five permitted colours that means *this did not happen*.
        draw_.edge(box, 0.7f);
        draw_.hue(Rect{box.x0, box.y0, box.x0 + metrics_.px(2.0f), box.y1}, 0xD03A2Bu, 1.0f);
        draw_.text(box.x0 + pad, box.y0 + pad, refusal_.text, size);
    }

    if (tooltip_.set && !tooltip_.text.empty()) {
        const f32 pad = metrics_.px(6.0f);
        const f32 size = metrics_.text();
        const f32 width = DrawList::measure(tooltip_.text, size) + pad * 2.0f;
        const f32 height = DrawList::line_height(size) + pad * 2.0f;
        // Below the row it is about, and above it when there is no room below. Never over it:
        // a tooltip that covers the thing it explains has explained the wrong thing.
        f32 x = std::clamp(tooltip_.over.x0, 0.0f, std::max(0.0f, static_cast<f32>(width_) - width));
        f32 y = tooltip_.over.y1 + metrics_.px(4.0f);
        if (y + height > static_cast<f32>(height_)) {
            y = std::max(0.0f, tooltip_.over.y0 - height - metrics_.px(4.0f));
        }
        const Rect box{x, y, x + width, y + height};
        draw_.glass(box);
        draw_.edge(box, 0.5f);
        draw_.text(box.x0 + pad, box.y0 + pad, tooltip_.text, size);
    }

    draw_.bin();
    sound_.settle();
}

Ui::Anim& Ui::anim(u64 id) {
    Anim& a = anims_[id];
    a.touched = frame_;
    return a;
}

bool Ui::hovering(u64 id, const Rect& rect) {
    const bool inside = rect.holds(input_.mouse_x, input_.mouse_y) &&
                        draw_.clip().holds(input_.mouse_x, input_.mouse_y);
    if (!inside) return false;
    // Something drawn later is over this point, so this is not what is being pointed at. The
    // pointer is still CLAIMED — the press belongs to the interface either way, and letting it
    // through to the world would be the same bug one layer down.
    for (const Rect& claimed : claims_) {
        if (claimed.holds(input_.mouse_x, input_.mouse_y)) {
            wants_mouse_ = true;
            return false;
        }
    }
    hot_ = id;
    wants_mouse_ = true;
    if (was_hot_ != id) hover_since_ = seconds_;
    return true;
}

void Ui::press_started(u64 id) {
    Anim& a = anim(id);
    a.press = 1.0f;
    sound_.say(Cue::Press);
}

void Ui::caret(f32 x, f32 y0, f32 y1, f64 since) {
    // Between about a third and full ink, a whole cycle a second. Cosine rather than a square wave
    // for the reason the icons overshoot: the eye reads acceleration, and a line that eases is a
    // line that is alive where one that snaps is one that is broken.
    const f64 age = seconds_ - since;
    const f32 phase = static_cast<f32>(std::cos(age * 6.2831853));
    const f32 coverage = 0.35f + 0.65f * (0.5f + 0.5f * phase);
    const f32 wide = std::max(1.0f, metrics_.scale);
    draw_.ink(Rect{x, y0, x + wide, y1}, coverage);
}

void Ui::tooltip(const Rect& over, std::string_view sentence) {
    if (sentence.empty()) return;
    tooltip_.over = over;
    tooltip_.text = std::string(sentence);
    tooltip_.set = true;
}

void Ui::refuse(const Rect& at, std::string_view why) {
    refusal_.at = at;
    refusal_.text = std::string(why);
    refusal_.until = seconds_ + kRefusalSeconds;
    sound_.say(Cue::Refuse);
}

void Ui::stop_typing() {
    typing_ = 0;
    buffer_.clear();
    caret_ = 0;
}

void Ui::panel(const Rect& rect, f32 strength) {
    draw_.glass(rect, strength);
    draw_.edge(rect, 0.45f, std::max(1.0f, metrics_.scale * 0.5f));
}

void Ui::divider(const Rect& rect, bool vertical) {
    const f32 thickness = std::max(1.0f, metrics_.scale * 0.5f);
    const Rect line = vertical ? Rect{rect.mid_x() - thickness * 0.5f, rect.y0,
                                      rect.mid_x() + thickness * 0.5f, rect.y1}
                               : Rect{rect.x0, rect.mid_y() - thickness * 0.5f, rect.x1,
                                      rect.mid_y() + thickness * 0.5f};
    draw_.ink(line, 0.22f);
}

void Ui::label(const Rect& row, std::string_view text, Align align, u32 style, f32 coverage) {
    const f32 size = metrics_.text();
    const f32 top = row.mid_y() - DrawList::cap_height(size) * 0.5f;
    f32 x = row.x0;
    if (align == Align::Centre) x = row.mid_x();
    if (align == Align::Right) x = row.x1;
    // Coverage below one is how a row that is waiting on something says so: the same ink, less
    // of it. Not a second colour, because there is no second colour.
    draw_.text(x, top, text, size, style, align, coverage);
}

f32 Ui::shared_size(const Rect& rect, const std::string_view* labels, u32 count) const {
    if (count == 0) return metrics_.text();
    const f32 each = rect.width() / static_cast<f32>(count);
    const f32 pad = metrics_.px(6.0f);
    const f32 room = each - pad * 2.0f - metrics_.icon() * 0.55f - metrics_.px(6.0f);
    f32 longest = 1.0f;
    for (u32 i = 0; i < count; ++i) {
        longest = std::max(longest, DrawList::measure(labels[i], 1.0f));
    }
    return std::clamp(std::floor(room / longest), metrics_.scale, metrics_.text());
}

void Ui::icon_and_label(const Rect& rect, Icon icon, std::string_view text, f32 phase,
                        f32 coverage, bool centred, f32 forced_size) {
    const f32 pad = metrics_.px(6.0f);
    // The drawing may come within half the padding of the row's own edges. A whole pad either side
    // was costing it six of its twenty-two pixels to keep clear of a boundary nothing is drawn on.
    const f32 nominal = std::min(metrics_.icon(), rect.height() - pad * 0.5f);
    f32 size = (forced_size > 0.0f) ? forced_size : metrics_.text();

    if (centred) {
        // Stacked: the drawing above the word. This is what a title button is, and it is the one
        // shape where neither the drawing nor the word has to give anything up — so both are
        // larger than a row's, which is what makes a title read as a title.
        size = metrics_.scale * 3.0f;
        const f32 cell = std::min(nominal * 2.2f, rect.height() - DrawList::line_height(size) - pad);
        const f32 total = cell + metrics_.px(6.0f) + DrawList::cap_height(size);
        const f32 top = rect.mid_y() - total * 0.5f;
        if (icon != Icon::None && cell > 0.0f) {
            draw_.icon(Rect{rect.mid_x() - cell * 0.5f, top, rect.mid_x() + cell * 0.5f, top + cell},
                       icon, coverage, phase);
        }
        if (!text.empty()) {
            draw_.text(rect.mid_x(), top + cell + metrics_.px(6.0f), text, size, kPlain,
                       Align::Centre);
        }
        return;
    }

    // In a row: the drawing first, then the word. When they do not both fit, the drawing gives up
    // room before the word does and never gives up all of it — it shrinks to 0.55 of nominal and
    // stops, and only then does the word start shrinking.
    const f32 room = rect.width() - pad * 2.0f;
    f32 cell = (icon == Icon::None) ? 0.0f : nominal;
    f32 word = text.empty() ? 0.0f : DrawList::measure(text, size);
    const f32 between = (cell > 0.0f && word > 0.0f) ? metrics_.px(6.0f) : 0.0f;

    if (cell + between + word > room && cell > 0.0f) {
        const f32 floor_cell = nominal * 0.55f;
        cell = std::max(floor_cell, room - between - word);
    }
    if (cell + between + word > room && word > 0.0f) {
        // Only now does the word give anything up, and it gives up size before it gives up
        // letters: a truncated word is a word you cannot read, and a smaller one is a word.
        const f32 want = std::max(metrics_.scale, size * (room - cell - between) / std::max(word, 1.0f));
        size = std::max(metrics_.scale, std::floor(want));
        word = DrawList::measure(text, size);
    }

    f32 x = rect.x0 + pad;
    if (cell > 0.0f) {
        draw_.icon(Rect{x, rect.mid_y() - cell * 0.5f, x + cell, rect.mid_y() + cell * 0.5f}, icon,
                   coverage, phase);
        x += cell + between;
    }
    if (!text.empty()) {
        const f32 top = rect.mid_y() - DrawList::cap_height(size) * 0.5f;
        draw_.push_clip(Rect{rect.x0, rect.y0, rect.x1 - pad * 0.5f, rect.y1});
        draw_.text(x, top, text, size);
        draw_.pop_clip();
    }
}

bool Ui::button(u64 id, const Rect& rect, Icon icon, std::string_view text,
                std::string_view hint, bool stacked) {
    const bool over = hovering(id, rect);
    bool fired = false;

    if (over && input_.mouse_left_pressed) {
        active_ = id;
        press_started(id);
    }
    if (active_ == id && input_.mouse_left_released) {
        fired = over;   // a press that slid off and let go is a press somebody changed their mind about
        active_ = 0;
    }

    Anim& a = anim(id);
    a.press = std::max(0.0f, a.press - dt_ / kPressSeconds);

    // A static wash while the pointer is over it, which is legibility rather than motion: you
    // have to know what you are about to press. Motion is the icon's phase, and that only ever
    // runs because of `a.press`.
    const bool held = (active_ == id && input_.mouse_left);
    if (over || held) draw_.ink(rect, held ? 0.16f : 0.07f);
    icon_and_label(rect, icon, text, press_phase(a.press), 1.0f, stacked);

    if (over && seconds_ - hover_since_ > kRestSeconds) tooltip(rect, hint);
    return fired;
}

bool Ui::icon_button(u64 id, const Rect& rect, Icon icon, std::string_view hint) {
    const bool over = hovering(id, rect);
    bool fired = false;
    if (over && input_.mouse_left_pressed) {
        active_ = id;
        press_started(id);
    }
    if (active_ == id && input_.mouse_left_released) {
        fired = over;
        active_ = 0;
    }
    Anim& a = anim(id);
    a.press = std::max(0.0f, a.press - dt_ / kPressSeconds);

    const bool held = (active_ == id && input_.mouse_left);
    if (over || held) draw_.ink(rect, held ? 0.16f : 0.07f);
    // A square button is nothing BUT its drawing — there is no word beside it to leave room for —
    // so the drawing takes the room a word would have had.
    const f32 cell = std::min(rect.width(), rect.height()) * 0.86f;
    draw_.icon(Rect{rect.mid_x() - cell * 0.5f, rect.mid_y() - cell * 0.5f, rect.mid_x() + cell * 0.5f,
                    rect.mid_y() + cell * 0.5f},
               icon, 1.0f, press_phase(a.press));
    if (over && seconds_ - hover_since_ > kRestSeconds) tooltip(rect, hint);
    return fired;
}

bool Ui::toggle(u64 id, const Rect& rect, std::string_view text, bool& value,
                std::string_view hint) {
    const bool over = hovering(id, rect);
    bool changed = false;
    if (over && input_.mouse_left_pressed) {
        active_ = id;
        press_started(id);
    }
    if (active_ == id && input_.mouse_left_released) {
        if (over) {
            value = !value;
            changed = true;
            sound_.say(Cue::Commit, value ? 1.0f : 0.75f);
        }
        active_ = 0;
    }

    Anim& a = anim(id);
    a.press = std::max(0.0f, a.press - dt_ / kPressSeconds);
    const f32 want = value ? 1.0f : 0.0f;
    if (!a.shown_valid) {
        a.shown = want;
        a.shown_valid = true;
    }
    a.shown = chase(a.shown, want, dt_);

    if (over) draw_.ink(rect, 0.07f);
    const f32 pad = metrics_.px(6.0f);
    const f32 track_w = metrics_.px(26.0f);
    const f32 track_h = metrics_.px(12.0f);
    const Rect track{rect.x1 - pad - track_w, rect.mid_y() - track_h * 0.5f, rect.x1 - pad,
                     rect.mid_y() + track_h * 0.5f};
    draw_.ink(track, 0.12f);
    draw_.edge(track, 0.35f);
    const f32 knob = track_h - metrics_.px(2.0f);
    const f32 travel = track.width() - knob - metrics_.px(2.0f);
    const f32 kx = track.x0 + metrics_.px(1.0f) + travel * a.shown;
    draw_.ink(Rect{kx, track.y0 + metrics_.px(1.0f), kx + knob, track.y1 - metrics_.px(1.0f)}, 1.0f);

    // Clipped to the room before the switch. A label that runs under its own control is a label
    // whose last word is the one you needed — and at this size a long row name is the normal case,
    // not the exception.
    const Rect words{rect.x0 + pad, rect.y0, track.x0 - pad, rect.y1};
    draw_.push_clip(words);
    label(words, text);
    draw_.pop_clip();
    if (over && seconds_ - hover_since_ > kRestSeconds) tooltip(rect, hint);
    return changed;
}

bool Ui::choice(u64 id, const Rect& rect, std::string_view row_label, const Icon* icons,
                const std::string_view* labels, const std::string_view* tooltips, u32 count,
                u32& value, std::string_view hint) {
    if (count == 0) return false;
    const f32 pad = metrics_.px(6.0f);
    const f32 label_room = row_label.empty() ? 0.0f : rect.width() * 0.45f;
    if (!row_label.empty()) {
        label(Rect{rect.x0 + pad, rect.y0, rect.x0 + label_room, rect.y1}, row_label);
    }
    const Rect row{rect.x0 + label_room, rect.y0, rect.x1 - pad, rect.y1};
    const f32 each = row.width() / static_cast<f32>(count);

    bool changed = false;
    for (u32 i = 0; i < count; ++i) {
        const Rect cell{row.x0 + each * static_cast<f32>(i), row.y0,
                        row.x0 + each * static_cast<f32>(i + 1), row.y1};
        const u64 own = hash_combine(id, i + 1);
        const bool over = hovering(own, cell);
        if (over && input_.mouse_left_pressed) {
            active_ = own;
            // Pressing the answer already chosen is silent, because nothing happened. The
            // drawing still answers the press, because a press did happen.
            Anim& pressed = anim(own);
            pressed.press = 1.0f;
            if (value != i) sound_.say(Cue::Press);
        }
        if (active_ == own && input_.mouse_left_released) {
            if (over && value != i) {
                value = i;
                changed = true;
                sound_.say(Cue::Commit);
            }
            active_ = 0;
        }

        Anim& a = anim(own);
        a.press = std::max(0.0f, a.press - dt_ / kPressSeconds);
        if (value == i) draw_.ink(cell, 0.20f);
        else if (over) draw_.ink(cell, 0.07f);
        icon_and_label(cell, icons[i], labels[i], press_phase(a.press), 1.0f, false);

        // One of several answers say their own sentence: three buttons on a row are three
        // questions, and the row's description can only answer the first. An option with nothing
        // of its own falls back to the row's.
        if (over && seconds_ - hover_since_ > kRestSeconds) {
            const std::string_view own_hint = (tooltips != nullptr && !tooltips[i].empty())
                                                  ? tooltips[i]
                                                  : hint;
            tooltip(cell, own_hint);
        }
    }
    return changed;
}

bool Ui::tabs(u64 id, const Rect& rect, const Icon* icons, const std::string_view* labels,
              u32 count, u32& active) {
    if (count == 0) return false;
    const f32 each = rect.width() / static_cast<f32>(count);
    // One size for the row, decided by its longest word. Letting each tab shrink to fit its own
    // cell gives three words at three sizes, which reads as a mistake rather than as a fit.
    const f32 size = shared_size(rect, labels, count);
    bool changed = false;
    for (u32 i = 0; i < count; ++i) {
        const Rect cell{rect.x0 + each * static_cast<f32>(i), rect.y0,
                        rect.x0 + each * static_cast<f32>(i + 1), rect.y1};
        const u64 own = hash_combine(id, i + 101);
        const bool over = hovering(own, cell);
        if (over && input_.mouse_left_pressed) {
            active_ = own;
            anim(own).press = 1.0f;
            if (active != i) sound_.say(Cue::Press);
        }
        if (active_ == own && input_.mouse_left_released) {
            if (over && active != i) {
                active = i;
                changed = true;
                sound_.say(Cue::Open);
            }
            active_ = 0;
        }
        Anim& a = anim(own);
        a.press = std::max(0.0f, a.press - dt_ / kPressSeconds);
        if (active == i) {
            draw_.ink(cell, 0.16f);
            // The one that is open is underlined, because a wash alone reads as "hovered" the
            // moment two of them are washed at once.
            draw_.ink(Rect{cell.x0, cell.y1 - metrics_.px(2.0f), cell.x1, cell.y1}, 0.9f);
        } else if (over) {
            draw_.ink(cell, 0.07f);
        }
        icon_and_label(cell, icons[i], labels[i], press_phase(a.press), active == i ? 1.0f : 0.75f, false,
                       size);
    }
    return changed;
}

bool Ui::section_open(u64 id) const {
    const auto at = folds_.find(id);
    return at != folds_.end() && at->second;
}

void Ui::close_all_sections() {
    for (auto& fold : folds_) fold.second = false;
}

Ui::Fold Ui::section(u64 id, const Rect& row, std::string_view name, bool dirty, u32 depth) {
    Fold out;
    // Sections start CLOSED, and the window that owns them folds them again every time it opens.
    // A panel that opens showing everything is the wall of rows the fold exists to replace; five
    // words and five triangles is what *what can I change here* actually looks like as an answer.
    auto at = folds_.find(id);
    if (at == folds_.end()) at = folds_.emplace(id, false).first;

    const f32 indent = metrics_.px(10.0f) * static_cast<f32>(depth);
    const Rect head{row.x0 + indent, row.y0, row.x1, row.y1};
    // The gutter is reserved whether or not the reset is drawn in it, so a heading's word sits in
    // the same place as every other row's and the column does not move when a value is changed.
    const Rect gutter{head.x1 - metrics_.row(), head.y0, head.x1, head.y1};
    const Rect grip{head.x0, head.y0, gutter.x0, head.y1};

    const bool over = hovering(id, grip);
    if (over && input_.mouse_left_pressed) {
        active_ = id;
        press_started(id);
    }
    if (active_ == id && input_.mouse_left_released) {
        if (over) {
            at->second = !at->second;
            sound_.say(at->second ? Cue::Open : Cue::Close);
        }
        active_ = 0;
    }
    Anim& a = anim(id);
    a.press = std::max(0.0f, a.press - dt_ / kPressSeconds);
    if (over) draw_.ink(grip, 0.07f);

    const f32 cell = std::min(metrics_.icon(), grip.height());
    draw_.icon(Rect{grip.x0, grip.mid_y() - cell * 0.5f, grip.x0 + cell, grip.mid_y() + cell * 0.5f},
               at->second ? Icon::Expanded : Icon::Collapsed, 1.0f, press_phase(a.press));
    // A heading is BOLD and at full strength: a heavier letter that is also fainter than the rows
    // under it reads as a row that has been disabled rather than as a heading.
    label(Rect{grip.x0 + cell + metrics_.px(5.0f), grip.y0, grip.x1, grip.y1}, name, Align::Left,
          kBold, 1.0f);
    draw_.ink(Rect{head.x0, head.y1 - metrics_.px(1.0f), head.x1, head.y1}, 0.18f);

    out.open = at->second;
    out.reset = reset_button(hash_combine(id, 0x9E3779B9ull), gutter, dirty,
                             "Put everything under this heading back to how it shipped");
    return out;
}

bool Ui::reset_button(u64 id, const Rect& cell, bool changed, std::string_view hint) {
    // Nothing is drawn when nothing has changed, and that absence is the second thing this says:
    // an empty gutter down the side of a panel is *every one of these is as it shipped*, which is
    // a question a player would otherwise have to ask a wiki.
    if (!changed) return false;
    const f32 size = std::min(cell.width(), cell.height()) * 0.72f;
    const Rect box{cell.mid_x() - size * 0.5f, cell.mid_y() - size * 0.5f, cell.mid_x() + size * 0.5f,
                   cell.mid_y() + size * 0.5f};
    const bool over = hovering(id, box);
    bool fired = false;
    if (over && input_.mouse_left_pressed) {
        active_ = id;
        press_started(id);
    }
    if (active_ == id && input_.mouse_left_released) {
        fired = over;
        if (fired) sound_.say(Cue::Commit, 0.8f);
        active_ = 0;
    }
    Anim& a = anim(id);
    a.press = std::max(0.0f, a.press - dt_ / kPressSeconds);
    if (over) draw_.ink(box, 0.12f);
    draw_.icon(box, Icon::Reset, over ? 1.0f : 0.55f, press_phase(a.press));
    if (over && seconds_ - hover_since_ > kRestSeconds) tooltip(box, hint);
    return fired;
}

void Ui::open_menu(u64 id, f32 x, f32 y) {
    menu_ = id;
    menu_x_ = x;
    menu_y_ = y;
    menu_opened_frame_ = frame_;
    sound_.say(Cue::Open);
}

i32 Ui::menu(u64 id, const MenuItem* items, u32 count) {
    if (menu_ != id || count == 0) return -1;

    const f32 row = metrics_.row();
    const f32 pad = metrics_.px(4.0f);
    f32 widest = metrics_.px(40.0f);
    for (u32 i = 0; i < count; ++i) {
        widest = std::max(widest, DrawList::measure(items[i].label, metrics_.text()));
    }
    const f32 width = widest + metrics_.icon() + metrics_.px(24.0f);
    const f32 height = row * static_cast<f32>(count) + pad * 2.0f;
    // Flipped rather than clamped when it would run off: a menu whose last item is under the edge
    // of the screen is a menu missing the item somebody was reaching for.
    f32 x = menu_x_;
    f32 y = menu_y_;
    if (x + width > static_cast<f32>(width_)) x = std::max(0.0f, menu_x_ - width);
    if (y + height > static_cast<f32>(height_)) y = std::max(0.0f, menu_y_ - height);
    const Rect box{x, y, x + width, y + height};

    // Over everything, because a menu belongs to the screen rather than to the window that raised
    // it — the same rule as the tooltip, for the same reason.
    draw_.push_clip(screen());
    draw_.glass(box);
    draw_.edge(box, 0.6f);

    i32 chosen = -1;
    for (u32 i = 0; i < count; ++i) {
        const Rect cell{box.x0 + pad, box.y0 + pad + row * static_cast<f32>(i), box.x1 - pad,
                        box.y0 + pad + row * static_cast<f32>(i + 1)};
        const u64 own = hash_combine(id, i + 1);
        const bool over = hovering(own, cell) && items[i].enabled;
        if (over) draw_.ink(cell, 0.14f);
        if (over && input_.mouse_left_released) chosen = static_cast<i32>(i);
        // An item that cannot act is drawn at less ink rather than left out: a menu whose length
        // changes with the selection is a menu whose items are never in the same place twice.
        icon_and_label(cell, items[i].icon, items[i].label, 1.0f, items[i].enabled ? 1.0f : 0.35f,
                       false);
    }
    draw_.pop_clip();

    // Anything else closes it. The frame it opened on is excluded, or the press that opened it
    // would also be the press that dismissed it.
    const bool elsewhere = (input_.mouse_left_pressed || input_.mouse_right_pressed) &&
                           !box.holds(input_.mouse_x, input_.mouse_y);
    if (chosen >= 0 || input_.was_pressed(Key::Escape) ||
        (elsewhere && frame_ != menu_opened_frame_)) {
        menu_ = 0;
    }
    // While it is up it owns the pointer, so a click meant for it never also reaches the world.
    wants_mouse_ = true;
    return chosen;
}

bool Ui::pressable(u64 id, const Rect& rect, std::string_view hint) {
    const bool over = hovering(id, rect);
    bool fired = false;
    if (over && input_.mouse_left_pressed) {
        active_ = id;
        press_started(id);
    }
    if (active_ == id && input_.mouse_left_released) {
        fired = over;
        active_ = 0;
    }
    // Nothing is drawn, not even the wash every other control gets while the pointer is over it.
    // What this is over is a picture, and a picture with a hover state is furniture.
    if (over && seconds_ - hover_since_ > kRestSeconds) tooltip(rect, hint);
    return fired;
}

bool Ui::touched() const {
    if (input_.mouse_left_pressed || input_.mouse_right_pressed || input_.mouse_left ||
        input_.mouse_right || input_.mouse_middle) {
        return true;
    }
    if (input_.mouse_dx != 0.0f || input_.mouse_dy != 0.0f || input_.wheel != 0.0f) return true;
    if (!input_.typed.empty()) return true;
    for (usize key = 0; key < static_cast<usize>(Key::Count); ++key) {
        if (input_.down[key] || input_.pressed[key]) return true;
    }
    return false;
}

std::string Ui::spell(f64 value, const Number& about) const {
    char text[64];
    std::snprintf(text, sizeof(text), "%.*f", std::clamp(about.decimals, 0, 6), value);
    std::string out = trim_zeros(text);
    if (!about.unit.empty()) {
        out += " ";
        out += std::string(about.unit);
    }
    return out;
}

bool Ui::number(u64 id, const Rect& rect, const Number& about, f64& value) {
    const bool over = hovering(id, rect);
    // True on any frame this WROTE to `value`, which is every frame of a drag as well as the one a
    // typed number is committed on. See the drag below for why it cannot be only the last one.
    bool changed = false;
    const f32 pad = metrics_.px(6.0f);
    const f64 span = (about.high > about.low) ? (about.high - about.low) : 1.0;

    // --- typing into it ---------------------------------------------------------------------
    //
    // A double-click turns the row into a field with everything selected, because typing into a
    // value means replacing it far more often than it means editing it.
    if (over && input_.mouse_left_pressed && input_.click_count >= 2) {
        typing_ = id;
        buffer_ = trim_zeros([&] {
            char text[64];
            std::snprintf(text, sizeof(text), "%.*f", std::clamp(about.decimals, 0, 6), value);
            return std::string(text);
        }());
        caret_ = buffer_.size();
        buffer_all_selected_ = true;
        caret_since_ = seconds_;
        active_ = 0;
    } else if (over && input_.mouse_left_pressed) {
        active_ = id;
        grab_x_ = input_.mouse_x;
        grab_value_ = value;
        press_started(id);
    }

    // A press anywhere else puts the row back to being a slider.
    //
    // It used to take enter or escape and nothing else, so a row typed into stayed a field for the
    // rest of the session however far away you clicked — and a slider that has quietly stopped
    // being a slider is a control that ignores the next drag. Clicking away is escape rather than
    // enter: `14-ui-style.md` already says escaping out of a value you were typing is silent and
    // keeps what it had, and a click somewhere else is not somebody committing a number.
    if (typing_ == id && input_.mouse_left_pressed && !over) stop_typing();

    if (typing_ == id) {
        for (char c : input_.typed) {
            // A number is digits, one point, one sign and an exponent. Anything else typed into a
            // value is a typo, and refusing the CHARACTER is a better answer than refusing the
            // value afterwards — the feedback is that nothing appeared.
            const bool numeric = (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '+' ||
                                 c == 'e' || c == 'E';
            if (!numeric) continue;
            if (buffer_all_selected_) {
                buffer_.clear();
                caret_ = 0;
                buffer_all_selected_ = false;
            }
            buffer_.insert(buffer_.begin() + static_cast<isize>(caret_), c);
            ++caret_;
        }
        if (input_.fired(Key::Backspace) && caret_ > 0) {
            if (buffer_all_selected_) {
                buffer_.clear();
                caret_ = 0;
                buffer_all_selected_ = false;
            } else {
                buffer_.erase(buffer_.begin() + static_cast<isize>(caret_ - 1));
                --caret_;
            }
        }
        if (input_.fired(Key::Left) && caret_ > 0) { --caret_; buffer_all_selected_ = false; }
        if (input_.fired(Key::Right) && caret_ < buffer_.size()) { ++caret_; buffer_all_selected_ = false; }
        if (input_.was_pressed(Key::Home)) { caret_ = 0; buffer_all_selected_ = false; }
        if (input_.was_pressed(Key::End)) { caret_ = buffer_.size(); buffer_all_selected_ = false; }
        if (input_.was_pressed(Key::Escape)) {
            // Escaping out of a value you were typing is silent on purpose: nothing happened.
            stop_typing();
        } else if (input_.was_pressed(Key::Enter)) {
            const f64 typed = std::strtod(buffer_.c_str(), nullptr);
            std::string why = about.guard ? about.guard(typed) : std::string();
            if (!buffer_.empty() && why.empty()) {
                // NOT clamped. The slider's range is where the handle can go, not where the value
                // can (D444): type ten times the maximum and the value is ten times the maximum,
                // with the handle sitting at the end of its travel saying so.
                value = typed;
                changed = true;
                sound_.say(Cue::Commit);
            } else if (!why.empty()) {
                refuse(rect, why);
            }
            stop_typing();
        }
    }

    // --- dragging it -------------------------------------------------------------------------
    //
    // The value is written on EVERY frame of the drag and not only when the hand comes off, and this
    // returns true on every one of those frames. That is not a nicety: a caller writes what came back
    // into wherever the setting lives, so a value only written on release is a value that was never
    // written at all — the release frame has the button already up, so the old code recomputed
    // nothing, handed back the number it was given, and every slider in the settings window was a
    // no-op that put the handle back where it started. It read exactly as *the sliders reset
    // themselves*, because they did.
    //
    // The position is recomputed from where the pointer IS rather than accumulated, so a drag that
    // leaves the row and comes back is still tracking the same grab.
    if (active_ == id) {
        const f32 track = std::max(1.0f, rect.width());
        const f64 was = value;
        f64 want = grab_value_ + static_cast<f64>(input_.mouse_x - grab_x_) / track * span;
        want = std::clamp(want, about.low, about.high);
        if (about.step > 0.0) want = about.low + std::round((want - about.low) / about.step) * about.step;
        if (want != was) {
            value = want;
            changed = true;
            // A notch passing under the handle, pitched by which way it went, so a drag is audibly
            // a direction rather than a rattle.
            if (about.step > 0.0) sound_.say(Cue::Step, want > was ? 1.12f : 0.89f);
        }
        if (input_.mouse_left_released) {
            active_ = 0;
            std::string why = about.guard ? about.guard(value) : std::string();
            if (!why.empty()) {
                value = grab_value_;   // and the caller is told, so the refusal reaches the setting
                changed = true;
                refuse(rect, why);
            } else {
                changed = true;
                sound_.say(Cue::Commit);
            }
        }
    }

    // --- drawing it ---------------------------------------------------------------------------
    //
    // The fill IS ink, the way the loading bar's is, so the label and the value invert per pixel
    // as the fill passes under them. A track with a colour of its own would need the label moved
    // off it, which is the ordinary failure this whole rule exists to avoid.
    Anim& a = anim(id);
    a.press = std::max(0.0f, a.press - dt_ / kPressSeconds);
    const f32 want_at = static_cast<f32>(std::clamp((value - about.low) / span, 0.0, 1.0));
    if (!a.shown_valid) {
        a.shown = want_at;
        a.shown_valid = true;
    }
    a.shown = chase(a.shown, want_at, dt_);

    draw_.ink(rect, 0.10f);
    const f32 head = rect.x0 + rect.width() * a.shown;
    draw_.ink(Rect{rect.x0, rect.y0, head, rect.y1}, 0.34f);
    // The handle: where the value is, drawn as a bar so that the end of its travel is visible and
    // a value past the end is legible as a value past the end.
    draw_.ink(Rect{head - metrics_.px(1.5f), rect.y0, head + metrics_.px(1.5f), rect.y1}, 0.95f);
    if (over || active_ == id) draw_.edge(rect, 0.5f);

    label(Rect{rect.x0 + pad, rect.y0, rect.x1, rect.y1}, about.label);
    if (typing_ == id) {
        const f32 size = metrics_.text();
        const f32 top = rect.mid_y() - DrawList::cap_height(size) * 0.5f;
        const f32 width = DrawList::measure(buffer_, size);
        const f32 x = rect.x1 - pad - width;
        if (buffer_all_selected_ && !buffer_.empty()) {
            draw_.ink(Rect{x - metrics_.px(2.0f), rect.y0 + metrics_.px(3.0f),
                           rect.x1 - pad + metrics_.px(2.0f), rect.y1 - metrics_.px(3.0f)},
                      0.35f);
        }
        draw_.text(x, top, buffer_, size);
        // The one motion in this interface that does not answer a press, because it is not saying
        // something happened — it is saying where you are.
        caret(x + DrawList::measure(std::string_view(buffer_).substr(0, caret_), size),
              rect.y0 + metrics_.px(3.0f), rect.y1 - metrics_.px(3.0f), caret_since_);
    } else {
        label(Rect{rect.x0, rect.y0, rect.x1 - pad, rect.y1}, spell(value, about), Align::Right);
    }

    if (over && typing_ != id && seconds_ - hover_since_ > kRestSeconds) tooltip(rect, about.tooltip);
    return changed;
}

void Ui::type_into(u64 id, const std::string& value) {
    typing_ = id;
    buffer_ = value;
    caret_ = buffer_.size();
    // Chosen, so the first key typed replaces `untitled` rather than making `untitledporch`.
    buffer_all_selected_ = true;
    caret_since_ = seconds_;
}

bool Ui::field(u64 id, const Rect& rect, std::string& value, std::string_view placeholder,
               std::string_view hint) {
    const bool over = hovering(id, rect);
    bool committed = false;
    const f32 pad = metrics_.px(6.0f);

    if (input_.mouse_left_pressed) {
        if (over) {
            if (typing_ != id) {
                typing_ = id;
                buffer_ = value;
                caret_ = buffer_.size();
                buffer_all_selected_ = input_.click_count >= 2;
                caret_since_ = seconds_;
            }
        } else if (typing_ == id) {
            // Clicking away commits what was typed. Anything else silently throws away a rename
            // somebody finished and did not press enter on.
            value = buffer_;
            committed = true;
            stop_typing();
            sound_.say(Cue::Commit);
        }
    }

    if (typing_ == id) {
        if (!input_.typed.empty()) {
            if (buffer_all_selected_) {
                buffer_.clear();
                caret_ = 0;
                buffer_all_selected_ = false;
            }
            buffer_.insert(caret_, input_.typed);
            caret_ += input_.typed.size();
        }
        if (input_.fired(Key::Backspace)) {
            if (buffer_all_selected_) {
                buffer_.clear();
                caret_ = 0;
                buffer_all_selected_ = false;
            } else if (caret_ > 0) {
                // Back over a whole character, not a byte: deleting half of a two-byte letter
                // leaves a string nothing can draw.
                usize back = caret_ - 1;
                while (back > 0 && (static_cast<u8>(buffer_[back]) & 0xC0u) == 0x80u) --back;
                buffer_.erase(back, caret_ - back);
                caret_ = back;
            }
        }
        if (input_.fired(Key::Delete) && caret_ < buffer_.size()) {
            usize forward = caret_ + 1;
            while (forward < buffer_.size() && (static_cast<u8>(buffer_[forward]) & 0xC0u) == 0x80u) {
                ++forward;
            }
            buffer_.erase(caret_, forward - caret_);
        }
        if (input_.fired(Key::Left) && caret_ > 0) {
            --caret_;
            while (caret_ > 0 && (static_cast<u8>(buffer_[caret_]) & 0xC0u) == 0x80u) --caret_;
            buffer_all_selected_ = false;
        }
        if (input_.fired(Key::Right) && caret_ < buffer_.size()) {
            ++caret_;
            while (caret_ < buffer_.size() && (static_cast<u8>(buffer_[caret_]) & 0xC0u) == 0x80u) {
                ++caret_;
            }
            buffer_all_selected_ = false;
        }
        if (input_.was_pressed(Key::Home)) { caret_ = 0; buffer_all_selected_ = false; }
        if (input_.was_pressed(Key::End)) { caret_ = buffer_.size(); buffer_all_selected_ = false; }
        if (input_.was_pressed(Key::Escape)) {
            stop_typing();
        } else if (input_.was_pressed(Key::Enter)) {
            value = buffer_;
            committed = true;
            stop_typing();
            sound_.say(Cue::Commit);
        }
    }

    draw_.ink(rect, (typing_ == id) ? 0.16f : 0.09f);
    draw_.edge(rect, (typing_ == id) ? 0.7f : (over ? 0.45f : 0.25f));
    const f32 size = metrics_.text();
    const f32 top = rect.mid_y() - DrawList::cap_height(size) * 0.5f;
    const std::string& shown = (typing_ == id) ? buffer_ : value;
    draw_.push_clip(Rect{rect.x0 + pad * 0.5f, rect.y0, rect.x1 - pad * 0.5f, rect.y1});
    if (shown.empty() && typing_ != id) {
        label(Rect{rect.x0 + pad, rect.y0, rect.x1, rect.y1}, placeholder, Align::Left, kItalic, 0.5f);
    } else {
        if (buffer_all_selected_ && typing_ == id && !buffer_.empty()) {
            draw_.ink(Rect{rect.x0 + pad - metrics_.px(2.0f), rect.y0 + metrics_.px(3.0f),
                           rect.x0 + pad + DrawList::measure(buffer_, size) + metrics_.px(2.0f),
                           rect.y1 - metrics_.px(3.0f)},
                      0.35f);
        }
        draw_.text(rect.x0 + pad, top, shown, size);
    }
    if (typing_ == id) {
        caret(rect.x0 + pad + DrawList::measure(std::string_view(buffer_).substr(0, caret_), size),
              rect.y0 + metrics_.px(3.0f), rect.y1 - metrics_.px(3.0f), caret_since_);
    }
    draw_.pop_clip();

    if (over && typing_ != id && seconds_ - hover_since_ > kRestSeconds) tooltip(rect, hint);
    return committed;
}

bool Ui::drag_handle(u64 id, const Rect& rect, bool along_x, bool inverted, f32 extent,
                     f64& value) {
    const bool over = hovering(id, rect);
    if (over && input_.mouse_left_pressed) {
        active_ = id;
        grab_x_ = along_x ? input_.mouse_x : input_.mouse_y;
        grab_value_ = value;
        // No `press_started`: a splitter has nothing to report yet, and a sound on grabbing one
        // would be a sound that reports a contact rather than a change.
    }
    bool moved = false;
    if (active_ == id) {
        const f32 now = along_x ? input_.mouse_x : input_.mouse_y;
        const f64 delta = static_cast<f64>((now - grab_x_) / std::max(extent, 1.0f));
        const f64 want = grab_value_ + (inverted ? -delta : delta);
        if (want != value) {
            value = want;
            moved = true;
        }
        if (input_.mouse_left_released) active_ = 0;
        wants_mouse_ = true;
    }
    // A hairline where the hand can take hold. Static, because being able to grab something is
    // not something that happened.
    if (over || active_ == id) draw_.ink(rect, (active_ == id) ? 0.30f : 0.14f);
    return moved;
}

Rect Ui::begin_scroll(u64 id, const Rect& rect, f32 content_height) {
    f32& offset = scrolls_[id];
    const f32 most = std::max(0.0f, content_height - rect.height());
    if (rect.holds(input_.mouse_x, input_.mouse_y) && input_.wheel != 0.0f) {
        // Scrolling is silent on purpose, and it is not a press, so nothing animates either.
        offset -= input_.wheel * metrics_.px(48.0f);
        wants_mouse_ = true;
    }

    // --- the bar, which is a control now ----------------------------------------------------
    //
    // It was a hairline, on the reasoning that a list you can drag by its bar is a list with two
    // ways to do one thing. That is right about a settings panel of nine rows and wrong about a
    // document: a clip fragment is sixteen hundred lines, and a wheel is the only way to cross it —
    // which is a hundred turns, and no way at all to see where in the file you are without
    // counting. **Asked for directly.** So it keeps saying how much there is and where you are, and
    // it now also takes a drag; the wheel is unchanged and is still what most people use.
    const u64 bar_id = hash_combine(id, 0x5C120211ull);
    const f32 bar_wide = metrics_.px(7.0f);
    const f32 tall = (content_height > 0.0f)
                         ? std::max(metrics_.px(28.0f),
                                    rect.height() * rect.height() / content_height)
                         : rect.height();
    const Rect gutter{rect.x1 - bar_wide, rect.y0, rect.x1, rect.y1};
    if (most > 0.0f) {
        const f32 travel = std::max(1.0f, rect.height() - tall);
        const bool over_gutter = hovering(bar_id, gutter);
        if (over_gutter && input_.mouse_left_pressed) {
            active_ = bar_id;
            const f32 handle_at = rect.y0 + travel * std::clamp(offset / most, 0.0f, 1.0f);
            // A press ON the handle picks it up where it is; a press anywhere else in the gutter
            // jumps there first, because a bar you have to drag from one end is a bar that makes
            // you travel the distance you were trying to skip.
            if (input_.mouse_y < handle_at || input_.mouse_y >= handle_at + tall) {
                offset = std::clamp((input_.mouse_y - rect.y0 - tall * 0.5f) / travel, 0.0f, 1.0f) *
                         most;
            }
            grab_x_ = input_.mouse_y - (rect.y0 + travel * std::clamp(offset / most, 0.0f, 1.0f));
            grab_value_ = offset;
        }
        if (active_ == bar_id) {
            offset = std::clamp((input_.mouse_y - grab_x_ - rect.y0) / travel, 0.0f, 1.0f) * most;
            wants_mouse_ = true;
            if (input_.mouse_left_released) active_ = 0;
        }
    }
    offset = std::clamp(offset, 0.0f, most);

    draw_.push_clip(rect);
    scroll_stack_.push_back(offset);
    if (most > 0.0f) {
        const f32 travel = std::max(1.0f, rect.height() - tall);
        const f32 at = rect.y0 + travel * (offset / most);
        // The track first, faintly, because a handle with nothing behind it says where you are and
        // not how far there is to go.
        draw_.ink(Rect{gutter.x0 + metrics_.px(2.0f), gutter.y0, gutter.x1 - metrics_.px(2.0f),
                       gutter.y1},
                  0.07f);
        const bool live = active_ == bar_id || hot_ == bar_id;
        draw_.ink(Rect{gutter.x0 + metrics_.px(1.0f), at, gutter.x1 - metrics_.px(1.0f), at + tall},
                  live ? 0.55f : 0.30f);
    }
    return Rect{rect.x0, rect.y0 - offset, rect.x1, rect.y0 - offset + content_height};
}

void Ui::end_scroll() {
    draw_.pop_clip();
    if (!scroll_stack_.empty()) scroll_stack_.pop_back();
}

f32 Ui::scroll_of(u64 id) const {
    const auto it = scrolls_.find(id);
    return (it == scrolls_.end()) ? 0.0f : it->second;
}

bool Ui::band(u64 id, const Rect& rect, Rect& box, bool& finished) {
    finished = false;
    const bool inside = rect.holds(input_.mouse_x, input_.mouse_y) &&
                        draw_.clip().holds(input_.mouse_x, input_.mouse_y);
    if (inside && input_.mouse_left_pressed && active_ == 0) {
        active_ = id;
        bands_[id] = Rect{input_.mouse_x, input_.mouse_y, input_.mouse_x, input_.mouse_y};
    }
    if (active_ != id) return false;

    Rect& live = bands_[id];
    live.x1 = input_.mouse_x;
    live.y1 = input_.mouse_y;
    box = Rect{std::min(live.x0, live.x1), std::min(live.y0, live.y1),
               std::max(live.x0, live.x1), std::max(live.y0, live.y1)};
    if (input_.mouse_left_released) {
        active_ = 0;
        finished = true;
    } else {
        draw_.ink(box, 0.12f);
        draw_.edge(box, 0.6f);
    }
    wants_mouse_ = true;
    return true;
}

f32 Ui::scroll_overflow(u64 id, const Rect& room, std::string_view text) {
    const f32 wide = DrawList::measure(text, metrics_.text());
    const f32 over = wide - room.width();
    if (over <= 0.5f) return 0.0f;

    // Only the one under the pointer travels. A listing where every long name is moving at once is
    // a listing nobody can read any of, and the row somebody is pointing at is the row they are
    // trying to read — which makes this the same *asking* a tooltip is, answered without words.
    if (!room.holds(input_.mouse_x, input_.mouse_y) ||
        !draw_.clip().holds(input_.mouse_x, input_.mouse_y)) {
        anim(id).shown = 0.0f;
        return 0.0f;
    }

    // Held at the start, travelling, held at the end, then cut back. The speed is per second of
    // TEXT rather than per second of row, so a name twice as long takes twice as long to read
    // rather than going twice as fast.
    constexpr f32 kWait = 0.9f;          // seconds at each end
    constexpr f32 kSpeed = 46.0f;        // interface pixels a second
    const f32 travel = over / (metrics_.scale * kSpeed);
    const f32 cycle = kWait + travel + kWait;
    Anim& a = anim(id);
    a.shown = (a.shown <= 0.0f) ? 0.0001f : a.shown + dt_;
    if (a.shown > cycle) a.shown -= cycle;
    const f32 at = a.shown;
    if (at <= kWait) return 0.0f;
    if (at >= kWait + travel) return -over;
    return -over * ((at - kWait) / std::max(travel, 0.0001f));
}

f32 Ui::markdown(const Rect& box, std::string_view text, bool measure_only) {
    const Markup document = parse_markup(std::string(text));
    const f32 base = metrics_.text();
    const f32 leading = metrics_.px(3.0f);
    f32 y = box.y0;

    for (const MarkupLine& line : document.lines) {
        const f32 indent = metrics_.px(8.0f) * static_cast<f32>(line.indent) +
                           metrics_.px(8.0f) * static_cast<f32>(line.quote);
        f32 x = box.x0 + indent;

        if (line.kind == MarkupBlock::blank) {
            y += base * 2.0f;
            continue;
        }
        if (line.kind == MarkupBlock::rule) {
            if (!measure_only) {
                draw_.ink(Rect{box.x0, y + base, box.x1, y + base + std::max(1.0f, metrics_.scale)},
                          0.3f);
            }
            y += base * 3.0f;
            continue;
        }

        // A heading spends size while there is size to spend and weight when there is not:
        // `#` three times, `##` twice, `###` and deeper bold.
        f32 size = base;
        u32 extra = kPlain;
        if (line.kind == MarkupBlock::heading) {
            if (line.level == 1) size = base * 3.0f;
            else if (line.level == 2) size = base * 2.0f;
            else extra = kBold;
        }

        if (!measure_only && line.quote > 0) {
            draw_.ink(Rect{box.x0 + metrics_.px(2.0f), y, box.x0 + metrics_.px(4.0f),
                           y + DrawList::line_height(size)}, 0.35f);
        }

        if (line.kind == MarkupBlock::bullet || line.kind == MarkupBlock::ordered ||
            line.box >= 0) {
            std::string marker;
            if (line.box == 0) marker = "[ ]";
            else if (line.box == 1) marker = "[x]";
            else if (line.kind == MarkupBlock::ordered) marker = std::to_string(line.ordinal) + ".";
            else marker = "-";
            if (!measure_only) draw_.text(x, y, marker, size);
            x += DrawList::measure(marker, size) + metrics_.px(4.0f);
        }

        for (const Run& run : line.runs) {
            const u32 style = static_cast<u32>(run.style) | extra |
                              ((line.kind == MarkupBlock::code) ? kMono : 0u);
            // Runs are contiguous — the spaces between words are already in the text — so nothing
            // is added between them. A gap per run would put a hole either side of every `bold`.
            if (!measure_only) x += draw_.text(x, y, run.text, size, style);
            else x += DrawList::measure(run.text, size, style);
        }
        y += DrawList::line_height(size) + leading;
    }
    return y - box.y0;
}

}  // namespace ws::ui
