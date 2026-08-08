#pragma once
// Voxel types.
//
// This is the answer to "every voxel can have its own colour, tags, properties and
// transparency" without spending gigabytes on a world where almost every voxel is the
// same as its neighbour (documentation/03-voxel-data-model.md §2, decision D51).
//
// A voxel stores a 32-bit VoxelTypeId. A type is a pair of interned records:
//
//   VisualRecord     what it LOOKS like — 16 bytes, deduplicated, NEVER merged.
//   BehaviourRecord  what it IS and DOES — tags, properties, script. Deduplicated
//                    aggressively, and near-identical records may be merged to reclaim
//                    space (answer O4), because "how many genuinely distinct substances
//                    does a world have" has a much smaller answer than "how many
//                    distinct colours".
//
// Cost, therefore: a million-voxel wall of one colour costs one record. Painting one
// voxel differently costs one more record, for that voxel. Sameness is free; uniqueness
// costs exactly what it is.

#include <unordered_map>
#include <vector>

#include "core/types.hpp"
#include "world/property.hpp"
#include "world/tags.hpp"

namespace ws {

using VoxelTypeId = u32;
using VisualId = u32;
using BehaviourId = u32;

// Reserved: id 0 is always empty space, in every world, forever.
inline constexpr VoxelTypeId kAir = 0;
inline constexpr VisualId kAirVisual = 0;
inline constexpr BehaviourId kAirBehaviour = 0;

enum class VisualFlags : u8 {
    None = 0,
    SmoothSurface = 1 << 0,   // normal from the fill gradient, for water and lenses
    Dispersive = 1 << 1,      // wavelength-dependent IOR (documentation/04 §3)

    // Bits 3 and 4 are a two-bit field, not three flags: which world axis the grain of a
    // brushed surface runs along. A voxel world can honestly name exactly one thing here — it
    // has no UVs and no tangent frame, and the only directions that lie in a face's plane and
    // are still the same direction on the next face round a corner are the world's own three.
    // See kFlagBrushShift in shaders/pt_material.glsl, which reads them.
    BrushX = 1 << 3,
    BrushY = 2 << 3,
    BrushZ = 3 << 3,

    // Bit 2 was CausticSource, "photons are traced through this". It is gone rather than
    // reserved, because it named a mechanism this renderer has ruled out: a photon pass is a
    // second traversal of the world, march is enormous, and the path tracer already carries as
    // many inlined copies of it as the driver will take. Caustics here come out of the radiance
    // bins, which already hold what arrives at a face along a direction — the same information
    // a photon map holds, keyed the way everything else in this renderer is keyed.
};

inline VisualFlags operator|(VisualFlags a, VisualFlags b) {
    return static_cast<VisualFlags>(static_cast<u8>(a) | static_cast<u8>(b));
}
inline bool has_flag(VisualFlags value, VisualFlags flag) {
    return (static_cast<u8>(value) & static_cast<u8>(flag)) != 0;
}

// Exactly 16 bytes. Every field is quantised on purpose: this record is fetched per
// shading point on a bandwidth-starved GPU, and the difference between 16 and 32 bytes
// is the difference between one cache line serving four voxel types and two.
struct VisualRecord {
    u8 red = 0;
    u8 green = 0;
    u8 blue = 0;
    u8 opacity = 255;        // 0 = fully transparent

    u8 roughness = 128;
    u8 metallic = 0;
    u8 ior = 0;              // 1.0 + ior/128, so 0 is vacuum and 255 is about 3.0
    u8 emissive = 0;         // scale; the tint is below

    u8 absorb_red = 0;       // Beer-Lambert, per metre
    u8 absorb_green = 0;
    u8 absorb_blue = 0;
    u8 translucency = 0;

    u16 emissive_tint = 0xFFFF;   // RGB565
    u8 flags = 0;                 // VisualFlags
    // Two lobes in one byte, four bits each: low nibble clearcoat strength, high nibble sheen.
    // Sixteen steps is plenty for either, because both are the *strength* of a lobe whose shape
    // is fixed and nobody can see the difference between a lacquer at 9/15 and one at 10/15.
    // Zero in both nibbles is what every record written before this holds, so every existing
    // world reads exactly as it did. See material_of in shaders/pt_material.glsl.
    u8 coat = 0;

