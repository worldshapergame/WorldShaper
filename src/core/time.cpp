#include "core/time.hpp"

#include <algorithm>
#include <chrono>

namespace ws {

u64 now_ns() {
    using clock = std::chrono::steady_clock;
    static const clock::time_point start = clock::now();
    return static_cast<u64>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - start).count());
}

void FrameStats::push(f64 frame_ms) {
    last_ms_ = frame_ms;
    samples_[cursor_] = frame_ms;
    cursor_ = (cursor_ + 1) % kWindow;
    if (count_ < kWindow) ++count_;
    ++frame_index_;
}

f64 FrameStats::average_ms() const {
    if (count_ == 0) return 0.0;
    f64 sum = 0.0;
    for (usize i = 0; i < count_; ++i) sum += samples_[i];
    return sum / static_cast<f64>(count_);
}

f64 FrameStats::percentile_ms(f64 fraction) const {
    if (count_ == 0) return 0.0;
    f64 sorted[kWindow];
    std::copy(samples_, samples_ + count_, sorted);
    std::sort(sorted, sorted + count_);
    usize index = static_cast<usize>(fraction * static_cast<f64>(count_ - 1) + 0.5);
    if (index >= count_) index = count_ - 1;
    return sorted[index];
}

f64 FrameStats::max_ms() const {
    if (count_ == 0) return 0.0;
    return *std::max_element(samples_, samples_ + count_);
}

}  // namespace ws
