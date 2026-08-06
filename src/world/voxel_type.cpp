#include "world/voxel_type.hpp"

#include <cstring>

#include "core/assert.hpp"
#include "core/hash.hpp"

namespace ws {

u64 VisualRecord::content_hash() const {
    u64 words[2];
    std::memcpy(words, this, sizeof(VisualRecord));
    return hash_combine(hash_mix(words[0]), words[1]);
}

u64 BehaviourRecord::content_hash() const {
    u64 h = hash_mix(material);
    h = hash_combine(h, script);
    h = hash_combine(h, tags.content_hash());
    return hash_combine(h, properties.content_hash());
}

bool BehaviourRecord::operator==(const BehaviourRecord& other) const {
    return material == other.material && script == other.script && tags == other.tags &&
           properties == other.properties;
}

VoxelTypeTable::VoxelTypeTable() {
    // Air is id 0 in every table, so an empty brick is a run of zeroes and a zeroed
    // buffer is a valid empty world.
    VisualRecord air_visual{};
    air_visual.red = 0;
    air_visual.green = 0;
    air_visual.blue = 0;
    air_visual.opacity = 0;
    air_visual.emissive_tint = 0;

    BehaviourRecord air_behaviour{};
    air_behaviour.tags.add(0);  // "air" is the first tag TagRegistry registers

    const VisualId visual = intern_visual(air_visual);
    const BehaviourId behaviour = intern_behaviour(air_behaviour);
    const VoxelTypeId air = intern(visual, behaviour);

    WS_CHECK(visual == kAirVisual && behaviour == kAirBehaviour && air == kAir,
             "air must be id 0 in every table");
}

VisualId VoxelTypeTable::intern_visual(const VisualRecord& record) {
    ++intern_calls_;
    const u64 hash = record.content_hash();
    std::vector<VisualId>& bucket = visual_index_[hash];
    for (VisualId candidate : bucket) {
        if (visuals_[candidate] == record) {
            ++dedup_hits_;
            return candidate;
        }
    }
    const VisualId id = static_cast<VisualId>(visuals_.size());
    visuals_.push_back(record);
    bucket.push_back(id);
    return id;
}

BehaviourId VoxelTypeTable::intern_behaviour(const BehaviourRecord& record) {
    ++intern_calls_;
    const u64 hash = record.content_hash();
    std::vector<BehaviourId>& bucket = behaviour_index_[hash];
    for (BehaviourId candidate : bucket) {
        if (behaviours_[candidate] == record) {
            ++dedup_hits_;
            return candidate;
        }
    }
    const BehaviourId id = static_cast<BehaviourId>(behaviours_.size());
    behaviours_.push_back(record);
    bucket.push_back(id);
    return id;
}

VoxelTypeId VoxelTypeTable::intern(VisualId visual, BehaviourId behaviour) {
    WS_ASSERT(visual < visuals_.size(), "unknown visual record");
    WS_ASSERT(behaviour < behaviours_.size(), "unknown behaviour record");
    ++intern_calls_;

    const u64 key = (static_cast<u64>(visual) << 32) | behaviour;
    const auto found = type_index_.find(key);
    if (found != type_index_.end()) {
        ++dedup_hits_;
        return found->second;
    }
    const VoxelTypeId id = static_cast<VoxelTypeId>(types_.size());
    types_.push_back(VoxelType{visual, behaviour});
    type_index_.emplace(key, id);
    return id;
}

VoxelTypeId VoxelTypeTable::intern(const VisualRecord& visual,
                                   const BehaviourRecord& behaviour) {
    return intern(intern_visual(visual), intern_behaviour(behaviour));
}

const VisualRecord& VoxelTypeTable::visual(VisualId id) const {
    WS_ASSERT(id < visuals_.size(), "unknown visual record {}", id);
    return visuals_[id < visuals_.size() ? id : 0];
}

const BehaviourRecord& VoxelTypeTable::behaviour(BehaviourId id) const {
    WS_ASSERT(id < behaviours_.size(), "unknown behaviour record {}", id);
    return behaviours_[id < behaviours_.size() ? id : 0];
}

const VoxelType& VoxelTypeTable::type(VoxelTypeId id) const {
    WS_ASSERT(id < types_.size(), "unknown voxel type {}", id);
    return types_[id < types_.size() ? id : 0];
}

PropertyValue VoxelTypeTable::property(const PropertyRegistry& registry, VoxelTypeId id,
                                       PropertyId property_id) const {
    return behaviour_of(id).properties.get(registry, property_id);
}

bool VoxelTypeTable::has_tag(VoxelTypeId id, TagId tag) const {
    return behaviour_of(id).tags.has(tag);
}

VoxelTypeStats VoxelTypeTable::stats() const {
    VoxelTypeStats out;
    out.visual_records = static_cast<u32>(visuals_.size());
    out.behaviour_records = static_cast<u32>(behaviours_.size());
    out.types = static_cast<u32>(types_.size());
    out.visual_bytes = visuals_.size() * sizeof(VisualRecord);
    out.type_bytes = types_.size() * sizeof(VoxelType);

    u64 behaviour_bytes = 0;
    for (const BehaviourRecord& record : behaviours_) {
        behaviour_bytes += sizeof(BehaviourRecord);
        behaviour_bytes += record.tags.overflow().size() * sizeof(TagId);
        behaviour_bytes += record.properties.size() * sizeof(PropertyMap::Entry);
    }
    out.behaviour_bytes = behaviour_bytes;
    out.dedup_hits = dedup_hits_;
    out.intern_calls = intern_calls_;
    return out;
}

}  // namespace ws
