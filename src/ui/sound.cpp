#include "ui/sound.hpp"

#include <algorithm>
#include <cmath>

namespace ws::ui {
namespace {

constexpr f32 kPi = 3.14159265358979f;

// What each cue is made of. A cue is an envelope over a waveform and nothing else — no filter, no
// reverb, no second oscillator — because the job is to be *noticed and forgotten*, and anything
// with a tail in it is a sound the player hears again while doing the next thing.
struct Shape {
    f32 seconds;
    f32 hertz;      // where it starts
    f32 sweep;      // and the multiple of that it ends on: 1 is level, 2 is an octave up
    f32 gain;
    f32 square;     // 0 is a sine, 1 is a square. A click needs edges; a chime does not.
};

constexpr Shape kShapes[static_cast<usize>(Cue::Count)] = {
    {0.000f,    0.0f, 1.00f, 0.00f, 0.0f},   // None
    // Slide: a notch under the handle. Longer than `Step` and with a fall in it, because a tick
    // with no fall is a click and a click is what a rattle is made of — and loud enough to be
    // felt, which is what documentation/14-ui-style.md means by *more than feels necessary*.
    // 880 Hz is the middle of its six octaves, so pitch 1 is the middle of the slider.
    {0.034f,  880.0f, 0.84f, 0.30f, 0.45f},  // Slide
    {0.018f,  900.0f, 1.00f, 0.18f, 0.7f},   // Step: a notch, as short as a notch
    {0.035f,  520.0f, 0.92f, 0.35f, 0.4f},   // Press
    {0.110f,  330.0f, 1.60f, 0.32f, 0.0f},   // Open: it goes up, because something appeared
    {0.110f,  520.0f, 0.62f, 0.32f, 0.0f},   // Close: and down, because something went away
    {0.075f,  660.0f, 1.33f, 0.40f, 0.0f},   // Commit: a fifth, arrived at rather than held
    {0.160f,  220.0f, 0.55f, 0.34f, 0.2f},   // Erase: a drop, and the longest thing here
    {0.190f,  150.0f, 0.94f, 0.45f, 1.0f},   // Refuse: low, square, and unmistakably not a yes
};

// The envelope. A few milliseconds of attack so nothing starts with a click of its own, then a
// decay to nothing — every cue ends at exactly zero, which is what lets one voice be enough.
f32 envelope(f32 age, f32 seconds) {
    if (age < 0.0f || age >= seconds) return 0.0f;
    constexpr f32 kAttack = 0.004f;
    const f32 rise = std::min(1.0f, age / kAttack);
    const f32 left = 1.0f - (age / seconds);
    return rise * left * left;   // squared, so the tail is short rather than merely quiet
}

f32 wave(f32 phase, f32 square) {
    const f32 sine = std::sin(phase * 2.0f * kPi);
    const f32 edged = (std::fmod(phase, 1.0f) < 0.5f) ? 1.0f : -1.0f;
    return sine + (edged - sine) * square;
}

}  // namespace

f32 slide_pitch(f32 through) {
    const f32 at = std::clamp(through, 0.0f, 1.0f);
    // The base is the geometric MIDDLE of the span, so this is the span's ratio raised to how far
    // along the travel the handle is, centred: 1/8 at the bottom, 8 at the top, 1 in the middle.
    const f32 spread = kSlideHighHertz / kSlideLowHertz;
    return std::pow(spread, at - 0.5f);
}

f32 cue_seconds(Cue cue) {
    const usize index = static_cast<usize>(cue);
    if (index >= static_cast<usize>(Cue::Count)) return 0.0f;
    return kShapes[index].seconds;
}

void Sound::configure(u32 sample_rate) {
    rate_.store(sample_rate > 0 ? sample_rate : 48000u, std::memory_order_relaxed);
}

void Sound::set_volume(f32 volume) {
    volume_.store(std::clamp(volume, 0.0f, 1.0f), std::memory_order_relaxed);
}

void Sound::say(Cue cue, f32 pitch) {
    if (cue == Cue::None) return;
    // The loudest claim wins, and it wins here rather than at the end of the frame, so that the
    // whole of a frame's claims cost one compare each and nothing is stored.
    const u32 want = static_cast<u32>(cue);
    u32 held = claim_.load(std::memory_order_relaxed);
    while (want > held) {
        if (claim_.compare_exchange_weak(held, want, std::memory_order_relaxed)) {
            claim_pitch_.store(pitch, std::memory_order_relaxed);
            return;
        }
    }
}

void Sound::settle() {
    const u32 claim = claim_.exchange(0, std::memory_order_relaxed);
    if (claim == 0) return;
    // Off is exactly silent: nothing is handed over at all, so `render` has nothing to mix and
    // writes zeros because that is all there is to write.
    if (!enabled_.load(std::memory_order_relaxed) ||
        volume_.load(std::memory_order_relaxed) <= 0.0f) {
        return;
    }
    start_pitch_.store(claim_pitch_.load(std::memory_order_relaxed), std::memory_order_relaxed);
    start_.store(claim, std::memory_order_release);
    last_.store(static_cast<Cue>(claim), std::memory_order_relaxed);
}

void Sound::render(f32* interleaved, u32 frames) {
    for (u32 i = 0; i < frames * 2; ++i) interleaved[i] = 0.0f;

    const u32 started = start_.exchange(0, std::memory_order_acquire);
    if (started != 0) {
        voice_.cue = static_cast<Cue>(started);
        // Wide enough for the six octaves a slider spans (`slide_pitch`), which is what this was
        // narrower than: clamped at four, the top third of every slider sounded the same note.
        voice_.pitch = std::clamp(start_pitch_.load(std::memory_order_relaxed), 0.10f, 8.5f);
        voice_.phase = 0.0f;
        voice_.age = 0.0f;
        voice_.live = true;
    }
    if (!voice_.live) return;

    // The switch being turned off mid-cue cuts it rather than fading it. A cue is under a fifth of
    // a second: anything else would be a fade nobody hears and a branch everybody pays for.
    if (!enabled_.load(std::memory_order_relaxed)) {
        voice_.live = false;
        return;
    }

    const Shape& shape = kShapes[static_cast<usize>(voice_.cue)];
    const f32 rate = static_cast<f32>(rate_.load(std::memory_order_relaxed));
    const f32 gain = shape.gain * volume_.load(std::memory_order_relaxed);
    const f32 step = 1.0f / rate;

    for (u32 frame = 0; frame < frames; ++frame) {
        if (voice_.age >= shape.seconds) {
            voice_.live = false;
            break;
        }
        const f32 through = voice_.age / shape.seconds;
        // The sweep is geometric, because pitch is: a linear ramp from 330 to 528 spends most of
        // itself in the top fifth of the interval and reads as a bend rather than as an interval.
        const f32 hertz = shape.hertz * voice_.pitch * std::pow(shape.sweep, through);
        // **The edges come out as the pitch climbs.** A square wave is a stack of odd harmonics
        // and the device has a ceiling: at seven kilohertz the third harmonic is past twenty and
        // folds back down as a whine that has nothing to do with the note. Taking the edges out is
        // also what a real detent does — a small tick is duller the higher it is — so the same one
        // line is the anti-aliasing and the voice.
        const f32 edges = shape.square * std::clamp(1.0f - hertz / 4000.0f, 0.0f, 1.0f);
        const f32 sample = wave(voice_.phase, edges) * envelope(voice_.age, shape.seconds) * gain;
        interleaved[frame * 2] = sample;
        interleaved[frame * 2 + 1] = sample;
        voice_.phase += hertz * step;
        if (voice_.phase > 1.0f) voice_.phase -= std::floor(voice_.phase);
        voice_.age += step;
    }
}

}  // namespace ws::ui
