#pragma once
// Properties.
//
// documentation/03-voxel-data-model.md §4: properties are an open registry, not a fixed
// struct. Adding `viscosity` or a mod's own `mana_conductivity` never touches engine
// code, and a voxel type stores only what it overrides.
//
// The domain split is load-bearing rather than tidy:
//
//   Domain::Simulation  — anything that can change the world. Integer types ONLY,
//                         because two machines must agree bit for bit
//                         (documentation/05-simulation.md §12 rule 1).
//   Domain::Visual      — anything only the renderer reads. Floats are fine here;
//                         nothing downstream of them is shared state.
//
// Registering a Simulation property with a floating-point type is rejected at
// registration, so the rule cannot be broken by accident three stages from now.

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "core/types.hpp"

namespace ws {

using PropertyId = u32;
inline constexpr PropertyId kInvalidProperty = 0xFFFFFFFFu;

enum class PropertyType : u8 { Int, Uint, Bool, Float };
enum class PropertyDomain : u8 { Simulation, Visual };

// One property value. Stored as raw bits so the registry can hold any type without a
// variant and without allocation.
struct PropertyValue {
    u64 bits = 0;

    static PropertyValue from_int(i64 v);
    static PropertyValue from_uint(u64 v);
    static PropertyValue from_bool(bool v);
    static PropertyValue from_float(f32 v);

    i64 as_int() const;
    u64 as_uint() const;
    bool as_bool() const;
    f32 as_float() const;

    bool operator==(const PropertyValue& other) const { return bits == other.bits; }
};

struct PropertyInfo {
    std::string name;
    PropertyType type = PropertyType::Int;
    PropertyDomain domain = PropertyDomain::Simulation;
    PropertyValue default_value;
};

class PropertyRegistry {
public:
    PropertyRegistry();

    // Returns kInvalidProperty if the definition is rejected (a float in the simulation
    // domain, or a redefinition with a different type).
    PropertyId define(std::string_view name, PropertyType type, PropertyDomain domain,
                      PropertyValue default_value);

    PropertyId find(std::string_view name) const;
    const PropertyInfo& info(PropertyId id) const;
    u32 count() const { return static_cast<u32>(properties_.size()); }

private:
    std::vector<PropertyInfo> properties_;
    std::unordered_map<std::string, PropertyId> by_name_;
    PropertyInfo invalid_;
};

// A sparse set of overrides. Sorted by id so two maps with the same contents compare and
// hash identically — which is what lets behaviour records be deduplicated.
class PropertyMap {
public:
    void set(PropertyId id, PropertyValue value);
    void clear(PropertyId id);
    bool has(PropertyId id) const;

    // Falls back to the registry default when the property is not overridden.
    PropertyValue get(const PropertyRegistry& registry, PropertyId id) const;

    usize size() const { return entries_.size(); }
    u64 content_hash() const;
    bool operator==(const PropertyMap& other) const { return entries_ == other.entries_; }

    struct Entry {
        PropertyId id;
        PropertyValue value;
        bool operator==(const Entry&) const = default;
    };
    const std::vector<Entry>& entries() const { return entries_; }

private:
    std::vector<Entry> entries_;
};

// The properties the engine itself reads. Ids are stable because these are registered
// first, in this order, by PropertyRegistry's constructor.
namespace props {
inline constexpr PropertyId kDensity      = 0;   // kg/m³
inline constexpr PropertyId kFriction     = 1;   // 0–255
inline constexpr PropertyId kHardness     = 2;   // 0–255
inline constexpr PropertyId kPhase        = 3;   // enum, see Phase
inline constexpr PropertyId kIgnitionTemp = 4;   // kelvin
inline constexpr PropertyId kMeltTemp     = 5;   // kelvin
inline constexpr PropertyId kConductivity = 6;   // 0–255
inline constexpr PropertyId kViscosity    = 7;   // 0–255
inline constexpr PropertyId kGravityWarp  = 8;   // signed: >0 attracts, <0 repels
}  // namespace props

enum class Phase : u8 { Solid = 0, SoftSolid, Granular, Fluid, Gas, Rope, Cloth };

}  // namespace ws