    u64 content_hash() const;
    bool operator==(const VisualRecord&) const = default;
};
static_assert(sizeof(VisualRecord) == 16, "VisualRecord must stay 16 bytes");

struct BehaviourRecord {
    u32 material = 0;         // authoring identity and tag source
    u32 script = 0;           // optional Lua handle (answer C5)
    TagSet tags;
    PropertyMap properties;

    u64 content_hash() const;
    bool operator==(const BehaviourRecord& other) const;
};

struct VoxelType {
    VisualId visual = kAirVisual;
    BehaviourId behaviour = kAirBehaviour;
    bool operator==(const VoxelType&) const = default;
};

// Statistics the HUD shows, so a world that is genuinely accumulating unique voxels is
// visible before it becomes a memory problem rather than after.
struct VoxelTypeStats {
    u32 visual_records = 0;
    u32 behaviour_records = 0;
    u32 types = 0;
    u64 visual_bytes = 0;
    u64 behaviour_bytes = 0;
    u64 type_bytes = 0;
    u64 total_bytes() const { return visual_bytes + behaviour_bytes + type_bytes; }
    // How much interning saved: how many intern calls resolved to an existing record.
    u64 dedup_hits = 0;
    u64 intern_calls = 0;
    f64 dedup_rate() const {
        return (intern_calls > 0) ? static_cast<f64>(dedup_hits) / static_cast<f64>(intern_calls)
                                  : 0.0;
    }
};

class VoxelTypeTable {
public:
    VoxelTypeTable();

    VisualId intern_visual(const VisualRecord& record);
    BehaviourId intern_behaviour(const BehaviourRecord& record);
    VoxelTypeId intern(const VisualRecord& visual, const BehaviourRecord& behaviour);
    VoxelTypeId intern(VisualId visual, BehaviourId behaviour);

    const VisualRecord& visual(VisualId id) const;
    const BehaviourRecord& behaviour(BehaviourId id) const;
    const VoxelType& type(VoxelTypeId id) const;

    const VisualRecord& visual_of(VoxelTypeId id) const { return visual(type(id).visual); }
    const BehaviourRecord& behaviour_of(VoxelTypeId id) const {
        return behaviour(type(id).behaviour);
    }

    // Convenience for the places that only want one property of one voxel.
    PropertyValue property(const PropertyRegistry& registry, VoxelTypeId id,
                           PropertyId property_id) const;
    bool has_tag(VoxelTypeId id, TagId tag) const;

    u32 visual_count() const { return static_cast<u32>(visuals_.size()); }
    u32 behaviour_count() const { return static_cast<u32>(behaviours_.size()); }
    u32 type_count() const { return static_cast<u32>(types_.size()); }
    VoxelTypeStats stats() const;

    // Direct access for a bulk loader, which is the only caller that already knows the records
    // are distinct and in the right order.
    const std::vector<VisualRecord>& visuals() const { return visuals_; }
    const std::vector<BehaviourRecord>& behaviours() const { return behaviours_; }
    const std::vector<VoxelType>& types() const { return types_; }

    // Takes the whole table at once, replacing whatever was there.
    //
    // The dedup indexes are *not* rebuilt here, and that is the point of the method existing. A
    // million records re-interned one at a time is a million content hashes and a million map
    // inserts — half a second of work to reconstruct something a bulk load already knows. The
    // indexes are marked stale instead and rebuilt the first time something actually interns,
    // which for a loaded world is usually never: placing a material that already exists needs no
    // lookup, and only creating a *new* record does.
    void adopt(std::vector<VisualRecord> visuals, std::vector<BehaviourRecord> behaviours,
               std::vector<VoxelType> types);

private:
    void rebuild_indexes();

    std::vector<VisualRecord> visuals_;
    std::vector<BehaviourRecord> behaviours_;
    std::vector<VoxelType> types_;

    std::unordered_map<u64, std::vector<VisualId>> visual_index_;
    std::unordered_map<u64, std::vector<BehaviourId>> behaviour_index_;
    std::unordered_map<u64, VoxelTypeId> type_index_;
    bool indexes_stale_ = false;

    u64 dedup_hits_ = 0;
    u64 intern_calls_ = 0;
};

}  // namespace ws
