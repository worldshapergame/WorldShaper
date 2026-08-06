#include "game/toolbelt.hpp"

#include <algorithm>

#include "core/assert.hpp"

namespace ws {

const char* tool_name(ToolKind tool) {
    switch (tool) {
        case ToolKind::Chisel:    return "chisel";
        case ToolKind::Clipboard: return "clipboard";
        default:                  return "none";
    }
}

Toolbelt::Toolbelt() {
    // One tool per slot to begin with: the chisel on 1, the clipboard on 2. Stacking more
    // than one on a slot is what holding the number and scrolling is for, and rearranging
    // the belt is part of the interface stage.
    slots_[0].tools.push_back(ToolKind::Chisel);
    slots_[1].tools.push_back(ToolKind::Clipboard);
}

void Toolbelt::assign(u32 slot, ToolKind tool) {
    WS_ASSERT(slot < kToolSlots, "tool slot {} out of range", slot);
    std::vector<ToolKind>& tools = slots_[slot].tools;
    if (std::find(tools.begin(), tools.end(), tool) == tools.end()) tools.push_back(tool);
}

void Toolbelt::clear(u32 slot) {
    WS_ASSERT(slot < kToolSlots, "tool slot {} out of range", slot);
    slots_[slot].tools.clear();
    slots_[slot].current = 0;
}

bool Toolbelt::select_slot(u32 slot) {
    WS_ASSERT(slot < kToolSlots, "tool slot {} out of range", slot);
    // An empty slot is not a selection. Silently moving to a slot with nothing on it would
    // leave the player holding no tool and no way to tell why.
    if (slots_[slot].tools.empty()) return false;
    const ToolKind before = active();
    active_slot_ = slot;
    return active() != before;
}

bool Toolbelt::cycle(i32 steps) {
    Slot& slot = slots_[active_slot_];
    if (slot.tools.size() < 2 || steps == 0) return false;
    const u32 count = static_cast<u32>(slot.tools.size());
    const ToolKind before = active();
    // Wrapped with a modulo that is correct for negative steps; scrolling back past the
    // first tool lands on the last.
    const i64 moved = static_cast<i64>(slot.current) + steps;
    const i64 wrapped = ((moved % count) + count) % count;
    slot.current = static_cast<u32>(wrapped);
    return active() != before;
}

ToolKind Toolbelt::active() const {
    const Slot& slot = slots_[active_slot_];
    if (slot.tools.empty()) return ToolKind::Count;
    return slot.tools[slot.current];
}

bool Toolbelt::slot_empty(u32 slot) const { return slots_[slot].tools.empty(); }
u32 Toolbelt::slot_size(u32 slot) const { return static_cast<u32>(slots_[slot].tools.size()); }
u32 Toolbelt::slot_position(u32 slot) const { return slots_[slot].current; }

}  // namespace ws
