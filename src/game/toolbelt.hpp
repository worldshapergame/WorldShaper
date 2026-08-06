#pragma once
// The tool belt: nine slots, each holding any number of tools.
//
// Tapping a number picks that slot's current tool. Holding the number and scrolling cycles
// through everything assigned to it, so one key can carry a whole family — every shaping
// tool on 1, every measuring tool on 2 — without the belt growing a row of icons for each.
//
// The wheel is deliberately claimed only while a number is held. It is the busiest input in
// the game: the chisel takes it with G to set a working distance, the clipboard takes it
// bare to slide a clone along the view direction, and the free camera takes it for speed.
// Making plain scroll mean "change tool" would have taken it away from the tool currently
// in use, which is the one that needs it.

#include <vector>

#include "core/types.hpp"

namespace ws {

enum class ToolKind : u8 {
    Chisel = 0,
    Clipboard,
    Count,
};

const char* tool_name(ToolKind tool);

inline constexpr u32 kToolSlots = 9;

class Toolbelt {
public:
    Toolbelt();

    // Slots are 0-based here and shown 1-based; the input layer does the conversion.
    void assign(u32 slot, ToolKind tool);
    void clear(u32 slot);

    // Returns true when the active tool changed, which is what a tool needs in order to
    // abandon whatever it had half-finished.
    bool select_slot(u32 slot);
    bool cycle(i32 steps);

    ToolKind active() const;
    u32 active_slot() const { return active_slot_; }
    bool slot_empty(u32 slot) const;
    u32 slot_size(u32 slot) const;
    u32 slot_position(u32 slot) const;
    const std::vector<ToolKind>& slot_tools(u32 slot) const { return slots_[slot].tools; }

private:
    struct Slot {
        std::vector<ToolKind> tools;
        u32 current = 0;
    };

    Slot slots_[kToolSlots];
    u32 active_slot_ = 0;
};

}  // namespace ws
