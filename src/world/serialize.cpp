#include "world/serialize.hpp"

#include <algorithm>
#include <cstring>

#include "core/log.hpp"
#include "world/brick.hpp"
#include "world/chunk.hpp"
#include "world/gpu_brick.hpp"
#include "world/property.hpp"
#include "world/tags.hpp"
#include "world/voxel_type.hpp"
#include "world/world.hpp"

namespace ws {
namespace {

constexpr u32 kMagic = 0x31575357u;   // "WSW1"
constexpr u32 kVersion = 1;

// Answer K2: breaking saves is acceptable during development, so the version is a hard
// gate rather than a migration path. Migration lands in Stage 22.

}  // namespace

void ByteWriter::u8v(u8 value) { data_.push_back(value); }

void ByteWriter::u16v(u16 value) {
    data_.push_back(static_cast<u8>(value & 0xFF));
    data_.push_back(static_cast<u8>((value >> 8) & 0xFF));
}

void ByteWriter::u32v(u32 value) {
    for (u32 i = 0; i < 4; ++i) data_.push_back(static_cast<u8>((value >> (i * 8)) & 0xFF));
}

void ByteWriter::u64v(u64 value) {
    for (u32 i = 0; i < 8; ++i) data_.push_back(static_cast<u8>((value >> (i * 8)) & 0xFF));
}

void ByteWriter::i64v(i64 value) { u64v(static_cast<u64>(value)); }

void ByteWriter::bytes(const void* data, usize size) {
    const u8* source = static_cast<const u8*>(data);
    data_.insert(data_.end(), source, source + size);
}

void ByteWriter::text(const std::string& value) {
    u32v(static_cast<u32>(value.size()));
    bytes(value.data(), value.size());
}

u8 ByteReader::u8v() {
    if (cursor_ + 1 > size_) { ok_ = false; return 0; }
    return data_[cursor_++];
}

u16 ByteReader::u16v() {
    const u8 low = u8v();
    const u8 high = u8v();
    return static_cast<u16>(low | (static_cast<u16>(high) << 8));
}

u32 ByteReader::u32v() {
    u32 value = 0;
    for (u32 i = 0; i < 4; ++i) value |= static_cast<u32>(u8v()) << (i * 8);
    return value;
}

u64 ByteReader::u64v() {
    u64 value = 0;
    for (u32 i = 0; i < 8; ++i) value |= static_cast<u64>(u8v()) << (i * 8);
    return value;
}

i64 ByteReader::i64v() { return static_cast<i64>(u64v()); }

bool ByteReader::bytes(void* out, usize size) {
    if (cursor_ + size > size_) { ok_ = false; return false; }
    std::memcpy(out, data_ + cursor_, size);
    cursor_ += size;
    return true;
}

std::string ByteReader::text() {
    const u32 length = u32v();
    if (!ok_ || cursor_ + length > size_) { ok_ = false; return {}; }
    std::string value(reinterpret_cast<const char*>(data_ + cursor_), length);
    cursor_ += length;
    return value;
}

// --------------------------------------------------------------------------------------
// Bricks
//
// Canonical form: the distinct types in first-seen voxel order, then indices packed at
// the narrowest width that addresses them. Independent of how the brick is stored in
// memory, which is what makes the round trip byte-identical.
// --------------------------------------------------------------------------------------

void write_brick(ByteWriter& out, const Brick& brick) {
    const u32 voxels = static_cast<u32>(kBrickVoxels);

    // Shared with the GPU encoder so the save format and the GPU format can never drift
    // into disagreeing about what a brick's canonical palette is.
    std::vector<VoxelTypeId> palette;
    std::vector<u32> slots;
    canonical_palette(brick, palette, slots);
    const u32 bits = index_bits_for(palette.size());

    out.u8v(static_cast<u8>(bits));
    out.u32v(static_cast<u32>(palette.size()));
    for (VoxelTypeId type : palette) out.u32v(type);

    if (bits == 0) return;   // uniform; the palette alone says everything

    if (bits == 32) {
        for (u32 voxel = 0; voxel < voxels; ++voxel) out.u32v(palette[slots[voxel]]);
        return;
    }

    const u32 per_byte = 8 / bits;
    for (u32 base = 0; base < voxels; base += per_byte) {
        u8 packed = 0;
        for (u32 i = 0; i < per_byte; ++i) {
            packed = static_cast<u8>(packed | ((slots[base + i] & ((1u << bits) - 1)) << (i * bits)));
        }
        out.u8v(packed);
    }
}

bool read_brick(ByteReader& in, Brick& brick) {
    const u32 voxels = static_cast<u32>(kBrickVoxels);
    const u8 bits = in.u8v();
    const u32 palette_size = in.u32v();
    if (!in.ok() || palette_size == 0 || palette_size > voxels) return false;

    std::vector<VoxelTypeId> palette(palette_size);
    for (VoxelTypeId& type : palette) type = in.u32v();
    if (!in.ok()) return false;

    if (bits == 0) {
        brick.fill(palette[0]);
        return true;
    }

    // Decoded into a plain array and handed over in one go.
    //
    // This used to call set() once per voxel, which re-tests the brick's form, searches its
    // palette and can re-pack all 512 indices — 512 times over, per brick. On a world whose
    // voxels are individually varied that is Direct-form bricks with 512-entry palettes, and the
    // search alone is quadratic: loading measured slower than rebuilding the world from its
    // source, which rather defeats the point of saving it.
    VoxelTypeId decoded[kBrickVoxels];

    if (bits == 32) {
        for (u32 voxel = 0; voxel < voxels; ++voxel) decoded[voxel] = in.u32v();
        if (!in.ok()) return false;
        brick.assign(decoded);
        return true;
    }

    if (bits != 1 && bits != 2 && bits != 4 && bits != 8) return false;

    const u32 per_byte = 8u / bits;
    const u32 mask = (1u << bits) - 1;
    for (u32 base = 0; base < voxels; base += per_byte) {
        const u8 packed = in.u8v();
        for (u32 i = 0; i < per_byte; ++i) {
            const u32 slot = (packed >> (i * bits)) & mask;
            if (slot >= palette_size) return false;
            decoded[base + i] = palette[slot];
        }
    }
    if (!in.ok()) return false;
    brick.assign(decoded);
    return true;
}

// --------------------------------------------------------------------------------------
// World
// --------------------------------------------------------------------------------------

void write_world(ByteWriter& out, const World& world) {
    const std::vector<ChunkCoord> coords = world.sorted_chunk_coords();

    // Count the chunks that will actually be written, so the reader does not have to
    // tolerate a mismatch.
    u32 written = 0;
    for (const ChunkCoord& coord : coords) {
        const Chunk* chunk = world.chunk(coord);
        if (chunk != nullptr && !chunk->empty()) ++written;
    }
    out.u32v(written);

    for (const ChunkCoord& coord : coords) {
        const Chunk* chunk = world.chunk(coord);
        if (chunk == nullptr || chunk->empty()) continue;

        out.i64v(coord.x);
        out.i64v(coord.y);
        out.i64v(coord.z);

        // Gather occupied bricks in a fixed order.
        std::vector<u16> occupied;
        const u32 axis = static_cast<u32>(kChunkBricks);
        for (u32 bz = 0; bz < axis; ++bz) {
            for (u32 by = 0; by < axis; ++by) {
                for (u32 bx = 0; bx < axis; ++bx) {
                    const Brick* brick = chunk->brick(bx, by, bz);
                    if (brick == nullptr || brick->empty()) continue;
                    occupied.push_back(static_cast<u16>((bx << 10) | (by << 5) | bz));
                }
            }
        }

        out.u32v(static_cast<u32>(occupied.size()));
        for (u16 key : occupied) {
            out.u16v(key);
            const u32 bx = (key >> 10) & 31u;
            const u32 by = (key >> 5) & 31u;
            const u32 bz = key & 31u;
            write_brick(out, *chunk->brick(bx, by, bz));
        }
    }
}

bool read_world(ByteReader& in, World& world) {
    const u32 chunk_count = in.u32v();
    if (!in.ok()) return false;

    for (u32 i = 0; i < chunk_count; ++i) {
        ChunkCoord coord;
        coord.x = in.i64v();
        coord.y = in.i64v();
        coord.z = in.i64v();
        const u32 brick_count = in.u32v();
        if (!in.ok()) return false;

        Chunk& chunk = world.chunk_for_write(coord);
        for (u32 b = 0; b < brick_count; ++b) {
            const u16 key = in.u16v();
            if (!in.ok()) return false;
            const u32 bx = (key >> 10) & 31u;
            const u32 by = (key >> 5) & 31u;
            const u32 bz = key & 31u;
            if (!read_brick(in, chunk.brick_for_write(bx, by, bz))) return false;
        }
    }
    return in.ok();
}

// --------------------------------------------------------------------------------------
// Whole save
// --------------------------------------------------------------------------------------

void write_save(ByteWriter& out, const WorldSave& save) {
    out.u32v(kMagic);
    out.u32v(kVersion);

    // Tags
    out.u32v(save.tags->count());
    for (u32 i = 0; i < save.tags->count(); ++i) {
        out.text(std::string(save.tags->name(i)));
    }

    // Properties
    out.u32v(save.properties->count());
    for (u32 i = 0; i < save.properties->count(); ++i) {
        const PropertyInfo& info = save.properties->info(i);
        out.text(info.name);
        out.u8v(static_cast<u8>(info.type));
        out.u8v(static_cast<u8>(info.domain));
        out.u64v(info.default_value.bits);
    }

    // Visual records
    out.u32v(save.types->visual_count());
    for (u32 i = 0; i < save.types->visual_count(); ++i) {
        const VisualRecord& record = save.types->visual(i);
        out.bytes(&record, sizeof(VisualRecord));
    }

    // Behaviour records
    out.u32v(save.types->behaviour_count());
    for (u32 i = 0; i < save.types->behaviour_count(); ++i) {
        const BehaviourRecord& record = save.types->behaviour(i);
        out.u32v(record.material);
        out.u32v(record.script);

        const u64* words = record.tags.fast_words();
        for (u32 w = 0; w < kFastTagWords; ++w) out.u64v(words[w]);
        out.u32v(static_cast<u32>(record.tags.overflow().size()));
        for (TagId tag : record.tags.overflow()) out.u32v(tag);

        out.u32v(static_cast<u32>(record.properties.size()));
        for (const PropertyMap::Entry& entry : record.properties.entries()) {
            out.u32v(entry.id);
            out.u64v(entry.value.bits);
        }
    }

    // Voxel types
    out.u32v(save.types->type_count());
    for (u32 i = 0; i < save.types->type_count(); ++i) {
        const VoxelType& type = save.types->type(i);
        out.u32v(type.visual);
        out.u32v(type.behaviour);
    }

    write_world(out, *save.world);
}

bool read_save(ByteReader& in, WorldSave& save) {
    if (in.u32v() != kMagic) {
        WS_LOG_ERROR("save", "not a WorldShaper world file");
        return false;
    }
    const u32 version = in.u32v();
    if (version != kVersion) {
        WS_LOG_ERROR("save", "world file is version {}, this build reads version {}",
                     version, kVersion);
        return false;
    }

    const u32 tag_count = in.u32v();
    for (u32 i = 0; i < tag_count; ++i) {
        const std::string name = in.text();
        if (!in.ok()) return false;
        const TagId id = save.tags->intern(name);
        if (id != i) {
            WS_LOG_ERROR("save", "tag '{}' loaded as id {} but the file says {}", name, id, i);
            return false;
        }
    }

    const u32 property_count = in.u32v();
    for (u32 i = 0; i < property_count; ++i) {
        const std::string name = in.text();
        const auto type = static_cast<PropertyType>(in.u8v());
        const auto domain = static_cast<PropertyDomain>(in.u8v());
        const PropertyValue default_value{in.u64v()};
        if (!in.ok()) return false;
        const PropertyId id = save.properties->define(name, type, domain, default_value);
        if (id != i) {
            WS_LOG_ERROR("save", "property '{}' loaded as id {} but the file says {}", name,
                         id, i);
            return false;
        }
    }

    const u32 visual_count = in.u32v();
    std::vector<VisualRecord> visuals(visual_count);
    for (VisualRecord& record : visuals) {
        if (!in.bytes(&record, sizeof(VisualRecord))) return false;
    }

    const u32 behaviour_count = in.u32v();
    std::vector<BehaviourRecord> behaviours(behaviour_count);
    for (BehaviourRecord& record : behaviours) {
        record.material = in.u32v();
        record.script = in.u32v();

        for (u32 w = 0; w < kFastTagWords; ++w) {
            const u64 word = in.u64v();
            for (u32 bit = 0; bit < 64; ++bit) {
                if ((word >> bit) & 1u) record.tags.add(w * 64 + bit);
            }
        }
        const u32 overflow_count = in.u32v();
        for (u32 i = 0; i < overflow_count; ++i) record.tags.add(in.u32v());

        const u32 property_entries = in.u32v();
        if (!in.ok()) return false;
        for (u32 i = 0; i < property_entries; ++i) {
            const PropertyId id = in.u32v();
            const PropertyValue value{in.u64v()};
            record.properties.set(id, value);
        }
        if (!in.ok()) return false;
    }

    const u32 type_count = in.u32v();
    if (!in.ok()) return false;
    for (u32 i = 0; i < type_count; ++i) {
        const u32 visual_index = in.u32v();
        const u32 behaviour_index = in.u32v();
        if (!in.ok() || visual_index >= visual_count || behaviour_index >= behaviour_count) {
            return false;
        }
        const VoxelTypeId id =
            save.types->intern(visuals[visual_index], behaviours[behaviour_index]);
        if (id != i) {
            WS_LOG_ERROR("save", "voxel type {} loaded as {}", i, id);
            return false;
        }
    }

    return read_world(in, *save.world);
}

}  // namespace ws
