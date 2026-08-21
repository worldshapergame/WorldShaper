// The interface's voice, and the silences that are the design rather than an omission.
//
// documentation/14-ui-style.md spends most of its feedback section on what does NOT make a sound,
// because "we forgot" and "we decided not to" look identical from outside. Two of those are
// checkable here and both are: **off is exactly silent** — zero samples, not merely quiet — and
// **one sound a frame**, the one that says the most.

#include <cmath>
#include <vector>

#include "doctest/doctest.h"
#include "ui/sound.hpp"

using namespace ws;
using namespace ws::ui;

namespace {

// Long enough to hold the longest cue whole, so "silent" is a claim about a window that would
// certainly have had something in it.
constexpr u32 kRate = 48000;
constexpr u32 kFrames = kRate / 2;

f32 loudest(const std::vector<f32>& samples) {
    f32 worst = 0.0f;
    for (f32 sample : samples) worst = std::max(worst, std::abs(sample));
    return worst;
}

std::vector<f32> render(Sound& sound) {
    std::vector<f32> samples(static_cast<usize>(kFrames) * 2, 0.5f);   // deliberately not zero
    sound.render(samples.data(), kFrames);
    return samples;
}

}  // namespace

TEST_CASE("off is exactly silent, not merely quiet") {
    Sound sound;
    sound.configure(kRate);
    sound.set_enabled(false);
    sound.say(Cue::Refuse);
    sound.settle();

    const std::vector<f32> samples = render(sound);
    // Every sample, counted. A volume of nought that still mixes an envelope is a build that
    // crackles on somebody's machine for a feature they turned off.
    for (f32 sample : samples) CHECK(sample == 0.0f);
}

TEST_CASE("a volume of nought is the same silence") {
    Sound sound;
    sound.configure(kRate);
    sound.set_volume(0.0f);
    sound.say(Cue::Commit);
    sound.settle();
    CHECK(loudest(render(sound)) == 0.0f);
}

TEST_CASE("on, a cue is actually audible") {
    Sound sound;
    sound.configure(kRate);
    sound.set_volume(1.0f);
    sound.say(Cue::Commit);
    sound.settle();
    CHECK(loudest(render(sound)) > 0.05f);
}

TEST_CASE("one sound a frame: the loudest claim wins and the rest are dropped") {
    // A single press produces several true statements at once — a value moved, a value committed,
    // a window opened — and playing all of them is a muddle two frames long for one action.
    Sound sound;
    sound.configure(kRate);
    sound.set_volume(1.0f);
    sound.say(Cue::Step);
    sound.say(Cue::Refuse);
    sound.say(Cue::Press);
    sound.settle();
    CHECK(sound.last() == Cue::Refuse);

    // And the frame after says nothing at all, because nothing was claimed.
    sound.settle();
    CHECK(sound.last() == Cue::Refuse);   // unchanged: `settle` with no claim starts nothing
}

TEST_CASE("nothing claimed is nothing started") {
    Sound sound;
    sound.configure(kRate);
    sound.set_volume(1.0f);
    sound.settle();
    CHECK(sound.last() == Cue::None);
    CHECK(loudest(render(sound)) == 0.0f);
}

TEST_CASE("every cue ends, and ends at zero") {
    // One voice is enough by construction only if a cue is over before the next one starts. Every
    // one of them decays to exactly nothing well inside a frame at any playable frame rate.
    Sound sound;
    sound.configure(kRate);
    sound.set_volume(1.0f);
    for (u32 which = 1; which < static_cast<u32>(Cue::Count); ++which) {
        const Cue cue = static_cast<Cue>(which);
        INFO("cue " << which);
        CHECK(cue_seconds(cue) > 0.0f);
        CHECK(cue_seconds(cue) < 0.25f);

        sound.say(cue);
        sound.settle();
        std::vector<f32> samples = render(sound);
        // The far end of the window is past the end of the cue, so it is silence.
        for (usize i = samples.size() - 64; i < samples.size(); ++i) CHECK(samples[i] == 0.0f);
        // And it started with an attack rather than a step, so nothing begins with a click of its
        // own: the first sample is near zero.
        CHECK(std::abs(samples[0]) < 0.05f);
    }
}

