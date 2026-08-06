#pragma once
// Auto-repeat for a held key.
//
// The window layer reports the raw press and deliberately ignores the operating system's
// key repeat: OS repeat carries the user's typing settings, which are right for typing and
// wrong for a game — the delay is long, the rate varies per machine, and two players
// holding the same key would rotate a clip at different speeds. Anything shared has to be
// the same everywhere, so the rate lives here.
//
// Time-based rather than frame-based for the same reason: at 30 FPS and at 144 FPS a held
// key has to do the same thing per second.

#include "core/types.hpp"

namespace ws {

class KeyRepeat {
public:
    // The first press fires once. Holding past `delay` fires `rate` times a second after
    // that. Returns how many times the action should run this frame, which is usually 0 or
    // 1 and is more only if a frame ran long.
    u32 poll(bool down, f64 dt) {
        if (!down) {
            held_ = false;
            timer_ = 0.0;
            return 0;
        }
        if (!held_) {
            held_ = true;
            timer_ = -delay_;   // negative: counting up to the first repeat
            return 1;
        }
        timer_ += dt;
        if (timer_ < 0.0) return 0;
        const f64 interval = 1.0 / rate_;
        u32 fired = 0;
        while (timer_ >= 0.0 && fired < kMaxPerFrame) {
            ++fired;
            timer_ -= interval;
        }
        // A frame long enough to owe more than the cap drops the excess rather than
        // banking it; otherwise one stutter spins a clip through half a turn. Reset to a
        // whole interval short of the next fire, not to zero — zero is still "due now", so
        // the very next poll would fire again however short it was.
        if (fired == kMaxPerFrame) timer_ = -interval;
        return fired;
    }

    void configure(f64 delay_seconds, f64 repeats_per_second) {
        delay_ = delay_seconds;
        rate_ = repeats_per_second;
    }

private:
    static constexpr u32 kMaxPerFrame = 8;
    f64 delay_ = 0.35;
    f64 rate_ = 14.0;
    f64 timer_ = 0.0;
    bool held_ = false;
};

}  // namespace ws
