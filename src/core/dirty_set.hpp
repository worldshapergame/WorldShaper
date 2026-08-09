#pragma once
// Which elements of one array changed this frame, so an upload copies those rather than the whole
// used prefix.
//
// Shared by every structure that mirrors a CPU array onto the card. It is here rather than beside
// the first one that needed it because the second one needing it is how a coalescing rule ends up
// written twice and drifting -- and a dirty map that disagrees with itself is a stale byte on the
// GPU, which is the one class of fault that shows up as a wrong picture rather than an error.

#include <utility>
#include <vector>

#include "core/types.hpp"

namespace ws {

// Which elements of one array changed this frame.
//
// A bitmap rather than a list of indices, because a single build touches the same node several
// times over -- once when it is allocated, again when its own children are folded into it, again
// when its parent folds it in -- and a list would send it once per touch. It also has to be
// cheap to mark, since a busy frame marks tens of thousands of times.
//
// Runs are coalesced with a gap tolerance on the way out: two dirty blocks with one clean block
// between them go as a single copy, because a copy has a fixed cost of its own and sending four
// spare kilobytes is cheaper than issuing a second one.
class DirtySet {
public:
    void create(u64 count) {
        count_ = count;
        words_.assign(static_cast<usize>((count + 63) / 64), 0ull);
        marked_ = 0;
    }
    void clear() {
        if (marked_ != 0) std::fill(words_.begin(), words_.end(), 0ull);
        marked_ = 0;
    }
    void mark(u64 index) {
        if (index >= count_) return;
        const u64 word = index >> 6;
        const u64 bit = 1ull << (index & 63);
        if ((words_[static_cast<usize>(word)] & bit) == 0) {
            words_[static_cast<usize>(word)] |= bit;
            ++marked_;
        }
    }
    void mark_range(u64 first, u64 count) {
        for (u64 i = 0; i < count; ++i) mark(first + i);
    }
    bool empty() const { return marked_ == 0; }
    u64 marked() const { return marked_; }

    // Coalesced runs of dirty elements, as (first, count). `gap` is how many clean elements may
    // sit inside a run before it is worth splitting.
    std::vector<std::pair<u64, u64>> runs(u64 gap) const {
        std::vector<std::pair<u64, u64>> out;
        u64 i = 0;
        while (i < count_) {
            if (!test(i)) { ++i; continue; }
            u64 first = i;
            u64 last = i;
            ++i;
            while (i < count_) {
                if (test(i)) { last = i; ++i; continue; }
                // Look ahead over the gap rather than ending the run immediately.
                u64 probe = i;
                bool found = false;
                while (probe < count_ && probe - last <= gap) {
                    if (test(probe)) { found = true; break; }
                    ++probe;
                }
                if (!found) break;
                last = probe;
                i = probe + 1;
            }
            out.emplace_back(first, last - first + 1);
        }
        return out;
    }

private:
    bool test(u64 index) const {
        return (words_[static_cast<usize>(index >> 6)] >> (index & 63)) & 1ull;
    }
    std::vector<u64> words_;
    u64 count_ = 0;
    u64 marked_ = 0;
};


}  // namespace ws
