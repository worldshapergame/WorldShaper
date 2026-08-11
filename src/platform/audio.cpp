#include "platform/audio.hpp"

#include <SDL3/SDL.h>

#include <vector>

#include "core/log.hpp"

namespace ws {
namespace {

// The device asks for more samples; we render exactly what it asked for and no more.
//
// SDL calls this on its own thread with a byte count that varies frame to frame, so the scratch
// buffer is grown to whatever the largest request has been and then reused. Allocating inside an
// audio callback is the classic way to produce a click on the one machine you do not have.
void feed(void* user, SDL_AudioStream* stream, int additional, int total) {
    (void)total;
    if (additional <= 0) return;
    auto* source = static_cast<AudioSource*>(user);

    static thread_local std::vector<f32> scratch;
    const int floats = additional / static_cast<int>(sizeof(f32));
    const int frames = floats / 2;
    if (frames <= 0) return;
    if (static_cast<int>(scratch.size()) < floats) scratch.resize(static_cast<usize>(floats));

    source->render(scratch.data(), static_cast<u32>(frames));
    SDL_PutAudioStreamData(stream, scratch.data(), frames * 2 * static_cast<int>(sizeof(f32)));
}

}  // namespace

Audio::~Audio() { destroy(); }

bool Audio::create(AudioSource& source) {
    // The window has already brought SDL up for video; this adds the subsystem rather than
    // initialising SDL a second time, and remembers whether it was the one that did so.
    if (!SDL_WasInit(SDL_INIT_AUDIO)) {
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
            WS_LOG_WARN("audio", "no audio this run: {}", SDL_GetError());
            return false;
        }
        owns_subsystem_ = true;
    }

    source_ = &source;

    SDL_AudioSpec spec{};
    spec.format = SDL_AUDIO_F32;
    spec.channels = 2;
    spec.freq = 48000;

    stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, feed, source_);
    if (stream_ == nullptr) {
        WS_LOG_WARN("audio", "no playback device: {}", SDL_GetError());
        if (owns_subsystem_) {
            SDL_QuitSubSystem(SDL_INIT_AUDIO);
            owns_subsystem_ = false;
        }
        return false;
    }

    // What the device actually took, which need not be what was asked for. The synthesis is exact
    // at whatever rate it is told, so this is a number to pass on rather than a thing to resample.
    SDL_AudioSpec got{};
    const SDL_AudioDeviceID device = SDL_GetAudioStreamDevice(stream_);
    if (SDL_GetAudioDeviceFormat(device, &got, nullptr)) {
        rate_ = static_cast<u32>(got.freq);
    } else {
        rate_ = static_cast<u32>(spec.freq);
    }

    SDL_ResumeAudioStreamDevice(stream_);
    WS_LOG_INFO("audio", "playback at {} Hz", rate_);
    return true;
}

void Audio::destroy() {
    if (stream_ != nullptr) {
        // Destroying the stream stops the callback before this returns, which is what makes it
        // safe for the source to go away on the next line.
        SDL_DestroyAudioStream(stream_);
        stream_ = nullptr;
    }
    source_ = nullptr;
    if (owns_subsystem_) {
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        owns_subsystem_ = false;
    }
}

}  // namespace ws
