#include "world/tags.hpp"

#include <algorithm>

#include "core/assert.hpp"

namespace ws {

void TagSet::add(TagId tag) {
    WS_ASSERT(tag != kInvalidTag, "cannot add an invalid tag");
    if (tag < kFastTagBits) {
        fast_[tag / 64] |= (u64{1} << (tag % 64));
        return;
    }
    const auto at = std::lower_bound(overflow_.begin(), overflow_.end(), tag);
    if (at == overflow_.end() || *at != tag) overflow_.insert(at, tag);
}

void TagSet::remove(TagId tag) {
    if (tag < kFastTagBits) {
        fast_[tag / 64] &= ~(u64{1} << (tag % 64));
        return;
    }
    const auto at = std::lower_bound(overflow_.begin(), overflow_.end(), tag);
    if (at != overflow_.end() && *at == tag) overflow_.erase(at);
}

bool TagSet::has(TagId tag) const {
    if (tag == kInvalidTag) return false;
    if (tag < kFastTagBits) return (fast_[tag / 64] & (u64{1} << (tag % 64))) != 0;
    return std::binary_search(overflow_.begin(), overflow_.end(), tag);
}

bool TagSet::empty() const {
    for (u64 word : fast_) {
        if (word != 0) return false;
    }
    return overflow_.empty();
}

bool TagSet::has_all(const TagSet& required) const {
    for (u32 i = 0; i < kFastTagWords; ++i) {
        if ((fast_[i] & required.fast_[i]) != required.fast_[i]) return false;
    }
    for (TagId tag : required.overflow_) {
        if (!has(tag)) return false;
    }
    return true;
}

bool TagSet::has_any(const TagSet& any_of) const {
    for (u32 i = 0; i < kFastTagWords; ++i) {
        if ((fast_[i] & any_of.fast_[i]) != 0) return true;
    }
    for (TagId tag : any_of.overflow_) {
        if (has(tag)) return true;
    }
    return false;
}

u64 TagSet::content_hash() const {
    u64 h = 0x5BF03635ull;
    for (u64 word : fast_) h = hash_combine(h, word);
    for (TagId tag : overflow_) h = hash_combine(h, tag);
    return h;
}

bool TagSet::operator==(const TagSet& other) const {
    for (u32 i = 0; i < kFastTagWords; ++i) {
        if (fast_[i] != other.fast_[i]) return false;
    }
    return overflow_ == other.overflow_;
}

TagRegistry::TagRegistry() {
    // Tags the engine itself tests. Registered first so they land in the fast range and
    // keep stable ids across worlds, which matters for save compatibility.
    static const char* kBuiltIn[] = {
        "air",        "solid",      "fluid",       "gas",        "granular",
        "rope",       "cloth",      "soft",        "stone",      "soil",
        "sand",       "wood",       "metal",       "glass",      "ice",
        "organic",    "flammable",  "oxidizer",    "conductive", "insulator",
        "magnetic",   "buildable",  "diggable",    "opaque",     "transparent",
        "emissive",   "reflective", "refractive",  "dispersive", "caustic_source",
        "smooth_surface", "gravity_warp", "sticky", "slippery",  "corrosive",
        "explosive",  "radioactive", "ore",        "gem",        "liquid_source",
    };
    for (const char* name : kBuiltIn) intern(name);
}

TagId TagRegistry::intern(std::string_view name) {
    const std::string key(name);
    const auto found = by_name_.find(key);
    if (found != by_name_.end()) return found->second;

    const TagId id = static_cast<TagId>(names_.size());
    names_.push_back(key);
    by_name_.emplace(key, id);
    return id;
}

TagId TagRegistry::find(std::string_view name) const {
    const auto found = by_name_.find(std::string(name));
    return (found != by_name_.end()) ? found->second : kInvalidTag;
}

std::string_view TagRegistry::name(TagId tag) const {
    if (tag >= names_.size()) return {};
    return names_[tag];
}

u32 TagRegistry::fast_count() const {
    return std::min<u32>(static_cast<u32>(names_.size()), kFastTagBits);
}

}  // namespace ws
