#include "world/property.hpp"

#include <algorithm>
#include <bit>

#include "core/assert.hpp"
#include "core/hash.hpp"
#include "core/log.hpp"

namespace ws {

PropertyValue PropertyValue::from_int(i64 v) { return PropertyValue{static_cast<u64>(v)}; }
PropertyValue PropertyValue::from_uint(u64 v) { return PropertyValue{v}; }
PropertyValue PropertyValue::from_bool(bool v) { return PropertyValue{v ? 1ull : 0ull}; }
PropertyValue PropertyValue::from_float(f32 v) {
    return PropertyValue{static_cast<u64>(std::bit_cast<u32>(v))};
}

i64 PropertyValue::as_int() const { return static_cast<i64>(bits); }
u64 PropertyValue::as_uint() const { return bits; }
bool PropertyValue::as_bool() const { return bits != 0; }
f32 PropertyValue::as_float() const { return std::bit_cast<f32>(static_cast<u32>(bits)); }

PropertyRegistry::PropertyRegistry() {
    // Order matters: these ids are referenced by name in ws::props and are baked into
    // saves. Append, never insert.
    define("density", PropertyType::Uint, PropertyDomain::Simulation,
           PropertyValue::from_uint(1000));
    define("friction", PropertyType::Uint, PropertyDomain::Simulation,
           PropertyValue::from_uint(128));
    define("hardness", PropertyType::Uint, PropertyDomain::Simulation,
           PropertyValue::from_uint(128));
    define("phase", PropertyType::Uint, PropertyDomain::Simulation,
           PropertyValue::from_uint(static_cast<u64>(Phase::Solid)));
    define("ignition_temp", PropertyType::Uint, PropertyDomain::Simulation,
           PropertyValue::from_uint(0));
    define("melt_temp", PropertyType::Uint, PropertyDomain::Simulation,
           PropertyValue::from_uint(0));
    define("conductivity", PropertyType::Uint, PropertyDomain::Simulation,
           PropertyValue::from_uint(64));
    define("viscosity", PropertyType::Uint, PropertyDomain::Simulation,
           PropertyValue::from_uint(0));
    define("gravity_warp", PropertyType::Int, PropertyDomain::Simulation,
           PropertyValue::from_int(0));

    invalid_.name = "<invalid>";
}

PropertyId PropertyRegistry::define(std::string_view name, PropertyType type,
                                    PropertyDomain domain, PropertyValue default_value) {
    if (domain == PropertyDomain::Simulation && type == PropertyType::Float) {
        // Rejecting this here is the cheapest possible enforcement of the rule that keeps
        // multiplayer working. See documentation/05-simulation.md §12.
        WS_LOG_ERROR("property",
                     "'{}' rejected: simulation properties must be integer, not float",
                     name);
        return kInvalidProperty;
    }

    const std::string key(name);
    const auto found = by_name_.find(key);
    if (found != by_name_.end()) {
        const PropertyInfo& existing = properties_[found->second];
        if (existing.type != type || existing.domain != domain) {
            WS_LOG_ERROR("property", "'{}' redefined with a different type or domain", name);
            return kInvalidProperty;
        }
        return found->second;
    }

    const PropertyId id = static_cast<PropertyId>(properties_.size());
    properties_.push_back(PropertyInfo{key, type, domain, default_value});
    by_name_.emplace(key, id);
    return id;
}

PropertyId PropertyRegistry::find(std::string_view name) const {
    const auto found = by_name_.find(std::string(name));
    return (found != by_name_.end()) ? found->second : kInvalidProperty;
}

const PropertyInfo& PropertyRegistry::info(PropertyId id) const {
    if (id >= properties_.size()) return invalid_;
    return properties_[id];
}

void PropertyMap::set(PropertyId id, PropertyValue value) {
    WS_ASSERT(id != kInvalidProperty, "cannot set an invalid property");
    const auto at = std::lower_bound(
        entries_.begin(), entries_.end(), id,
        [](const Entry& entry, PropertyId key) { return entry.id < key; });
    if (at != entries_.end() && at->id == id) {
        at->value = value;
        return;
    }
    entries_.insert(at, Entry{id, value});
}

void PropertyMap::clear(PropertyId id) {
    const auto at = std::lower_bound(
        entries_.begin(), entries_.end(), id,
        [](const Entry& entry, PropertyId key) { return entry.id < key; });
    if (at != entries_.end() && at->id == id) entries_.erase(at);
}

bool PropertyMap::has(PropertyId id) const {
    const auto at = std::lower_bound(
        entries_.begin(), entries_.end(), id,
        [](const Entry& entry, PropertyId key) { return entry.id < key; });
    return at != entries_.end() && at->id == id;
}

PropertyValue PropertyMap::get(const PropertyRegistry& registry, PropertyId id) const {
    const auto at = std::lower_bound(
        entries_.begin(), entries_.end(), id,
        [](const Entry& entry, PropertyId key) { return entry.id < key; });
    if (at != entries_.end() && at->id == id) return at->value;
    return registry.info(id).default_value;
}

u64 PropertyMap::content_hash() const {
    u64 h = 0x9E3779B1ull;
    for (const Entry& entry : entries_) {
        h = hash_combine(h, entry.id);
        h = hash_combine(h, entry.value.bits);
    }
    return h;
}

}  // namespace ws
