#pragma once
// The audio device, and the only other file that knows SDL exists.
//
// It is deliberately almost nothing: an interface that produces samples, a device that asks for
// them, and a failure path that costs silence rather than a start-up error. A game that will not
// open because a machine has no sound card has traded the thing for the report on the thing —
// which is the same argument the loading screen's pipeline makes for itself.
//
// The synthesis lives in src/ui/sound.cpp, which knows nothing about devices and can therefore be
// rendered into a buffer by a test. This end knows nothing about cues.

#include <string>

#include "core/types.hpp"

struct SDL_AudioStream;

namespace ws {

// Whatever is making the noise. Called on the device's own thread, so an implementation may touch
// nothing the frame loop is also touching without saying how it is safe to.
class AudioSource {
public:
    virtual ~AudioSource() = default;
    // `interleaved` holds `frames` pairs of stereo float samples and is not cleared beforehand:
    // filling all of it, with zeros where there is nothing to say, is the implementation's job.
    virtual void render(f32* interleaved, u32 frames) = 0;
};

class Audio {
public:
    ~Audio();

    Audio() = default;
    Audio(const Audio&) = delete;
    Audio& operator=(const Audio&) = delete;

    // Opens the default playback device and starts pulling from `source`, which must outlive this.
    // Returns false and logs one line when there is no device; everything above carries on.
    bool create(AudioSource& source);
    void destroy();

    bool valid() const { return stream_ != nullptr; }
    u32 sample_rate() const { return rate_; }

private:
    SDL_AudioStream* stream_ = nullptr;
    AudioSource* source_ = nullptr;
    u32 rate_ = 0;
    bool owns_subsystem_ = false;
};

}  // namespace ws
