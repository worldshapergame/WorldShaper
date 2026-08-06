#pragma once
// Serialisation.
//
// The property that matters is **byte-identical round trips**: save → load → save must
// produce the same bytes. It is asserted in CI (documentation/03-voxel-data-model.md §9,
// invariant 6) because without it, saving is a slow corruption: every load-and-save cycle
// would drift, and nobody would notice until a world would not open.
//
// That requires a canonical encoding rather than a dump of whatever is in memory. Bricks
// are written in a normalised palette form computed from their contents, so a brick that
// reached its state by carving and one that was built directly produce identical bytes.
//
// Everything is little-endian and explicit. Compression (zstd) wraps this in Stage 2; the
// container format with its append-only journal arrives in Stage 15.

#include <string>
#include <vector>

#include "core/types.hpp"

namespace ws {

class World;
class Brick;
class VoxelTypeTable;
class TagRegistry;
class PropertyRegistry;

class ByteWriter {
public:
    void u8v(u8 value);
    void u16v(u16 value);
    void u32v(u32 value);
    void u64v(u64 value);
    void i64v(i64 value);
    void bytes(const void* data, usize size);
    void text(const std::string& value);

    const std::vector<u8>& data() const { return data_; }
    usize size() const { return data_.size(); }

private:
    std::vector<u8> data_;
};

class ByteReader {
public:
    ByteReader(const u8* data, usize size) : data_(data), size_(size) {}
    explicit ByteReader(const std::vector<u8>& data) : data_(data.data()), size_(data.size()) {}

    u8 u8v();
    u16 u16v();
    u32 u32v();
    u64 u64v();
    i64 i64v();
    bool bytes(void* out, usize size);
    std::string text();

    bool ok() const { return ok_; }
    bool exhausted() const { return cursor_ >= size_; }
    usize remaining() const { return (cursor_ <= size_) ? size_ - cursor_ : 0; }

private:
    const u8* data_;
    usize size_;
    usize cursor_ = 0;
    bool ok_ = true;
};

// The whole of a world's persistent state. Held together in one struct so a save cannot
// accidentally omit the type table the voxels reference.
struct WorldSave {
    TagRegistry* tags = nullptr;
    PropertyRegistry* properties = nullptr;
    VoxelTypeTable* types = nullptr;
    World* world = nullptr;
};

void write_brick(ByteWriter& out, const Brick& brick);
bool read_brick(ByteReader& in, Brick& brick);

void write_world(ByteWriter& out, const World& world);
bool read_world(ByteReader& in, World& world);

void write_save(ByteWriter& out, const WorldSave& save);
bool read_save(ByteReader& in, WorldSave& save);

}  // namespace ws
