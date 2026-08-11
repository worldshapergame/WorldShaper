#pragma once
// The interface's voice: synthesised, never sampled, and silent far more often than it is not.
//
// documentation/14-ui-style.md sets the bar at *more than feels necessary* — tweaking is a core
// activity and a control that responds weakly makes tweaking feel like paperwork — and then spends
// most of its space on the deliberate silences, because "we forgot" and "we decided not to" look
// identical from outside. Those silences are the design here:
//
//   **A sound reports a change, never a contact.** There is no hover sound anywhere and there will
//   not be one. Pressing the answer already chosen is silent, because nothing happened.
//
//   **One sound a frame** — the one that says the most. A single press produces several true
//   statements at once (a value moved, a value committed, a window opened) and playing all of them
//   is a muddle two frames long for one action. So `say` is a claim and `settle` picks the winner.
//
//   **Off is exactly silent.** Not quiet: zero samples, which tests/test_sound.cpp checks by
//   counting them. A volume of nought that still mixes an envelope is a build that crackles on
//   somebody's machine for a feature they turned off.
//
// # Why synthesised
//
// A sample is a file, a licence, a loading step and a decision about sample rate. A cue here is an
// envelope over a waveform and about forty lines, it is exact at any rate the device asks for, and
// a control that wants to say "this value is bigger" can simply ask for the same cue a fifth
// higher rather than shipping twelve recordings of a click.

#include <atomic>

#include "core/types.hpp"
#include "platform/audio.hpp"

namespace ws::ui {

// What can be said. Ordered by how much it says, because that ordering IS the rule for which one
// survives a frame that produces several: the highest wins. A refusal outranks a commit outranks a
// press, because the surprising statement is the one worth the frame.
enum class Cue : u32 {
    None = 0,
    Step,      // a slider passing a notch, or a selection moving by one
    Press,     // a control taking a press
    Open,      // a window, a folder, a tab
    Close,
    Commit,    // a value changed and stuck
    Erase,     // something went to the trash
    Refuse,    // and the one that has to be heard over anything else
    Count,
};

// Rendered stereo interleaved, in float, at whatever rate the device asked for.
//
// It is an AudioSource, which is the whole of what the device end knows about it: ws_platform has
// no idea what a cue is and this file has no idea what a device is.
class Sound : public AudioSource {
public:
    // The device's rate. Called once, before anything is rendered, and again if the device
    // changes underneath us.
    void configure(u32 sample_rate);

    void set_enabled(bool on) { enabled_.store(on, std::memory_order_relaxed); }
    bool enabled() const { return enabled_.load(std::memory_order_relaxed); }
    // 0 to 1, and 0 is the same silence `enabled(false)` is: no voice starts at all.
    void set_volume(f32 volume);
    f32 volume() const { return volume_.load(std::memory_order_relaxed); }

    // A claim that something happened. Cheap and idempotent within a frame; the loudest claim of
    // the frame is the one that will be heard, and the rest are dropped rather than queued.
    //
    // `pitch` is a multiplier on the cue's own frequency, so "the value went up" and "the value
    // went down" are one cue at two pitches rather than two recordings.
    void say(Cue cue, f32 pitch = 1.0f);

    // End of frame: start the winner, forget the rest. Separate from `say` so that the choice is
    // made once, where the frame ends, rather than by whichever caller happened to be last.
    void settle();

    // The audio device's thread. Everything it touches is either atomic or owned by it alone.
    void render(f32* interleaved, u32 frames) override;

    // What was actually started last, for the tests and for the developer panel. Not for logic:
    // by the time anything could read it the voice is already sounding.
    Cue last() const { return last_.load(std::memory_order_relaxed); }

private:
    // One voice is enough by construction: the rule is one sound a frame, and a cue is shorter
    // than a frame is long at any frame rate the game is playable at. A second voice would only
    // ever be a way to break the rule.
    struct Voice {
        Cue cue = Cue::None;
        f32 pitch = 1.0f;
        f32 phase = 0.0f;
        f32 age = 0.0f;      // seconds since it started
        bool live = false;
    };

    std::atomic<bool> enabled_{true};
    std::atomic<f32> volume_{0.6f};
    std::atomic<u32> rate_{48000};

    // The frame's claim, and the handover to the audio thread. A single word each way: the frame
    // writes what it wants started, the device thread takes it and clears it. No lock, because a
    // lock held on an audio callback is a click every time the frame it is waiting on is slow.
    std::atomic<u32> claim_{0};        // the cue, as a number
    std::atomic<f32> claim_pitch_{1.0f};
    std::atomic<u32> start_{0};        // what `settle` handed over, taken by `render`
    std::atomic<f32> start_pitch_{1.0f};
    std::atomic<Cue> last_{Cue::None};

    Voice voice_{};
};

// How long a cue lasts, in seconds. Exposed because the tests measure it and because "off is
// exactly silent" is only checkable against a window long enough to hold a whole cue.
f32 cue_seconds(Cue cue);

}  // namespace ws::ui