TEST_CASE("a pitch is a multiplier, so one cue says two directions") {
    // "The value went up" and "the value went down" are one cue at two pitches rather than two
    // recordings. What is checked is that the pitch reaches the waveform at all: the two renders
    // differ.
    Sound up;
    Sound down;
    up.configure(kRate);
    down.configure(kRate);
    up.set_volume(1.0f);
    down.set_volume(1.0f);
    up.say(Cue::Step, 1.5f);
    down.say(Cue::Step, 0.7f);
    up.settle();
    down.settle();

    const std::vector<f32> a = render(up);
    const std::vector<f32> b = render(down);
    bool differ = false;
    for (usize i = 0; i < 512; ++i) {
        if (std::abs(a[i] - b[i]) > 0.01f) differ = true;
    }
    CHECK(differ);
}

TEST_CASE("a slider is an instrument: low at the bottom, high at the top") {
    // *Make the sound for sliding a slider become lower pitched the lower the value is and higher
    // pitched the higher the value is, between the biggest range of frequency most humans can
    // hear.* Six octaves of it, geometric, centred so the middle of a slider is the cue's own note.
    CHECK(slide_pitch(0.5f) == doctest::Approx(1.0f).epsilon(0.001));
    CHECK(slide_pitch(0.0f) < slide_pitch(0.25f));
    CHECK(slide_pitch(0.25f) < slide_pitch(0.5f));
    CHECK(slide_pitch(0.5f) < slide_pitch(0.75f));
    CHECK(slide_pitch(0.75f) < slide_pitch(1.0f));
    // The span itself, at the two ends, against the two ends it says it covers.
    const f32 spread = kSlideHighHertz / kSlideLowHertz;
    CHECK(slide_pitch(0.0f) == doctest::Approx(1.0f / std::sqrt(spread)).epsilon(0.001));
    CHECK(slide_pitch(1.0f) == doctest::Approx(std::sqrt(spread)).epsilon(0.001));
    // Geometric rather than linear: the half-way point is the geometric mean of the two ends and
    // not their average, which is the whole reason the bottom half of a slider is audible at all.
    CHECK(slide_pitch(0.5f) * slide_pitch(0.5f) ==
          doctest::Approx(slide_pitch(0.0f) * slide_pitch(1.0f)).epsilon(0.001));
    // Anything outside the travel is held at the ends rather than allowed off the keyboard.
    CHECK(slide_pitch(-3.0f) == doctest::Approx(slide_pitch(0.0f)));
    CHECK(slide_pitch(9.0f) == doctest::Approx(slide_pitch(1.0f)));

    // And the whole span reaches the waveform: the bottom of a slider and the top of one are not
    // the same sound, which is what a clamp too narrow for the span quietly made them.
    Sound low;
    Sound high;
    low.configure(kRate);
    high.configure(kRate);
    low.set_volume(1.0f);
    high.set_volume(1.0f);
    low.say(Cue::Slide, slide_pitch(0.0f));
    high.say(Cue::Slide, slide_pitch(1.0f));
    low.settle();
    high.settle();
    const std::vector<f32> quiet = render(low);
    const std::vector<f32> loud = render(high);
    // How many times each crosses zero in the first eighth of a second, which is a frequency
    // measured rather than asserted -- the high one has to cross many times more often.
    const auto crossings = [](const std::vector<f32>& in) {
        usize count = 0;
        for (usize i = 2; i < in.size(); i += 2) {
            if ((in[i - 2] < 0.0f) != (in[i] < 0.0f)) ++count;
        }
        return count;
    };
    CHECK(crossings(loud) > crossings(quiet) * 8);
    CHECK(crossings(quiet) > 0);

    // It is the QUIETEST claim there is, so a drag that ends in a commit is heard as a commit and
    // a drag over something that refuses is heard as a refusal.
    Sound both;
    both.configure(kRate);
    both.set_volume(1.0f);
    both.say(Cue::Slide, 2.0f);
    both.say(Cue::Commit);
    both.settle();
    CHECK(both.last() == Cue::Commit);
}
