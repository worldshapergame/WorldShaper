#include "world/brick.hpp"

#include <algorithm>
#include <bit>
#include <cstring>

#include "core/assert.hpp"
#include "core/hash.hpp"

namespace ws {
namespace {

constexpr u32 kVoxels = static_cast<u32>(kBrickVoxels);

}  // namespace

Brick::Brick(VoxelTypeId type) { fill(type); }

u32 Brick::bits_for(Form form) {
    switch (form) {
        case Form::Uniform:  return 0;
        case Form::Palette1: return 1;
        case Form::Palette2: return 2;
        case Form::Palette4: return 4;
        case Form::Palette8: return 8;
        case Form::Direct:   return 32;
    }
    return 0;
}

void Brick::fill(VoxelTypeId type) {
    form_ = Form::Uniform;
    uniform_ = type;
    palette_.clear();
    palette_.shrink_to_fit();
    indices_.clear();
    indices_.shrink_to_fit();
    direct_.clear();
    direct_.shrink_to_fit();
    const u64 word = (type == kAir) ? 0ull : ~0ull;
    for (u64& value : occupancy_) value = word;
}

u32 Brick::read_index(u32 voxel) const {
    const u32 bits = bits_for(form_);
    switch (bits) {
        case 1: return (indices_[voxel >> 3] >> (voxel & 7u)) & 0x1u;
        case 2: return (indices_[voxel >> 2] >> ((voxel & 3u) * 2u)) & 0x3u;
        case 4: return (indices_[voxel >> 1] >> ((voxel & 1u) * 4u)) & 0xFu;
        case 8: return indices_[voxel];
        default: return 0;
    }
}

void Brick::write_index(u32 voxel, u32 slot) {
    const u32 bits = bits_for(form_);
    switch (bits) {
        case 1: {
            u8& byte = indices_[voxel >> 3];
            const u32 shift = voxel & 7u;
            byte = static_cast<u8>((byte & ~(0x1u << shift)) | ((slot & 0x1u) << shift));
            break;
        }
        case 2: {
            u8& byte = indices_[voxel >> 2];
            const u32 shift = (voxel & 3u) * 2u;
            byte = static_cast<u8>((byte & ~(0x3u << shift)) | ((slot & 0x3u) << shift));
            break;
        }
        case 4: {
            u8& byte = indices_[voxel >> 1];
            const u32 shift = (voxel & 1u) * 4u;
            byte = static_cast<u8>((byte & ~(0xFu << shift)) | ((slot & 0xFu) << shift));
            break;
        }
        case 8:
            indices_[voxel] = static_cast<u8>(slot);
            break;
        default:
            break;
    }
}

void Brick::materialise_from_uniform() {
    // Uniform → Palette1 with the old value in slot 0. Cheapest possible promotion, and
    // the occupancy mask is already correct.
    const VoxelTypeId old = uniform_;
    form_ = Form::Palette1;
    palette_.assign(1, old);
    indices_.assign(kVoxels / 8, 0);
}

void Brick::promote_to(Form form) {
    WS_ASSERT(form > form_, "promote_to only widens");
    if (form_ == Form::Uniform) materialise_from_uniform();
    if (form == form_) return;

    if (form == Form::Direct) {
        direct_.resize(kVoxels);
        for (u32 voxel = 0; voxel < kVoxels; ++voxel) {
            direct_[voxel] = palette_[read_index(voxel)];
        }
        form_ = Form::Direct;
        indices_.clear();
        indices_.shrink_to_fit();
        palette_.clear();
        palette_.shrink_to_fit();
        return;
    }

    // Re-pack the same slot numbers at a wider stride.
    std::vector<u32> slots(kVoxels);
    for (u32 voxel = 0; voxel < kVoxels; ++voxel) slots[voxel] = read_index(voxel);

    form_ = form;
    const u32 bits = bits_for(form_);
    indices_.assign((kVoxels * bits + 7) / 8, 0);
    for (u32 voxel = 0; voxel < kVoxels; ++voxel) write_index(voxel, slots[voxel]);
}

u32 Brick::palette_slot_for(VoxelTypeId type) {
    for (u32 slot = 0; slot < palette_.size(); ++slot) {
        if (palette_[slot] == type) return slot;
    }

    const u32 slot = static_cast<u32>(palette_.size());
    const u32 capacity = 1u << bits_for(form_);
    if (slot >= capacity) {
        // One more distinct type than this width can address.
        switch (form_) {
            case Form::Palette1: promote_to(Form::Palette2); break;
            case Form::Palette2: promote_to(Form::Palette4); break;
            case Form::Palette4: promote_to(Form::Palette8); break;
            case Form::Palette8: promote_to(Form::Direct); return 0;
            default: break;
        }
    }
    palette_.push_back(type);
    return slot;
}

VoxelTypeId Brick::get(u32 index) const {
    WS_ASSERT(index < kVoxels, "voxel index {} out of range", index);
    switch (form_) {
        case Form::Uniform: return uniform_;
        case Form::Direct:  return direct_[index];
        default:            return palette_[read_index(index)];
    }
}

bool Brick::set(u32 index, VoxelTypeId type) {
    WS_ASSERT(index < kVoxels, "voxel index {} out of range", index);

    if (form_ == Form::Uniform) {
        if (uniform_ == type) return false;
        materialise_from_uniform();
    }

    if (form_ == Form::Direct) {
        if (direct_[index] == type) return false;
        direct_[index] = type;
    } else {
        const u32 current = read_index(index);
        if (palette_[current] == type) return false;
        const u32 slot = palette_slot_for(type);
        if (form_ == Form::Direct) {
            // palette_slot_for promoted us mid-call; the promotion copied every voxel.
            direct_[index] = type;
        } else {
            write_index(index, slot);
        }
    }

    const u32 word = index / 64;
    const u64 bit = u64{1} << (index % 64);
    if (type == kAir) {
        occupancy_[word] &= ~bit;
    } else {
        occupancy_[word] |= bit;
    }
    return true;
}

bool Brick::solid(u32 index) const {
    WS_ASSERT(index < kVoxels, "voxel index {} out of range", index);
    return (occupancy_[index / 64] & (u64{1} << (index % 64))) != 0;
}

bool Brick::empty() const {
    for (u64 word : occupancy_) {
        if (word != 0) return false;
    }
    return true;
}

void Brick::occupancy(u64 out[kBrickWords]) const {
    std::memcpy(out, occupancy_, sizeof(occupancy_));
}

void Brick::decode(VoxelTypeId out[kBrickVoxels]) const {
    switch (form_) {
        case Form::Uniform:
            for (u32 voxel = 0; voxel < kVoxels; ++voxel) out[voxel] = uniform_;
            return;
        case Form::Direct:
            std::memcpy(out, direct_.data(), kVoxels * sizeof(VoxelTypeId));
            return;
        default:
            for (u32 voxel = 0; voxel < kVoxels; ++voxel) out[voxel] = palette_[read_index(voxel)];
            return;
    }
}

void Brick::assign(const VoxelTypeId in[kBrickVoxels]) {
    // Which distinct types are present, and which palette slot each voxel wants.
    //
    // Found through a small open-addressed table rather than a linear scan of the palette.
    // A scan is the obvious way and it is fine while bricks hold four or five materials, but
    // the no-two-voxels-alike rule makes 512-distinct bricks ordinary, and there the scan
    // costs 512 × 256 comparisons for a brick that this costs 512 probes.
    constexpr u32 kSlots = 1024;                 // power of two, twice the voxel count
    constexpr u32 kNoKey = 0xFFFFFFFFu;
    u32 probe_key[kSlots];
    u16 probe_slot[kSlots];
    std::memset(probe_key, 0xFF, sizeof(probe_key));

    VoxelTypeId used[kVoxels];
    u16 slot_of[kVoxels];
    u32 used_count = 0;

    for (u32 voxel = 0; voxel < kVoxels; ++voxel) {
        const u32 type = static_cast<u32>(in[voxel]);
        u32 at = (type * 0x9E3779B1u) & (kSlots - 1);
        while (probe_key[at] != kNoKey && probe_key[at] != type) at = (at + 1) & (kSlots - 1);
        if (probe_key[at] == kNoKey) {
            probe_key[at] = type;
            probe_slot[at] = static_cast<u16>(used_count);
            used[used_count++] = in[voxel];
        }
        slot_of[voxel] = probe_slot[at];
    }

    if (used_count == 1) {
        fill(used[0]);
        return;
    }

    palette_.clear();
    indices_.clear();
    direct_.clear();

    if (used_count > 256) {
        form_ = Form::Direct;
        palette_.shrink_to_fit();
        indices_.shrink_to_fit();
        direct_.assign(in, in + kVoxels);
    } else {
        form_ = (used_count <= 2)    ? Form::Palette1
                : (used_count <= 4)  ? Form::Palette2
                : (used_count <= 16) ? Form::Palette4
                                     : Form::Palette8;
        direct_.shrink_to_fit();
        palette_.assign(used, used + used_count);
        const u32 bits = bits_for(form_);
        indices_.assign((kVoxels * bits + 7) / 8, 0);
        // The form is already set, so write_index packs at the final stride and no voxel is
        // ever written twice.
        for (u32 voxel = 0; voxel < kVoxels; ++voxel) write_index(voxel, slot_of[voxel]);
    }

    for (u64& word : occupancy_) word = 0;
    for (u32 voxel = 0; voxel < kVoxels; ++voxel) {
        if (in[voxel] != kAir) occupancy_[voxel / 64] |= (u64{1} << (voxel % 64));
    }
}

void Brick::type_histogram(std::vector<TypeCount>& out) const {
    out.clear();
    if (form_ == Form::Uniform) {
        out.push_back({uniform_, static_cast<u32>(kBrickVoxels)});
        return;
    }

    if (form_ == Form::Direct) {
        // No palette to count against, so sort a copy and walk the runs. Direct bricks are
        // the rare case — every voxel a different type — and 512 entries sort in less time
        // than a linear search over the distinct set would take.
        std::vector<VoxelTypeId> sorted = direct_;
        std::sort(sorted.begin(), sorted.end());
        for (usize i = 0; i < sorted.size();) {
            usize j = i;
            while (j < sorted.size() && sorted[j] == sorted[i]) ++j;
            out.push_back({sorted[i], static_cast<u32>(j - i)});
            i = j;
        }
        return;
    }

    u32 counts[256]{};
    for (u32 voxel = 0; voxel < static_cast<u32>(kBrickVoxels); ++voxel) ++counts[read_index(voxel)];
    for (u32 slot = 0; slot < palette_.size(); ++slot) {
        if (counts[slot] != 0) out.push_back({palette_[slot], counts[slot]});
    }
}

u32 Brick::fill_range(u32 x0, u32 y0, u32 z0, u32 x1, u32 y1, u32 z1, VoxelTypeId type,
                      WriteMask mask, std::vector<TypeCount>& displaced) {
    displaced.clear();

    // One read per voxel to tally what is leaving. The distinct set inside a single brick
    // is tiny in practice, so a linear search beats any map.
    u32 changed = 0;
    VoxelTypeId decoded[kVoxels];
    const bool bulk = form_ != Form::Uniform;
    if (bulk) decode(decoded);
    for (u32 z = z0; z <= z1; ++z) {
        for (u32 y = y0; y <= y1; ++y) {
            for (u32 x = x0; x <= x1; ++x) {
                const VoxelTypeId before =
                    bulk ? decoded[brick_index(x, y, z)] : uniform_;
                if (before == type || !mask_allows(mask, before)) continue;
                ++changed;
                bool found = false;
                for (TypeCount& entry : displaced) {
                    if (entry.type == before) {
                        ++entry.count;
                        found = true;
                        break;
                    }
                }
                if (!found) displaced.push_back({before, 1});
            }
        }
    }
    if (changed == 0) return 0;

    // A masked write only collapses to a whole-brick fill when the mask let everything
    // through; otherwise the voxels it skipped have to survive.
    if (x0 == 0 && y0 == 0 && z0 == 0 && x1 == 7 && y1 == 7 && z1 == 7 &&
        (mask == WriteMask::All || changed == static_cast<u32>(kVoxels))) {
        fill(type);
        return changed;
    }

    if (form_ == Form::Uniform) materialise_from_uniform();
    // Resolved once. It may widen the encoding, which re-packs every voxel — doing that
    // inside the loop is what made the per-voxel path slow.
    u32 slot = (form_ == Form::Direct) ? 0 : palette_slot_for(type);
    const bool direct = form_ == Form::Direct;

    for (u32 z = z0; z <= z1; ++z) {
        for (u32 y = y0; y <= y1; ++y) {
            for (u32 x = x0; x <= x1; ++x) {
                const u32 index = brick_index(x, y, z);
                // The tally above read the brick before any of it moved, so the mask is
                // re-tested against that snapshot rather than against what we have already
                // written this pass.
                // (`decoded` is only populated for a brick that was not uniform. A uniform
                // one gives the same answer for every voxel, and changed > 0 above already
                // proved that answer was yes.)
                if (mask != WriteMask::All && bulk && !mask_allows(mask, decoded[index])) {
                    continue;
                }
                if (direct) {
                    direct_[index] = type;
                } else {
                    write_index(index, slot);
                }
                const u64 bit = u64{1} << (index % 64);
                if (type == kAir) {
                    occupancy_[index / 64] &= ~bit;
                } else {
                    occupancy_[index / 64] |= bit;
                }
            }
        }
    }
    return changed;
}

u32 Brick::solid_count() const {
    u32 total = 0;
    for (u64 word : occupancy_) total += static_cast<u32>(std::popcount(word));
    return total;
}

u32 Brick::index_bits() const { return bits_for(form_); }

u32 Brick::palette_size() const {
    switch (form_) {
        case Form::Uniform: return 1;
        case Form::Direct:  return 0;   // no palette; every voxel is stored outright
        default:            return static_cast<u32>(palette_.size());
    }
}

usize Brick::bytes() const {
    // What this brick actually costs in the pool, ignoring the object header that a
    // pooled allocation would not pay.
    switch (form_) {
        case Form::Uniform: return sizeof(VoxelTypeId) + sizeof(Form);
        case Form::Direct:
            return sizeof(occupancy_) + direct_.size() * sizeof(VoxelTypeId);
        default:
            return sizeof(occupancy_) + indices_.size() +
                   palette_.size() * sizeof(VoxelTypeId);
    }
}

void Brick::refresh_occupancy() {
    for (u64& word : occupancy_) word = 0;
    for (u32 voxel = 0; voxel < kVoxels; ++voxel) {
        if (get(voxel) != kAir) occupancy_[voxel / 64] |= (u64{1} << (voxel % 64));
    }
}

void Brick::compact() {
    if (form_ == Form::Uniform) return;

    // Collect the distinct types actually present.
    std::vector<VoxelTypeId> used;
    used.reserve(16);
    for (u32 voxel = 0; voxel < kVoxels; ++voxel) {
        const VoxelTypeId type = get(voxel);
        if (std::find(used.begin(), used.end(), type) == used.end()) {
            used.push_back(type);
            if (used.size() > 256) break;   // stays Direct
        }
    }

    if (used.size() == 1) {
        fill(used[0]);
        return;
    }

    if (used.size() > 256) {
        if (form_ != Form::Direct) promote_to(Form::Direct);
        refresh_occupancy();
        return;
    }

    Form target = Form::Palette8;
    if (used.size() <= 2) target = Form::Palette1;
    else if (used.size() <= 4) target = Form::Palette2;
    else if (used.size() <= 16) target = Form::Palette4;

    // Read every voxel before the layout changes underneath us.
    std::vector<VoxelTypeId> values(kVoxels);
    for (u32 voxel = 0; voxel < kVoxels; ++voxel) values[voxel] = get(voxel);

    form_ = target;
    direct_.clear();
    direct_.shrink_to_fit();
    palette_ = used;
    const u32 bits = bits_for(form_);
    indices_.assign((kVoxels * bits + 7) / 8, 0);
    indices_.shrink_to_fit();

    for (u32 voxel = 0; voxel < kVoxels; ++voxel) {
        const auto at = std::find(palette_.begin(), palette_.end(), values[voxel]);
        write_index(voxel, static_cast<u32>(at - palette_.begin()));
    }
    refresh_occupancy();
}

bool Brick::validate() const {
    // 1. Occupancy exactly matches the stored voxels.
    for (u32 voxel = 0; voxel < kVoxels; ++voxel) {
        const bool bit = (occupancy_[voxel / 64] & (u64{1} << (voxel % 64))) != 0;
        if (bit != (get(voxel) != kAir)) return false;
    }

    if (form_ == Form::Uniform) return palette_.empty() && indices_.empty() && direct_.empty();

    if (form_ == Form::Direct) {
        return direct_.size() == kVoxels && palette_.empty() && indices_.empty();
    }

    // 2. The palette has no duplicates.
    for (usize i = 0; i < palette_.size(); ++i) {
        for (usize k = i + 1; k < palette_.size(); ++k) {
            if (palette_[i] == palette_[k]) return false;
        }
    }

    // 3. The form is wide enough for the palette, and the index buffer is the right size.
    const u32 bits = bits_for(form_);
    if (palette_.size() > (usize{1} << bits)) return false;
    if (indices_.size() != (kVoxels * bits + 7) / 8) return false;

    // 4. No index points past the palette.
    for (u32 voxel = 0; voxel < kVoxels; ++voxel) {
        if (read_index(voxel) >= palette_.size()) return false;
    }
    return true;
}

u64 Brick::content_hash() const {
    // Contents only — never the encoding. Two bricks holding the same voxels must hash
    // the same whether one is uniform and the other has a palette, because this hash is
    // what network reconciliation and the save round-trip compare
    // (documentation/06-multiplayer.md §4).
    u64 h = 0x1E35A7BDull;
    for (u32 voxel = 0; voxel < kVoxels; ++voxel) h = hash_combine(h, get(voxel));
    return h;
}

u64 Brick::shape_hash() const {
    // The occupancy, and not one bit about what fills it. See the declaration for why the pair of
    // hashes exists: this one survives a run that re-interns its type table and the other does not.
    u64 words[kBrickWords];
    occupancy(words);
    u64 h = 0x9E3779B9ull;
    for (u32 i = 0; i < kBrickWords; ++i) h = hash_combine(h, words[i]);
    return h;
}

bool Brick::operator==(const Brick& other) const {
    // Compares contents, not encoding: two bricks holding the same voxels are equal even
    // if one is uniform and the other has a palette. That is the property the save
    // round-trip test needs.
    for (u32 voxel = 0; voxel < kVoxels; ++voxel) {
        if (get(voxel) != other.get(voxel)) return false;
    }
    return true;
}

}  // namespace ws
