#pragma once
// Tags.
//
// A tag is a named label on a voxel type: `stone`, `flammable`, `conductive`, `ore`.
// documentation/03-voxel-data-model.md §4 — a voxel type may carry unlimited tags, but
// the ones simulation and rendering test every tick live in a 256-bit bitset so the
// question "is this flammable?" is one AND rather than a search.
//
// This is what makes reaction rules general: "wood burns" is written once against the
// `flammable` tag and applies to every material anyone ever authors, including the
// player's own.

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "core/hash.hpp"
#include "core/types.hpp"

namespace ws {

using TagId = u32;
inline constexpr TagId kInvalidTag = 0xFFFFFFFFu;

// The number of tags that get a bit in the fast set. Beyond this a tag still works
// everywhere on the CPU and in scripts; it simply is not one AND away on the GPU.
inline constexpr u32 kFastTagBits = 256;
inline constexpr u32 kFastTagWords = kFastTagBits / 64;

class TagSet {
public:
    void add(TagId tag);
    void remove(TagId tag);
    bool has(TagId tag) const;
    bool empty() const;

    // True when every tag in `required` is present. This is the operation reaction rules
    // run millions of times a tick.
    bool has_all(const TagSet& required) const;
    bool has_any(const TagSet& any_of) const;

    const u64* fast_words() const { return fast_; }
    const std::vector<TagId>& overflow() const { return overflow_; }

    u64 content_hash() const;
    bool operator==(const TagSet& other) const;

private:
    u64 fast_[kFastTagWords]{};
    std::vector<TagId> overflow_;   // sorted; tags at or beyond kFastTagBits
};

// Names are interned once per world. Bit assignment is by registration order, so the
// hottest tags — the ones the base game registers first — land in the fast range.
class TagRegistry {
public:
    TagRegistry();

    TagId intern(std::string_view name);
    TagId find(std::string_view name) const;   // kInvalidTag when unknown
    std::string_view name(TagId tag) const;
    u32 count() const { return static_cast<u32>(names_.size()); }
    u32 fast_count() const;

private:
    std::vector<std::string> names_;
    std::unordered_map<std::string, TagId> by_name_;
};

}  // namespace ws
